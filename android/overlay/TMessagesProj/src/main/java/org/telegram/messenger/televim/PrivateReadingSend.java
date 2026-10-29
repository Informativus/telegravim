package org.telegram.messenger.televim;

import org.telegram.messenger.DialogObject;
import org.telegram.messenger.MessageObject;
import org.telegram.messenger.MessagesController;
import org.telegram.messenger.MessagesStorage;
import org.telegram.messenger.SendMessagesHelper.SendMessageParams;
import org.telegram.tgnet.ConnectionsManager;
import org.telegram.tgnet.TLObject;
import org.telegram.tgnet.TLRPC;
import org.telegram.tgnet.tl.TL_update;

import java.lang.ref.WeakReference;
import java.util.ArrayList;
import java.util.concurrent.atomic.AtomicBoolean;

import org.telegram.messenger.televim.PrivateReadingController.AccountHandle;
import org.telegram.messenger.televim.PrivateReadingPolicy.Conversation;
import org.telegram.messenger.televim.PrivateReadingPolicy.Provenance;
import org.telegram.messenger.televim.PrivateReadingPolicy.ReleaseScope;
import org.telegram.messenger.televim.PrivateReadingPolicy.SendAuthorization;

public final class PrivateReadingSend {
    private static final int MAX_ATTEMPTS = 512;
    private static final int MAX_RESPONSE_UPDATES = 4096;
    private static final Origin UNAVAILABLE = new Origin(null, null, null);
    private static final Attempt[] EMPTY = new Attempt[0];
    private static final Object BUDGET = new Object();
    private static final ArrayList<WeakReference<Origin>> ORIGINS = new ArrayList<>();
    private static final ArrayList<WeakReference<Attempt>> RETAINED = new ArrayList<>();

    private PrivateReadingSend() {
    }

    public static Origin capture(int account) {
        AccountHandle owner = PrivateReadingController.captureAccount(account);
        if (owner == null) {
            return UNAVAILABLE;
        }
        MessagesController messages = MessagesController.getInstance(account);
        ConnectionsManager connections = ConnectionsManager.getInstance(account);
        if (!owner.isCurrent()) {
            return UNAVAILABLE;
        }
        synchronized (BUDGET) {
            pruneReferences(ORIGINS);
            if (ORIGINS.size() < MAX_ATTEMPTS) {
                Origin origin = new Origin(owner, messages, connections);
                ORIGINS.add(new WeakReference<>(origin));
                return origin;
            }
        }
        PrivateReadingRelease.sendPending(owner);
        return UNAVAILABLE;
    }

    private static <T> void pruneReferences(ArrayList<WeakReference<T>> references) {
        for (int index = references.size() - 1; index >= 0; --index) {
            if (references.get(index).get() == null) {
                references.remove(index);
            }
        }
    }

    private static Attempt retain(Registry registry, Origin origin, SendAuthorization authorization,
                                  int forumFlags, int parentReplyId, MessageObject message) {
        synchronized (BUDGET) {
            pruneReferences(RETAINED);
            if (RETAINED.size() >= MAX_ATTEMPTS) {
                return null;
            }
            Attempt attempt = new Attempt(registry, origin, authorization, forumFlags, parentReplyId, message);
            RETAINED.add(new WeakReference<>(attempt));
            return attempt;
        }
    }

    public static void origin(int account, SendMessageParams params) {
        if (params.receiptOrigin == null) {
            params.receiptOrigin = params.retryMessageObject == null ? capture(account) : UNAVAILABLE;
        }
    }

    public static final class Origin {
        private final AccountHandle owner;
        private final MessagesController messages;
        private final ConnectionsManager connections;

        private Origin(AccountHandle owner, MessagesController messages, ConnectionsManager connections) {
            this.owner = owner;
            this.messages = messages;
            this.connections = connections;
        }
    }

