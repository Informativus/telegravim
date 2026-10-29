package org.telegram.messenger.televim;

import android.widget.Toast;

import org.telegram.messenger.AndroidUtilities;
import org.telegram.messenger.ApplicationLoader;
import org.telegram.messenger.DialogObject;
import org.telegram.messenger.LocaleController;
import org.telegram.messenger.MessagesController;
import org.telegram.messenger.R;
import org.telegram.messenger.UserConfig;
import org.telegram.messenger.Utilities;
import org.telegram.tgnet.ConnectionsManager;
import org.telegram.tgnet.TLObject;
import org.telegram.tgnet.TLRPC;
import org.telegram.ui.ActionBar.BaseFragment;

import java.lang.ref.WeakReference;
import java.util.ArrayList;
import java.util.concurrent.atomic.AtomicBoolean;

import org.telegram.messenger.televim.PrivateReadingController.AccountHandle;
import org.telegram.messenger.televim.PrivateReadingPolicy.Conversation;
import org.telegram.messenger.televim.PrivateReadingPolicy.ReceiptDecision;
import org.telegram.messenger.televim.PrivateReadingPolicy.ReleaseScope;
import org.telegram.messenger.televim.PrivateReadingPolicy.RequestToken;

public final class PrivateReadingRelease {
    private PrivateReadingRelease() {
    }

    public static Batch capture(BaseFragment fragment, MessagesController messages, ConnectionsManager connections) {
        return new Batch(fragment, messages, connections);
    }

    public static boolean markDialog(BaseFragment fragment, MessagesController messages,
                                     ConnectionsManager connections, TLRPC.Dialog dialog) {
        return capture(fragment, messages, connections).markDialog(dialog);
    }

    public static boolean markTopic(BaseFragment fragment, MessagesController messages,
                                    ConnectionsManager connections, long dialogId, TLRPC.TL_forumTopic topic) {
        return capture(fragment, messages, connections).markTopic(dialogId, topic);
    }

    public static final class Batch {
        private final AccountHandle owner;
        private final MessagesController messages;
        private final ConnectionsManager connections;
        private final Feedback feedback;
        private int attemptedScopes;

        private Batch(BaseFragment fragment, MessagesController messages, ConnectionsManager connections) {
            owner = PrivateReadingController.captureAccount(fragment.getCurrentAccount());
            this.messages = messages;
            this.connections = connections;
            feedback = new Feedback(fragment, owner);
        }

        public boolean markDialog(TLRPC.Dialog dialog) {
            Conversation conversation = owner == null || dialog == null ? null
                    : owner.conversation(dialog.id, 0, 0);
            if (nativeAction(owner, conversation)) {
                return false;
            }
            if (conversation == null || !validCutoff(dialog.top_message)
                    || attemptedScopes >= PrivateReadingPolicy.MAX_RELEASES) {
                feedback.show(R.string.TelevimReadScopePending);
                return true;
            }
            final int cutoff = dialog.top_message;
            if (messages.isForum(dialog.id) || messages.isMonoForum(dialog.id)) {
                feedback.show(R.string.TelevimReadScopePending);
                ArrayList<TLRPC.TL_forumTopic> topics = messages.getTopicsController().getTopics(-dialog.id);
                if (topics != null) {
                    int count = Math.min(topics.size(), PrivateReadingPolicy.MAX_RELEASES);
                    for (int index = 0; index < count; ++index) {
                        TLRPC.TL_forumTopic topic = topics.get(index);
                        if (topic != null && validCutoff(topic.top_message)) {
                            release(topicConversation(owner, messages, dialog.id, topic),
                                    Math.min(cutoff, topic.top_message));
                        }
                    }
                }
            } else {
                release(conversation, cutoff);
            }
            return true;
        }

        public boolean markTopic(long dialogId, TLRPC.TL_forumTopic topic) {
            Conversation conversation = owner == null || topic == null ? null
                    : topicConversation(owner, messages, dialogId, topic);
            if (nativeAction(owner, conversation)) {
                return false;
            }
            if (conversation == null || !validCutoff(topic.top_message)) {
                feedback.show(R.string.TelevimReadScopePending);
            } else {
                release(conversation, topic.top_message);
            }
            return true;
        }

        private void release(Conversation conversation, int cutoff) {
            if (attemptedScopes >= PrivateReadingPolicy.MAX_RELEASES) {
                feedback.show(R.string.TelevimReadScopePending);
                return;
            }
            ++attemptedScopes;
            start(owner, messages, connections, conversation, cutoff, feedback);
        }
    }

    private static boolean nativeAction(AccountHandle owner, Conversation conversation) {
        return owner != null && owner.isCurrent() && conversation != null
                && owner.policy().decision(owner.identity(), conversation) == ReceiptDecision.NATIVE;
    }

    private static boolean validCutoff(int cutoff) {
        return cutoff > 0 && cutoff < Integer.MAX_VALUE;
    }

