package org.telegram.messenger.televim;

import android.content.Context;

import org.telegram.messenger.AccountInstance;
import org.telegram.messenger.ApplicationLoader;
import org.telegram.messenger.ChatObject;
import org.telegram.messenger.DialogObject;
import org.telegram.messenger.MessagesController;
import org.telegram.messenger.UserConfig;
import org.telegram.tgnet.ConnectionsManager;

import java.io.File;
import java.util.concurrent.Executor;

/**
 * Account-owned adapter for ordinary read and content receipt entry points.
 * It deliberately has no network path: native callers decide whether to enter
 * their existing local and transport work after this gate returns.
 */
public final class PrivateReadingController {
    private static final Object LOCK = new Object();
    private static final State[] STATES = new State[UserConfig.MAX_ACCOUNT_COUNT];

    private PrivateReadingController() {
    }

    public static boolean blocksHistory(int account, long dialogId) {
        State state = state(account);
        if (state == null) {
            return false;
        }
        return state.blocks(conversation(account, dialogId));
    }

    public static boolean blocksContent(int account, long dialogId, int messageId) {
        State state = state(account);
        if (state == null) {
            return false;
        }
        PrivateReadingPolicy.Conversation conversation = conversation(account, dialogId);
        PrivateReadingPolicy.ReceiptDecision decision = state.policy.decision(
                state.identity,
                conversation);
        if (decision == PrivateReadingPolicy.ReceiptDecision.NATIVE) {
            return false;
        }
        state.policy.observeContent(state.identity, conversation, messageId);
        return true;
    }

    public static boolean isProtected(int account, long dialogId) {
        return blocksHistory(account, dialogId);
    }

    public static PrivateReadingPolicy.Status status(int account) {
        State state = state(account);
        return state == null ? null : state.policy.status();
    }

    public static boolean saveSelection(int account, boolean master, boolean privateChats,
                                        boolean groups, boolean channels) {
        State state = state(account);
        return state != null && state.settings.requestSave(new PrivateReadingPolicy.Selection(
                master, privateChats, groups, channels));
    }

    public static void invalidate(int account) {
        synchronized (LOCK) {
            State state = STATES[account];
            STATES[account] = null;
            if (state != null) {
                state.close();
            }
        }
    }

    private static State state(int account) {
        if (account < 0 || account >= STATES.length || ApplicationLoader.applicationContext == null) {
            return null;
        }
        long userId = UserConfig.getInstance(account).getClientUserId();
        if (userId <= 0 || !UserConfig.getInstance(account).isClientActivated()) {
            invalidate(account);
            return null;
        }
        boolean test = ConnectionsManager.getInstance(account).isTestBackend();
        PrivateReadingPolicy.Environment environment = test
                ? PrivateReadingPolicy.Environment.TEST
                : PrivateReadingPolicy.Environment.PRODUCTION;
        synchronized (LOCK) {
            State current = STATES[account];
            if (current != null && current.userId == userId && current.environment == environment) {
                return current;
            }
            if (current != null) {
                current.close();
            }
            current = new State(account, userId, environment);
            STATES[account] = current;
            current.settings.load();
            return current;
        }
    }

    private static PrivateReadingPolicy.Conversation conversation(int account, long dialogId) {
        long userId = UserConfig.getInstance(account).getClientUserId();
        if (DialogObject.isEncryptedDialog(dialogId)) {
            return new PrivateReadingPolicy.Conversation(dialogId, 0, 0,
                    PrivateReadingPolicy.Category.SECRET);
        }
        if (dialogId == 0 || dialogId == userId) {
            return new PrivateReadingPolicy.Conversation(dialogId == 0 ? userId : dialogId,
                    0, 0, PrivateReadingPolicy.Category.SELF);
        }
        if (dialogId > 0) {
            return new PrivateReadingPolicy.Conversation(dialogId, 0, 0,
                    PrivateReadingPolicy.Category.PRIVATE);
        }
        long chatId = -dialogId;
        org.telegram.tgnet.TLRPC.Chat chat = MessagesController.getInstance(account).getChat(chatId);
        boolean known = chat != null;
        boolean broadcast = known && ChatObject.isChannelAndNotMegaGroup(chat);
        PrivateReadingPolicy.Category category = !known
                ? PrivateReadingPolicy.Category.UNKNOWN_GROUP_OR_CHANNEL
                : broadcast ? PrivateReadingPolicy.Category.CHANNEL : PrivateReadingPolicy.Category.GROUP;
        return new PrivateReadingPolicy.Conversation(dialogId, 0, 0, category);
    }

    private static final class State {
        final int account;
        final long userId;
        final PrivateReadingPolicy.Environment environment;
        final PrivateReadingPolicy policy;
        final PrivateReadingPolicy.Identity identity;
        final PrivateReadingSettings settings;

        State(int account, long userId, PrivateReadingPolicy.Environment environment) {
            this.account = account;
            this.userId = userId;
            this.environment = environment;
            policy = new PrivateReadingPolicy(environment, userId);
            identity = policy.identity();
            Context context = ApplicationLoader.applicationContext;
            Executor executor = runnable -> org.telegram.messenger.Utilities.globalQueue.postRunnable(runnable);
            settings = new PrivateReadingSettings(context.getNoBackupFilesDir(), policy, executor);
        }

        boolean blocks(PrivateReadingPolicy.Conversation conversation) {
            return policy.decision(policy.identity(), conversation)
                    != PrivateReadingPolicy.ReceiptDecision.NATIVE;
        }

        void close() {
            settings.close();
            policy.invalidate(policy.identity());
        }
    }
}
