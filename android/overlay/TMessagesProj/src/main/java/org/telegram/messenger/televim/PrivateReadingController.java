package org.telegram.messenger.televim;

import android.content.Context;

import org.telegram.SQLite.SQLiteDatabase;
import org.telegram.messenger.ApplicationLoader;
import org.telegram.messenger.ChatObject;
import org.telegram.messenger.DialogObject;
import org.telegram.messenger.MessageObject;
import org.telegram.messenger.MessagesController;
import org.telegram.messenger.MessagesStorage;
import org.telegram.messenger.UserConfig;
import org.telegram.tgnet.ConnectionsManager;

import java.util.concurrent.Executor;
import java.util.concurrent.atomic.AtomicBoolean;

/**
 * Account-owned adapter for ordinary read and content receipt entry points.
 * Native callers retain their local and transport pipelines; this adapter
 * captures their owner and schedules cancellation when that owner's requests
 * become invalid, without resolving account singletons during teardown.
 */
public final class PrivateReadingController {
    private static final Object LOCK = new Object();
    private static final State[] STATES = new State[UserConfig.MAX_ACCOUNT_COUNT];
    private static final Lifetime[] LIFETIMES = new Lifetime[UserConfig.MAX_ACCOUNT_COUNT];

    private PrivateReadingController() {
    }

    public static boolean blocksHistory(int account, long dialogId) {
        return blocksHistory(account, dialogId, 0, 0);
    }

    public static boolean blocksHistory(int account, long dialogId, long rootId, long subpeerId) {
        State state = state(account);
        if (state == null) {
            return validAccount(account);
        }
        return state.blocks(conversation(state, dialogId, rootId, subpeerId));
    }

    public static boolean blocksContent(int account, long dialogId, int messageId) {
        return blocksContent(account, dialogId, 0, 0, messageId);
    }

    public static boolean blocksContent(int account, long dialogId, long rootId, long subpeerId, int messageId) {
        State state = state(account);
        if (state == null) {
            return validAccount(account);
        }
        return state.blocksContent(conversation(state, dialogId, rootId, subpeerId), messageId);
    }

    public static boolean isProtected(int account, long dialogId) {
        return blocksHistory(account, dialogId);
    }

    public static PrivateReadingPolicy.Status status(int account) {
        State state = state(account);
        return state == null ? null : state.policy.status();
    }

    public static boolean saveSelection(int account, PrivateReadingPolicy.Identity expectedIdentity,
                                        boolean master, boolean privateChats, boolean groups, boolean channels, boolean bots) {
        State state = state(account);
        if (state == null || state.identity != expectedIdentity || !state.policy.status().loaded) {
            return false;
        }
        boolean scheduled = state.settings.requestSave(new PrivateReadingPolicy.Selection(
                master, privateChats, groups, channels, bots));
        state.cancelRequests();
        return scheduled;
    }

    public static AccountHandle captureAccount(int account) {
        State state = state(account);
        return state == null ? null : new AccountHandle(state);
    }

    /**
     * UserConfig publishes these transitions while holding its own lock.
     * Nothing under LOCK may acquire UserConfig or resolve native singletons.
     * Initial load publishes only once; only explicit authentication may replace
     * a logout tombstone. Profile callbacks cannot prove a new account lifetime.
     * Lifetime object identity also rejects state creation begun before logout.
     */
    public static void accountLoaded(int account, long userId) {
        if (!validAccount(account)) {
            return;
        }
        synchronized (LOCK) {
            if (LIFETIMES[account] == null) {
                LIFETIMES[account] = new Lifetime(userId);
            }
        }
    }

    public static void accountActivated(int account, long userId) {
        if (!validAccount(account) || userId <= 0) {
            return;
        }
        synchronized (LOCK) {
            replaceLifetime(account, userId);
        }
    }

    public static void currentUserChanged(int account, long userId) {
        if (!validAccount(account)) {
            return;
        }
        synchronized (LOCK) {
            Lifetime lifetime = LIFETIMES[account];
            if (lifetime != null && lifetime.userId > 0 && lifetime.userId != userId) {
                replaceLifetime(account, 0);
            }
        }
    }

    public static void invalidate(int account) {
        if (!validAccount(account)) {
            return;
        }
        synchronized (LOCK) {
            Lifetime lifetime = LIFETIMES[account];
            if (lifetime == null || lifetime.userId > 0) {
                replaceLifetime(account, 0);
            }
        }
    }