    /**
     * Native work retains its own payload. This registry has at most 512 weak
     * links and small capabilities, and never keeps message text or media alive.
     * A new manual retry gets a new MessageObject; automatic retries keep the
     * original object. Terminal callbacks capture Attempt, never a lookup key.
     */
    public static final class Registry {
        private final ArrayList<Attempt> attempts = new ArrayList<>();

        public void register(SendMessageParams params, MessageObject message, boolean welcome) {
            if (params.receiptRegistered) {
                return;
            }
            params.receiptRegistered = true;
            Provenance provenance = classify(params, message, welcome);
            if (provenance != null) {
                register(params.receiptOrigin, message, provenance,
                        params.replyToTopMsg == null ? 0 : params.replyToTopMsg.getId(),
                        parentReplyId(params, message));
            }
        }

        public void forward(Origin origin, MessageObject message, MessageObject source,
                            int scheduleDate, int expectedRoot) {
            if (scheduleDate == 0 && ordinary(message) && ordinaryPayload(message.messageOwner)
                    && source != null && !source.isEphemeral() && !source.isQuickReply()
                    && !source.isWelcomeMessage() && source.messageOwner.action == null
                    && !E2eChat.isEnvelope(source.messageOwner.message)) {
                register(origin, message, Provenance.USER_MESSAGE, expectedRoot, 0);
            }
        }

        private void register(Origin origin, MessageObject message, Provenance provenance,
                              int expectedRoot, int expectedParentReplyId) {
            if (origin == null || origin.owner == null || !origin.owner.isCurrent()) {
                return;
            }
            Conversation conversation = origin.owner.conversation(message);
            if (conversation == null) {
                PrivateReadingRelease.sendPending(origin.owner);
                return;
            }
            if (conversation.category == PrivateReadingPolicy.Category.SECRET
                    || conversation.category == PrivateReadingPolicy.Category.SELF) {
                return;
            }
            if (expectedRoot > 0 && conversation.rootId != expectedRoot
                    || message.messageOwner.saved_peer_id != null
                    && MessageObject.getMonoForumTopicId(message.messageOwner) != conversation.subpeerId) {
                PrivateReadingRelease.sendPending(origin.owner);
                return;
            }
            int forumFlags = origin.owner.storage().getForumTypeFlags(conversation.dialogId);
            int parentReplyId = expectedRoot == 0 && forumFlags == 0
                    && conversation.rootId == 0 && conversation.subpeerId == 0
                    && (conversation.category == PrivateReadingPolicy.Category.PRIVATE
                    || conversation.category == PrivateReadingPolicy.Category.GROUP)
                    ? expectedParentReplyId : 0;
            boolean refused = false;
            synchronized (this) {
                prune();
                if (attempts.size() >= MAX_ATTEMPTS) {
                    refused = true;
                } else {
                    PrivateReadingPolicy.Result<SendAuthorization> result = origin.owner.policy().registerSend(
                            origin.owner.identity(), conversation, message.messageOwner.random_id, provenance);
                    if (result.accepted()) {
                        Attempt attempt = retain(this, origin, result.value, forumFlags, parentReplyId, message);
                        if (attempt != null) {
                            attempts.add(attempt);
                        } else {
                            origin.owner.policy().failedSend(origin.owner.identity(), result.value);
                            refused = true;
                        }
                    } else {
                        refused = true;
                    }
                }
            }
            if (refused) {
                PrivateReadingRelease.sendPending(origin.owner);
            }
        }

        public synchronized Attempt capture(MessageObject message) {
            if (message == null) {
                return null;
            }
            for (Attempt attempt : attempts) {
                if (attempt.message.get() == message) {
                    return attempt;
                }
            }
            return null;
        }

        public synchronized Attempt[] capture(ArrayList<MessageObject> messages) {
            if (messages == null || messages.size() > MAX_ATTEMPTS) {
                return EMPTY;
            }
            Attempt[] result = new Attempt[messages.size()];
            for (int index = 0; index < messages.size(); ++index) {
                result[index] = capture(messages.get(index));
            }
            return result;
        }