    private static Conversation topicConversation(AccountHandle owner, MessagesController messages,
                                                   long dialogId, TLRPC.TL_forumTopic topic) {
        if (messages.isMonoForum(dialogId)) {
            long subpeer = topic.from_id == null ? 0 : DialogObject.getPeerDialogId(topic.from_id);
            return subpeer == 0 ? null : owner.conversation(dialogId, 0, subpeer);
        }
        return topic.id <= 0 ? null : owner.conversation(dialogId, topic.id, 0);
    }

    private static void start(AccountHandle owner, MessagesController messages, ConnectionsManager connections,
                              Conversation conversation, int cutoff, Feedback feedback) {
        if (owner == null || !owner.isCurrent() || conversation == null) {
            feedback.show(R.string.TelevimReadScopePending);
            return;
        }
        TLRPC.InputPeer peer = knownPeer(messages, owner, conversation.dialogId);
        TLRPC.InputPeer subpeer = conversation.subpeerId == 0 ? null
                : knownPeer(messages, owner, conversation.subpeerId);
        if (peer == null || conversation.subpeerId != 0 && subpeer == null) {
            feedback.show(R.string.TelevimReadScopePending);
            return;
        }
        PrivateReadingPolicy.Result<ReleaseScope> result = owner.policy().manualRead(
                owner.identity(), conversation, cutoff);
        if (!result.accepted()) {
            feedback.show(R.string.TelevimReadScopePending);
            return;
        }
        Drain drain = new Drain(owner, messages, connections, result.value, peer, subpeer, feedback);
        try {
            owner.storage().commitPrivateReadingRelease(owner, result.value, drain::committed);
        } catch (RuntimeException error) {
            drain.finish(R.string.TelevimReadFailed);
        }
    }

    private static TLRPC.InputPeer knownPeer(MessagesController messages, AccountHandle owner, long dialogId) {
        if (dialogId == 0 || dialogId > 0 && dialogId != owner.identity().userId
                && (messages.getUser(dialogId) == null || messages.getUser(dialogId).min)
                || dialogId < 0 && (messages.getChat(-dialogId) == null || messages.getChat(-dialogId).min)) {
            return null;
        }
        TLRPC.InputPeer peer = messages.getInputPeer(dialogId);
        if (peer instanceof TLRPC.TL_inputPeerSelf || peer instanceof TLRPC.TL_inputPeerChat) {
            return peer;
        }
        if ((peer instanceof TLRPC.TL_inputPeerUser || peer instanceof TLRPC.TL_inputPeerChannel)
                && peer.access_hash != 0) {
            return peer;
        }
        return null;
    }

    private static final class Feedback {
        final WeakReference<BaseFragment> fragment;
        final AccountHandle owner;
        private int state;
        private int shownState;
        private boolean scheduled;
        private Toast toast;

        Feedback(BaseFragment fragment, AccountHandle owner) {
            this.fragment = new WeakReference<>(fragment);
            this.owner = owner;
        }

        synchronized void show(int text) {
            int requested = text == R.string.TelevimReadFailed ? 3
                    : text == R.string.TelevimReadScopePending ? 2 : 1;
            state = Math.max(state, requested);
            if (scheduled || state <= shownState) {
                return;
            }
            scheduled = true;
            AndroidUtilities.runOnUIThread(this::display, 1);
        }

        private synchronized void display() {
            scheduled = false;
            BaseFragment screen = fragment.get();
            if (screen == null || screen.isFinished || screen.isPaused() || screen.getParentActivity() == null
                    || screen.getCurrentAccount() != UserConfig.selectedAccount
                    || owner != null && (!owner.isCurrent() || screen.getCurrentAccount() != owner.account())) {
                shownState = state;
                return;
            }
            shownState = state;
            int text = state == 3 ? R.string.TelevimReadFailed
                    : state == 2 ? R.string.TelevimReadScopePending : R.string.TelevimReadSentPending;
            if (toast != null) {
                toast.cancel();
            }
            toast = Toast.makeText(ApplicationLoader.applicationContext,
                    LocaleController.getString(text), Toast.LENGTH_LONG);
            toast.show();
        }
    }

    /**
     * The release owns one drain and never copies its frozen content snapshot.
     * Only one request is outstanding per drain; native transport owns token
     * settlement before invoking our callback. Unknown projection baselines are
     * explicitly deferred, with no local counter or notification subtraction.
     * A later revision-aware adapter must replace that pending disposition.
     */
    private static final class Drain {
        final AccountHandle owner;
        final MessagesController messages;
        final ConnectionsManager connections;
        final ReleaseScope scope;
        final TLRPC.InputPeer peer;
        final TLRPC.InputPeer subpeer;
        final Feedback feedback;
        final AtomicBoolean finished = new AtomicBoolean();
        final AtomicBoolean commitDelivered = new AtomicBoolean();
        int offset;
        boolean historySent;

        Drain(AccountHandle owner, MessagesController messages, ConnectionsManager connections,
              ReleaseScope scope, TLRPC.InputPeer peer, TLRPC.InputPeer subpeer, Feedback feedback) {
            this.owner = owner;
            this.messages = messages;
            this.connections = connections;
            this.scope = scope;
            this.peer = peer;
            this.subpeer = subpeer;
            this.feedback = feedback;
        }