    private static void replaceLifetime(int account, long userId) {
        State state = STATES[account];
        STATES[account] = null;
        LIFETIMES[account] = new Lifetime(userId);
        if (state != null) {
            state.close();
        }
    }

    private static boolean validAccount(int account) {
        return account >= 0 && account < STATES.length;
    }

    private static State state(int account) {
        if (!validAccount(account) || ApplicationLoader.applicationContext == null) {
            return null;
        }
        Lifetime lifetime;
        synchronized (LOCK) {
            lifetime = LIFETIMES[account];
            if (lifetime == null || lifetime.userId <= 0) {
                return null;
            }
        }
        ConnectionsManager connections = ConnectionsManager.getInstance(account);
        MessagesStorage storage = MessagesStorage.getInstance(account);
        boolean test = connections.isTestBackend();
        PrivateReadingPolicy.Environment environment = test
                ? PrivateReadingPolicy.Environment.TEST
                : PrivateReadingPolicy.Environment.PRODUCTION;
        synchronized (LOCK) {
            if (LIFETIMES[account] != lifetime) {
                return null;
            }
            State current = STATES[account];
            if (current != null && current.environment == environment) {
                return current;
            }
            if (current != null) {
                current.close();
            }
            current = new State(account, lifetime, environment, connections, storage);
            STATES[account] = current;
            current.settings.load();
            return current;
        }
    }

    /**
     * Capture at the originating action and retain through its asynchronous work.
     * The policy and identity always belong to that owner, even after logout, so
     * cancellation and completion can settle against the original policy.
     * isCurrent is a snapshot, not a lock around a later native side effect;
     * storage reservations and transport handoff still need their own fences.
     */
    public static final class AccountHandle {
        private final State owner;
        private final SQLiteDatabase database;

        private AccountHandle(State owner) {
            this.owner = owner;
            database = owner.storage.getDatabase();
        }

        public MessagesStorage storage() {
            return owner.storage;
        }

        public SQLiteDatabase database() {
            return database;
        }

        public int account() {
            return owner.account;
        }

        public PrivateReadingPolicy policy() {
            return owner.policy;
        }

        public PrivateReadingPolicy.Identity identity() {
            return owner.identity;
        }

        public PrivateReadingPolicy.Conversation conversation(long dialogId, long rootId, long subpeerId) {
            if (!isCurrent()) {
                return null;
            }
            PrivateReadingPolicy.Conversation result = PrivateReadingController.conversation(
                    owner, dialogId, rootId, subpeerId);
            return isCurrent() ? result : null;
        }

        public boolean blocksHistory(long dialogId, long rootId, long subpeerId) {
            PrivateReadingPolicy.Conversation conversation = conversation(dialogId, rootId, subpeerId);
            return !isCurrent() || owner.blocks(conversation);
        }

        public boolean blocksContent(long dialogId, long rootId, long subpeerId, int messageId) {
            PrivateReadingPolicy.Conversation conversation = conversation(dialogId, rootId, subpeerId);
            return !isCurrent() || owner.blocksContent(conversation, messageId);
        }

        public PrivateReadingPolicy.Conversation conversation(MessageObject message) {
            if (message == null || message.messageOwner == null || message.scheduled
                    || message.currentAccount != owner.account || !isCurrent()) {
                return null;
            }
            long dialogId = message.getDialogId();
            if (dialogId == owner.userId || DialogObject.isEncryptedDialog(dialogId)) {
                return conversation(dialogId, 0, 0);
            }
            int forumFlags = owner.storage.getForumTypeFlags(dialogId);
            boolean monoForum = (forumFlags & MessagesStorage.FORUM_TYPE_DIRECT) != 0;
            long topicId = MessageObject.getTopicId(owner.account, message.messageOwner, forumFlags);
            long rootId = topicId == 0 && (monoForum || message.messageOwner.isThreadMessage)
                    ? -1 : monoForum ? 0 : topicId;
            return conversation(dialogId, rootId, monoForum ? topicId : 0);
        }

        public boolean blocksContent(MessageObject message) {
            if (message == null || message.messageOwner == null || message.scheduled
                    || message.currentAccount != owner.account || !isCurrent()) {
                return true;
            }
            PrivateReadingPolicy.Conversation conversation = conversation(message);
            return !isCurrent() || owner.blocksContent(conversation, message.getId());
        }

        public boolean isCurrent() {
            synchronized (LOCK) {
                return STATES[owner.account] == owner
                        && LIFETIMES[owner.account] == owner.lifetime
                        && owner.policy.owns(owner.identity);
            }
        }
    }

    private static final class Lifetime {
        final long userId;