        public void cancel(MessageObject message) {
            fail(capture(message));
        }

        public synchronized void cancel(TLRPC.Message message) {
            if (message == null) {
                return;
            }
            for (int index = attempts.size() - 1; index >= 0; --index) {
                Attempt attempt = attempts.get(index);
                MessageObject work = attempt.message.get();
                if (work != null && work.messageOwner == message) {
                    fail(attempt);
                }
            }
        }

        public synchronized void manualRetry(MessageObject message) {
            if (message == null || message.messageOwner == null) {
                return;
            }
            long randomId = message.messageOwner.random_id;
            for (int index = attempts.size() - 1; index >= 0; --index) {
                Attempt attempt = attempts.get(index);
                if (attempt.authorization.originalRandomId == randomId) {
                    fail(attempt);
                }
            }
        }

        public synchronized void clear() {
            while (!attempts.isEmpty()) {
                fail(attempts.get(attempts.size() - 1));
            }
        }

        private void prune() {
            for (int index = attempts.size() - 1; index >= 0; --index) {
                Attempt attempt = attempts.get(index);
                if (attempt.message.get() == null || !attempt.origin.owner.isCurrent()) {
                    fail(attempt);
                }
            }
        }

        private synchronized void remove(Attempt attempt) {
            attempts.remove(attempt);
        }
    }

    public static final class Attempt {
        private final Registry registry;
        private final Origin origin;
        private final SendAuthorization authorization;
        private final int forumFlags;
        private final int parentReplyId;
        private final WeakReference<MessageObject> message;
        private final AtomicBoolean terminal = new AtomicBoolean();

        private Attempt(Registry registry, Origin origin, SendAuthorization authorization,
                        int forumFlags, int parentReplyId, MessageObject message) {
            this.registry = registry;
            this.origin = origin;
            this.authorization = authorization;
            this.forumFlags = forumFlags;
            this.parentReplyId = parentReplyId;
            this.message = new WeakReference<>(message);
        }
    }

    public static void fail(Attempt attempt) {
        if (attempt == null) {
            return;
        }
        if (attempt.terminal.compareAndSet(false, true)) {
            AccountHandle owner = attempt.origin.owner;
            owner.policy().failedSend(owner.identity(), attempt.authorization);
        }
        attempt.registry.remove(attempt);
    }

    public static void fail(Attempt[] attempts) {
        for (Attempt attempt : attempts) {
            fail(attempt);
        }
    }

    public static Attempt dispatch(Attempt attempt, TLObject request, boolean scheduled) {
        if (attempt != null && (scheduled || !requestMatches(attempt, request))) {
            fail(attempt);
            return null;
        }
        return attempt;
    }

    public static void dispatch(Attempt[] attempts, TLObject request, boolean scheduled) {
        for (int index = 0; index < attempts.length; ++index) {
            attempts[index] = dispatch(attempts[index], request, scheduled);
        }
    }

    public static void terminal(Attempt[] attempts, TLObject response, TLRPC.TL_error error) {
        for (Attempt attempt : attempts) {
            terminal(attempt, response, error, false);
        }
    }