        void committed(PrivateReadingStorage.Result result) {
            if (!commitDelivered.compareAndSet(false, true)) {
                return;
            }
            if (!enqueue(() -> {
                if (!owner.isCurrent() || result == null || result.scope != scope
                        || !result.committed || result.completion == null) {
                    finish(R.string.TelevimReadFailed);
                    return;
                }
                for (PrivateReadingPolicy.Projection projection : PrivateReadingPolicy.Projection.values()) {
                    owner.policy().reconcile(owner.identity(), result.completion, projection, -1);
                }
                next();
            })) {
                finish(R.string.TelevimReadFailed);
            }
        }

        private boolean enqueue(Runnable runnable) {
            try {
                return Utilities.stageQueue.postRunnable(runnable);
            } catch (RuntimeException error) {
                return false;
            }
        }

        void next() {
            if (finished.get()) {
                return;
            }
            if (!owner.isCurrent()) {
                finish(R.string.TelevimReadFailed);
                return;
            }
            if (historySent && offset == scope.contentCount()) {
                finish(R.string.TelevimReadSentPending);
                return;
            }
            final boolean history = !historySent;
            final int count = history ? 0 : Math.min(PrivateReadingPolicy.MAX_REQUEST_IDS,
                    scope.contentCount() - offset);
            TLObject request;
            try {
                request = history ? historyRequest() : contentRequest(count);
            } catch (RuntimeException error) {
                finish(R.string.TelevimReadFailed);
                return;
            }
            PrivateReadingPolicy.Result<RequestToken> reserved = owner.policy().reserveRequest(
                    owner.identity(), scope, history, history ? 0 : offset, count);
            if (!reserved.accepted()) {
                finish(R.string.TelevimReadScopePending);
                return;
            }
            AtomicBoolean delivered = new AtomicBoolean();
            connections.sendPrivateReadingRequest(request, owner, reserved.value, (response, error) -> {
                if (!delivered.compareAndSet(false, true)) {
                    return;
                }
                try {
                    if (!owner.isCurrent() || error != null
                            || !(response instanceof TLRPC.TL_messages_affectedMessages
                            || response instanceof TLRPC.TL_boolTrue)) {
                        finish(R.string.TelevimReadFailed);
                        return;
                    }
                    if (response instanceof TLRPC.TL_messages_affectedMessages) {
                        TLRPC.TL_messages_affectedMessages affected = (TLRPC.TL_messages_affectedMessages) response;
                        messages.processNewDifferenceParams(-1, affected.pts, -1, affected.pts_count);
                    }
                    historySent |= history;
                    offset += count;
                    if (!enqueue(this::next)) {
                        finish(R.string.TelevimReadFailed);
                    }
                } catch (RuntimeException exception) {
                    finish(R.string.TelevimReadFailed);
                }
            });
        }

        TLObject historyRequest() {
            if (scope.conversation.subpeerId != 0) {
                TLRPC.TL_messages_readSavedHistory request = new TLRPC.TL_messages_readSavedHistory();
                request.parent_peer = peer;
                request.peer = subpeer;
                request.max_id = scope.historyCutoff;
                return request;
            }
            if (scope.conversation.rootId != 0) {
                TLRPC.TL_messages_readDiscussion request = new TLRPC.TL_messages_readDiscussion();
                request.peer = peer;
                request.msg_id = (int) scope.conversation.rootId;
                request.read_max_id = scope.historyCutoff;
                return request;
            }
            if (peer instanceof TLRPC.TL_inputPeerChannel) {
                TLRPC.TL_channels_readHistory request = new TLRPC.TL_channels_readHistory();
                request.channel = channel();
                request.max_id = scope.historyCutoff;
                return request;
            }
            TLRPC.TL_messages_readHistory request = new TLRPC.TL_messages_readHistory();
            request.peer = peer;
            request.max_id = scope.historyCutoff;
            return request;
        }

        TLObject contentRequest(int count) {
            if (peer instanceof TLRPC.TL_inputPeerChannel) {
                TLRPC.TL_channels_readMessageContents request = new TLRPC.TL_channels_readMessageContents();
                request.channel = channel();
                for (int index = 0; index < count; ++index) {
                    request.id.add(scope.contentIdAt(offset + index));
                }
                return request;
            }
            TLRPC.TL_messages_readMessageContents request = new TLRPC.TL_messages_readMessageContents();
            for (int index = 0; index < count; ++index) {
                request.id.add(scope.contentIdAt(offset + index));
            }
            return request;
        }

        TLRPC.InputChannel channel() {
            TLRPC.TL_inputChannel channel = new TLRPC.TL_inputChannel();
            channel.channel_id = peer.channel_id;
            channel.access_hash = peer.access_hash;
            return channel;
        }

        void finish(int text) {
            if (finished.compareAndSet(false, true)) {
                if (text != R.string.TelevimReadSentPending) {
                    owner.policy().abandonRelease(owner.identity(), scope);
                } else {
                    owner.policy().finishRelease(owner.identity(), scope);
                }
                feedback.show(text);
            }
        }
    }
}