        Lifetime(long userId) {
            this.userId = userId;
        }
    }

    private static PrivateReadingPolicy.Conversation conversation(State state, long dialogId, long rootId, long subpeerId) {
        int account = state.account;
        long userId = state.userId;
        if (DialogObject.isEncryptedDialog(dialogId)) {
            return new PrivateReadingPolicy.Conversation(dialogId, 0, 0,
                    PrivateReadingPolicy.Category.SECRET);
        }
        if (dialogId == 0 || rootId < 0 || (rootId != 0 && subpeerId != 0)) {
            return null;
        }
        if (dialogId == userId) {
            return new PrivateReadingPolicy.Conversation(dialogId, rootId, subpeerId, PrivateReadingPolicy.Category.SELF);
        }
        if (dialogId > 0) {
            org.telegram.tgnet.TLRPC.User user = MessagesController.getInstance(account).getUser(dialogId);
            PrivateReadingPolicy.Category category = user == null || user.min
                    ? PrivateReadingPolicy.Category.UNKNOWN_PRIVATE_OR_BOT
                    : user.bot ? PrivateReadingPolicy.Category.BOT : PrivateReadingPolicy.Category.PRIVATE;
            return new PrivateReadingPolicy.Conversation(dialogId, rootId, subpeerId, category);
        }
        long chatId = -dialogId;
        org.telegram.tgnet.TLRPC.Chat chat = MessagesController.getInstance(account).getChat(chatId);
        boolean known = chat != null;
        boolean broadcast = known && ChatObject.isChannelAndNotMegaGroup(chat);
        PrivateReadingPolicy.Category category = !known
                ? PrivateReadingPolicy.Category.UNKNOWN_GROUP_OR_CHANNEL
                : broadcast ? PrivateReadingPolicy.Category.CHANNEL : PrivateReadingPolicy.Category.GROUP;
        return new PrivateReadingPolicy.Conversation(dialogId, rootId, subpeerId, category);
    }

    private static final class State {
        final int account;
        final long userId;
        final Lifetime lifetime;
        final PrivateReadingPolicy.Environment environment;
        final PrivateReadingPolicy policy;
        final PrivateReadingPolicy.Identity identity;
        final PrivateReadingSettings settings;
        final ConnectionsManager connections;
        final MessagesStorage storage;
        final AtomicBoolean cancellationScheduled = new AtomicBoolean();

        State(int account, Lifetime lifetime, PrivateReadingPolicy.Environment environment,
              ConnectionsManager connections, MessagesStorage storage) {
            this.account = account;
            this.lifetime = lifetime;
            this.userId = lifetime.userId;
            this.environment = environment;
            this.connections = connections;
            this.storage = storage;
            policy = new PrivateReadingPolicy(environment, userId);
            identity = policy.identity();
            Context context = ApplicationLoader.applicationContext;
            Executor executor = runnable -> org.telegram.messenger.Utilities.globalQueue.postRunnable(runnable);
            settings = new PrivateReadingSettings(context.getNoBackupFilesDir(), policy, executor);
        }

        boolean blocks(PrivateReadingPolicy.Conversation conversation) {
            PrivateReadingPolicy.ReceiptDecision decision = conversation == null
                    ? policy.decisionForUnknownConversation(identity) : policy.decision(identity, conversation);
            cancelRequests();
            return decision != PrivateReadingPolicy.ReceiptDecision.NATIVE;
        }

        boolean blocksContent(PrivateReadingPolicy.Conversation conversation, int messageId) {
            if (conversation == null) {
                return blocks(null);
            }
            PrivateReadingPolicy.ReceiptDecision decision = policy.decision(identity, conversation);
            cancelRequests();
            if (decision == PrivateReadingPolicy.ReceiptDecision.NATIVE) {
                return false;
            }
            policy.observeContent(identity, conversation, messageId);
            return true;
        }

        void cancelRequests() {
            if (policy.cancellationAt(0) == null || !cancellationScheduled.compareAndSet(false, true)) {
                return;
            }
            AccountHandle handle = new AccountHandle(this);
            Runnable cancellation = () -> {
                cancellationScheduled.set(false);
                connections.cancelPrivateReadingRequests(handle);
            };
            boolean posted;
            try {
                posted = org.telegram.messenger.Utilities.stageQueue.postRunnable(cancellation);
            } catch (RuntimeException error) {
                posted = false;
            }
            if (!posted) {
                cancellation.run();
            }
        }

        void close() {
            settings.close();
            cancelRequests();
        }
    }
}