    public static void terminal(Attempt attempt, TLObject response, TLRPC.TL_error error, boolean shortResult) {
        if (attempt == null || attempt.terminal.get()) {
            return;
        }
        int id = 0;
        if (error == null && attempt.origin.owner.isCurrent()) {
            if (shortResult && response instanceof TLRPC.TL_updateShortSentMessage) {
                TLRPC.TL_updateShortSentMessage sent = (TLRPC.TL_updateShortSentMessage) response;
                if (sent.out && validId(sent.id)) {
                    id = sent.id;
                }
            } else if (response instanceof TLRPC.Updates) {
                id = mappedResult(attempt, (TLRPC.Updates) response);
            }
        }
        if (!validId(id)) {
            fail(attempt);
            if (error == null) {
                PrivateReadingRelease.sendPending(attempt.origin.owner);
            }
            return;
        }
        if (!attempt.terminal.compareAndSet(false, true)) {
            return;
        }
        AccountHandle owner = attempt.origin.owner;
        PrivateReadingPolicy.Result<ReleaseScope> result = owner.policy().acceptedSend(
                owner.identity(), attempt.authorization, id);
        attempt.registry.remove(attempt);
        if (!result.accepted()) {
            owner.policy().failedSend(owner.identity(), attempt.authorization);
            PrivateReadingRelease.sendPending(owner);
        } else if (result.value != null) {
            PrivateReadingRelease.accepted(owner, attempt.origin.messages, attempt.origin.connections, result.value);
        }
    }

    private static int mappedResult(Attempt attempt, TLRPC.Updates updates) {
        if (updates.updates == null || updates.updates.size() > MAX_RESPONSE_UPDATES) {
            return 0;
        }
        long randomId = attempt.authorization.originalRandomId;
        int id = 0;
        for (TLRPC.Update update : updates.updates) {
            if (update instanceof TL_update.TL_updateMessageID) {
                TL_update.TL_updateMessageID mapped = (TL_update.TL_updateMessageID) update;
                if (mapped.random_id == randomId) {
                    if (id != 0 || !validId(mapped.id)) {
                        return 0;
                    }
                    id = mapped.id;
                }
            }
        }
        if (!validId(id)) {
            return 0;
        }
        TLRPC.Message result = null;
        for (TLRPC.Update update : updates.updates) {
            TLRPC.Message candidate = null;
            if (update instanceof TL_update.TL_updateMessageID) {
                TL_update.TL_updateMessageID mapped = (TL_update.TL_updateMessageID) update;
                if (mapped.id == id && mapped.random_id != randomId) {
                    return 0;
                }
            } else if (update instanceof TL_update.TL_updateNewMessage) {
                candidate = ((TL_update.TL_updateNewMessage) update).message;
            } else if (update instanceof TL_update.TL_updateNewChannelMessage) {
                candidate = ((TL_update.TL_updateNewChannelMessage) update).message;
            } else if (update instanceof TL_update.TL_updateNewScheduledMessage) {
                if (((TL_update.TL_updateNewScheduledMessage) update).message.id == id) {
                    return 0;
                }
            } else if (update instanceof TL_update.TL_updateQuickReplyMessage) {
                if (((TL_update.TL_updateQuickReplyMessage) update).message.id == id) {
                    return 0;
                }
            } else if (update instanceof TL_update.TL_updateNewEphemeralMessage) {
                return 0;
            }
            if (candidate != null && candidate.id == id) {
                if (result != null || !ordinaryPayload(candidate) || !sameConversation(attempt, candidate)) {
                    return 0;
                }
                result = candidate;
            }
        }
        return result == null ? 0 : id;
    }

    private static boolean sameConversation(Attempt attempt, TLRPC.Message message) {
        Conversation conversation = attempt.authorization.conversation;
        if (message.peer_id == null || DialogObject.getPeerDialogId(message.peer_id) != conversation.dialogId
                || message.saved_peer_id != null
                && MessageObject.getMonoForumTopicId(message) != conversation.subpeerId) {
            return false;
        }
        long topic = MessageObject.getTopicId(attempt.origin.owner.account(), message, attempt.forumFlags);
        boolean mono = (attempt.forumFlags & MessagesStorage.FORUM_TYPE_DIRECT) != 0;
        return mono ? conversation.rootId == 0 && topic != 0 && topic == conversation.subpeerId
                : conversation.subpeerId == 0 && topic == conversation.rootId
                && !(topic == 0 && message.isThreadMessage);
    }

    public static boolean shortResult(Attempt attempt, TLObject request) {
        if (attempt == null) {
            return false;
        }
        TLRPC.InputReplyTo reply;
        if (request instanceof TLRPC.TL_messages_sendMessage) {
            reply = ((TLRPC.TL_messages_sendMessage) request).reply_to;
        } else if (request instanceof TLRPC.TL_messages_sendMedia) {
            reply = ((TLRPC.TL_messages_sendMedia) request).reply_to;
        } else {
            return false;
        }
        Conversation conversation = attempt.authorization.conversation;
        long subpeer = reply == null ? 0 : peerId(reply.monoforum_peer_id);
        if (subpeer != conversation.subpeerId) {
            return false;
        }
        if (conversation.rootId == 0) {
            return reply == null || reply.top_msg_id == 0
                    && (reply.reply_to_msg_id == 0 || attempt.parentReplyId > 0
                    && reply.reply_to_msg_id == attempt.parentReplyId
                    && (reply.reply_to_peer_id == null
                    || peerId(reply.reply_to_peer_id) == conversation.dialogId));
        }
        if (reply == null) {
            return conversation.rootId == 1 && (attempt.forumFlags & MessagesStorage.FORUM_TYPE_CHAT) != 0;
        }
        long root = reply.top_msg_id != 0 ? reply.top_msg_id : reply.reply_to_msg_id;
        return root == conversation.rootId;
    }

    private static long peerId(TLRPC.InputPeer peer) {
        return peer == null ? 0 : peer.user_id != 0 ? peer.user_id
                : peer.chat_id != 0 ? -peer.chat_id : -peer.channel_id;
    }

    private static int parentReplyId(SendMessageParams params, MessageObject message) {
        MessageObject reply = params.replyToMsg;
        TLRPC.MessageReplyHeader header = message.messageOwner.reply_to;
        if (params.replyToTopMsg != null || reply == null || reply.messageOwner == null
                || reply.currentAccount != message.currentAccount || reply.scheduled
                || reply.messageOwner.isThreadMessage || reply.isTopicMainMessage
                || reply.messageOwner.reply_to != null && (reply.messageOwner.reply_to.forum_topic
                || reply.messageOwner.reply_to.reply_to_top_id != 0)
                || message.messageOwner.isThreadMessage || header == null || header.forum_topic
                || header.reply_to_top_id != 0 || !validId(reply.getId())
                || reply.getDialogId() != message.getDialogId() || header.reply_to_msg_id != reply.getId()) {
            return 0;
        }
        return reply.getId();
    }

    private static boolean requestMatches(Attempt attempt, TLObject request) {
        TLRPC.InputPeer peer;
        long randomId = attempt.authorization.originalRandomId;
        if (request instanceof TLRPC.TL_messages_sendMessage) {
            TLRPC.TL_messages_sendMessage send = (TLRPC.TL_messages_sendMessage) request;
            if (send.random_id != randomId || send.schedule_date != 0 || send.schedule_repeat_period != 0
                    || send.quick_reply_shortcut != null || send.ephemeralReceiverBotId != 0
                    || send.suggested_post != null || send.rich_message != null) {
                return false;
            }
            peer = send.peer;
        } else if (request instanceof TLRPC.TL_messages_sendMedia) {
            TLRPC.TL_messages_sendMedia send = (TLRPC.TL_messages_sendMedia) request;
            if (send.random_id != randomId || send.schedule_date != 0 || send.schedule_repeat_period != 0
                    || send.quick_reply_shortcut != null || send.ephemeralReceiverBotId != 0
                    || send.suggested_post != null || send.media instanceof TLRPC.TL_inputMediaPaidMedia
                    || send.media instanceof TLRPC.TL_inputMediaPoll || send.media instanceof TLRPC.TL_inputMediaStakeDice) {
                return false;
            }
            peer = send.peer;
        } else if (request instanceof TLRPC.TL_messages_sendMultiMedia) {
            TLRPC.TL_messages_sendMultiMedia send = (TLRPC.TL_messages_sendMultiMedia) request;
            if (send.schedule_date != 0 || send.quick_reply_shortcut != null || send.multi_media.size() > MAX_ATTEMPTS) {
                return false;
            }
            int matches = 0;
            for (TLRPC.TL_inputSingleMedia media : send.multi_media) {
                if (media.random_id == randomId) {
                    ++matches;
                }
            }
            if (matches != 1) {
                return false;
            }
            peer = send.peer;
        } else if (request instanceof TLRPC.TL_messages_forwardMessages) {
            TLRPC.TL_messages_forwardMessages send = (TLRPC.TL_messages_forwardMessages) request;
            if (send.schedule_date != 0 || send.schedule_repeat_period != 0 || send.quick_reply_shortcut != null
                    || send.from_ephemeral || send.suggested_post != null || send.random_id.size() > MAX_ATTEMPTS) {
                return false;
            }
            int matches = 0;
            for (long id : send.random_id) {
                if (id == randomId) {
                    ++matches;
                }
            }
            if (matches != 1) {
                return false;
            }
            peer = send.to_peer;
        } else {
            return false;
        }
        long dialogId = peerId(peer);
        return dialogId != 0 && dialogId == attempt.authorization.conversation.dialogId;
    }

    private static Provenance classify(SendMessageParams params, MessageObject message, boolean welcome) {
        if (!ordinary(message) || !ordinaryPayload(message.messageOwner) || welcome || params.receiptExcluded
                || params.scheduleDate != 0 || params.scheduleRepeatPeriod != 0 || params.e2eControl
                || params.quick_reply_shortcut != null || params.quick_reply_shortcut_id != 0
                || params.ephemeralReceiverBotId != 0 || params.pollSendParams != null || params.poll != null
                || params.todo != null || params.game != null || params.invoice != null || params.richMessage != null
                || params.stars != 0 || params.dice_stake != 0 || params.suggestionParams != null
                || params.sendingStory != null || params.isLivePhoto
                || params.params != null && (params.params.containsKey("query_id") || params.params.containsKey("bot"))
                || params.sendMessageChatArguments != null
                && (params.sendMessageChatArguments.quickReplyShortcut != null
                || params.sendMessageChatArguments.quickReplyShortcutId != 0
                || params.sendMessageChatArguments.welcomeMessageChatId != 0)) {
            return null;
        }
        String wire = message.messageOwner.message;
        if (E2eChat.isEnvelope(wire)) {
            return wire.startsWith("TVE2.MSG.") && (params.e2ePrepared || params.receiptManualRetry)
                    ? Provenance.TVE2_MESSAGE : null;
        }
        return Provenance.USER_MESSAGE;
    }

    private static boolean ordinary(MessageObject message) {
        return message != null && message.messageOwner != null && !message.scheduled && !message.isEditing()
                && !DialogObject.isEncryptedDialog(message.getDialogId());
    }

    private static boolean ordinaryPayload(TLRPC.Message message) {
        return message instanceof TLRPC.TL_message && message.out && message.action == null
                && message.quick_reply_shortcut == null && message.quick_reply_shortcut_id == 0
                && message.ephemeralReceiverBotId == 0 && message.via_business_bot_id == 0
                && !message.from_scheduled && message.date != 0x7ffffffe && message.guestchat_via_from == null
                && message.suggested_post == null && message.rich_message == null
                && !(message.media instanceof TLRPC.TL_messageMediaPaidMedia)
                && !(message.media instanceof TLRPC.TL_messageMediaPoll)
                && !(message.media instanceof TLRPC.TL_messageMediaToDo)
                && !(message.media instanceof TLRPC.TL_messageMediaGame)
                && !(message.media instanceof TLRPC.TL_messageMediaInvoice);
    }

    private static boolean validId(int id) {
        return id > 0 && id < Integer.MAX_VALUE;
    }
}
