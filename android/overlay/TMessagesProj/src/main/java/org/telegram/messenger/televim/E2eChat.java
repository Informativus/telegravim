package org.telegram.messenger.televim;

import android.widget.Toast;

import org.telegram.messenger.AndroidUtilities;
import org.telegram.messenger.ApplicationLoader;
import org.telegram.messenger.DialogObject;
import org.telegram.messenger.LocaleController;
import org.telegram.messenger.MessageObject;
import org.telegram.messenger.R;
import org.telegram.messenger.SendMessagesHelper;
import org.telegram.messenger.UserConfig;
import org.telegram.tgnet.TLObject;
import org.telegram.tgnet.TLRPC;

import java.util.HashMap;
import java.util.WeakHashMap;
import java.util.ArrayList;

public final class E2eChat {
    private static final HashMap<Long, E2eState> STATES = new HashMap<>();
    private static final WeakHashMap<TLObject, Long> REQUEST_PEERS = new WeakHashMap<>();
    public static final int MENU_ID = 1470001;
    public static final int TEXT_LIMIT = 960;

    public static synchronized E2eState state(int account) {
        long userId = UserConfig.getInstance(account).getClientUserId();
        E2eState result = STATES.get(userId);
        if (result == null) {
            result = new E2eState(ApplicationLoader.applicationContext, userId);
            STATES.put(userId, result);
        }
        return result;
    }

    public static synchronized void logout(int account) {
        long userId = UserConfig.getInstance(account).getClientUserId();
        if (userId <= 0) return;
        state(account).clear();
        STATES.remove(userId);
    }

    public static boolean enabled(int account, long peer) {
        return DialogObject.isUserDialog(peer) && state(account).enabled(peer);
    }

    public static boolean eligible(int account, TLRPC.User user) {
        return user != null && !user.bot && !user.deleted && !user.self
                && user.id != UserConfig.getInstance(account).getClientUserId();
    }

    public static void error(int resource) {
        AndroidUtilities.runOnUIThread(() -> Toast.makeText(ApplicationLoader.applicationContext,
                LocaleController.getString(resource), Toast.LENGTH_LONG).show());
    }

    public static boolean blockUnsupported(int account, long peer) {
        if (!enabled(account, peer)) return false;
        error(R.string.TelevimE2eTextOnly);
        return true;
    }

    public static boolean blockEdit(int account, MessageObject message) {
        if (message == null) return false;
        if (!enabled(account, message.getDialogId()) && !isEnvelope(message.messageOwner.message)
                && !state(account).encryptedId(message.getDialogId(), message.getId())) return false;
        error(R.string.TelevimE2eNoEdit);
        return true;
    }

    public static boolean isEnvelope(String text) {
        return text != null && (text.startsWith("TVE2.") || text.startsWith("TVE1."));
    }

    public static void sendControl(int account, long peer, String wire) {
        AndroidUtilities.runOnUIThread(() -> {
            SendMessagesHelper.SendMessageParams params = SendMessagesHelper.SendMessageParams.of(wire, peer);
            params.e2eControl = true;
            params.searchLinks = false;
            SendMessagesHelper.getInstance(account).sendMessage(params);
        });
    }

    public static boolean prepare(int account, SendMessagesHelper.SendMessageParams p) {
        if (!DialogObject.isUserDialog(p.peer)) return true;
        E2eState state = state(account);
        try {
            if (p.e2ePrepared) {
                if (!state.owns(p.peer, p.message)) {
                    error(R.string.TelevimE2eFailed);
                    return false;
                }
            } else if (p.e2eControl) {
                if (p.message == null || !(state.owns(p.peer, p.message) || p.message.equals(state.invitation()))) {
                    error(R.string.TelevimE2eFailed);
                    return false;
                }
            } else if (p.retryMessageObject != null && isEnvelope(p.retryMessageObject.messageOwner.message)) {
                String retry = p.retryMessageObject.messageOwner.message;
                if (!state.owns(p.peer, retry) && !(retry.startsWith("TVE2.KEY.") && retry.equals(state.invitation()))) {
                    error(R.string.TelevimE2eFailed);
                    return false;
                }
            } else if (state.enabled(p.peer)) {
                if (p.message == null || p.message.isEmpty() || p.caption != null || p.location != null
                        || p.photo != null || p.videoEditedInfo != null || p.user != null || p.document != null
                        || p.game != null || p.poll != null || p.todo != null || p.invoice != null
                        || p.retryMessageObject != null || p.replyMarkup != null || p.params != null && !p.params.isEmpty()
                        || p.scheduleDate != 0 || p.scheduleRepeatPeriod != 0 || p.replyToStoryItem != null
                        || p.sendingStory != null || p.richMessage != null || p.ephemeralReceiverBotId != 0
                        || p.quick_reply_shortcut != null || p.quick_reply_shortcut_id != 0
                        || p.sendMessageChatArguments != null && (p.sendMessageChatArguments.quickReplyShortcut != null
                            || p.sendMessageChatArguments.quickReplyShortcutId != 0 || p.sendMessageChatArguments.welcomeMessageChatId != 0)) {
                    error(R.string.TelevimE2eTextOnly);
                    return false;
                }
                if (!state.ready(p.peer)) {
                    error(R.string.TelevimE2ePending);
                    return false;
                }
                if (p.message.length() > TEXT_LIMIT) {
                    error(R.string.TelevimE2eTooLong);
                    return false;
                }
                p.message = state.encryptText(p.peer, p.message);
                p.e2ePrepared = true;
            } else {
                return true;
            }
            p.entities = null;
            p.replyQuote = null;
            p.replyToMsg = null;
            p.replyToTopMsg = null;
            p.webPage = null;
            p.mediaWebPage = null;
            p.searchLinks = false;
            p.replyMarkup = null;
            return true;
        } catch (Exception e) {
            error(R.string.TelevimE2eFailed);
            return false;
        }
    }

    private static long dialog(TLRPC.Message message, int account) {
        if (message.peer_id == null || message.peer_id.user_id == 0) return 0;
        long own = UserConfig.getInstance(account).getClientUserId();
        if (message.out || message.peer_id.user_id != own) return message.peer_id.user_id;
        return message.from_id == null ? 0 : message.from_id.user_id;
    }

    public static void live(int account, TLRPC.Message message) {
        render(account, message, true);
    }

    public static String display(int account, TLRPC.Message message) {
        return render(account, message, false);
    }

    public static boolean protectedMessage(int account, TLRPC.Message message) {
        if (message == null) return false;
        long peer = dialog(message, account);
        return peer > 0 && (isEnvelope(message.message) || state(account).encryptedId(peer, message.id));
    }

    public static void sanitize(int account, TLRPC.Message message) {
        if (!protectedMessage(account, message)) return;
        message.entities = new ArrayList<>();
        message.media = new TLRPC.TL_messageMediaEmpty();
        message.reply_markup = null;
        message.reply_to = null;
        message.replyMessage = null;
        message.rich_message = null;
        message.translatedRichMessage = null;
        message.translatedText = null;
        message.summaryText = null;
        message.translatedSummaryText = null;
        message.summarizedOpen = false;
        message.flags2 &= ~(1 << 13);
        message.flags &= ~(TLRPC.MESSAGE_FLAG_REPLY | TLRPC.MESSAGE_FLAG_HAS_ENTITIES
                | TLRPC.MESSAGE_FLAG_HAS_MEDIA | (1 << 6));
    }

    private static String render(int account, TLRPC.Message message, boolean live) {
        long peer = dialog(message, account);
        if (peer <= 0 || peer == UserConfig.getInstance(account).getClientUserId()) return null;
        String wire = message.message;
        E2eState state = state(account);
        if (!isEnvelope(wire)) {
            return state.encryptedId(peer, message.id) ? LocaleController.getString(R.string.TelevimE2eUnavailable) : null;
        }
        try {
            long sender = message.out ? UserConfig.getInstance(account).getClientUserId() : peer;
            if (message.fwd_from != null || message.via_bot_id != 0 || message.via_business_bot_id != 0
                    || message.guestchat_via_from != null
                    || message.via_bot_name != null && !message.via_bot_name.isEmpty()
                    || message.from_id != null && message.from_id.user_id != sender) {
                throw new Exception("Unexpected encrypted frame sender");
            }
            if (wire.startsWith("TVE2.KEY.")) {
                boolean changed = state.observeKey(peer, wire, live && !message.out);
                return LocaleController.getString(message.out ? R.string.TelevimE2eKeySent
                        : changed ? R.string.TelevimE2eKeyChanged : R.string.TelevimE2eKeyReceived) + "\n\n" + wire;
            }
            E2eState.Received result = state.receive(peer, message.id, message.out, live, wire);
            for (String reply : result.replies) sendControl(account, peer, reply);
            if (result.kind.equals("MSG")) return result.text;
            if (result.kind.equals("ON")) return LocaleController.getString(message.out
                    ? R.string.TelevimE2eRequested : R.string.TelevimE2ePeerRequested);
            if (result.kind.equals("OFF")) return LocaleController.getString(message.out
                    ? R.string.TelevimE2eDisabled : R.string.TelevimE2ePeerDisabled);
            return LocaleController.getString(result.positive ? R.string.TelevimE2eAcknowledged : R.string.TelevimE2ePending);
        } catch (Exception e) {
            return LocaleController.getString(R.string.TelevimE2eUnavailable);
        }
    }

    private static long peerId(int account, TLRPC.InputPeer peer) {
        if (peer instanceof TLRPC.TL_inputPeerSelf) return UserConfig.getInstance(account).getClientUserId();
        return peer == null ? 0 : peer.user_id;
    }

    public static synchronized void bindRequest(TLObject request, long peer) {
        REQUEST_PEERS.put(request, peer);
    }

    private static synchronized long requestPeer(TLObject request) {
        Long peer = REQUEST_PEERS.get(request);
        return peer == null ? 0 : peer;
    }

    public static boolean allowRequest(int account, TLObject object) {
        if (enabled(account, requestPeer(object))) return false;
        E2eState state = state(account);
        long peer = 0;
        if (object instanceof TLRPC.TL_messages_sendMessage) {
            TLRPC.TL_messages_sendMessage request = (TLRPC.TL_messages_sendMessage) object;
            peer = peerId(account, request.peer);
            boolean owned = state.owns(peer, request.message);
            boolean invitation = false;
            if (request.message != null && request.message.startsWith("TVE2.KEY.")) {
                try {
                    invitation = request.message.equals(state.invitation());
                } catch (Exception e) {
                    return false;
                }
            }
            if (!enabled(account, peer) && !owned && !invitation) return true;
            if (!owned && !invitation || request.schedule_date != 0 || request.quick_reply_shortcut != null
                    || request.rich_message != null || request.ephemeralReceiverBotId != 0) return false;
            request.no_webpage = true;
            request.reply_to = null;
            request.reply_markup = null;
            request.entities = new ArrayList<>();
            request.flags &= ~((1 << 0) | (1 << 2) | (1 << 3));
            if (invitation || !request.message.startsWith("TVE2.MSG.")) request.clear_draft = false;
            return true;
        } else if (object instanceof TLRPC.TL_messages_saveDraft) {
            peer = peerId(account, ((TLRPC.TL_messages_saveDraft) object).peer);
        } else if (object instanceof TLRPC.TL_messages_sendMedia) {
            peer = peerId(account, ((TLRPC.TL_messages_sendMedia) object).peer);
        } else if (object instanceof TLRPC.TL_messages_sendMultiMedia) {
            peer = peerId(account, ((TLRPC.TL_messages_sendMultiMedia) object).peer);
        } else if (object instanceof TLRPC.TL_messages_sendInlineBotResult) {
            peer = peerId(account, ((TLRPC.TL_messages_sendInlineBotResult) object).peer);
        } else if (object instanceof TLRPC.TL_messages_getInlineBotResults) {
            peer = peerId(account, ((TLRPC.TL_messages_getInlineBotResults) object).peer);
        } else if (object instanceof TLRPC.TL_messages_uploadMedia) {
            peer = peerId(account, ((TLRPC.TL_messages_uploadMedia) object).peer);
        } else if (object instanceof TLRPC.TL_messages_forwardMessages) {
            peer = peerId(account, ((TLRPC.TL_messages_forwardMessages) object).to_peer);
        } else if (object instanceof TLRPC.TL_messages_sendQuickReplyMessages) {
            peer = peerId(account, ((TLRPC.TL_messages_sendQuickReplyMessages) object).peer);
        } else if (object instanceof TLRPC.TL_messages_editMessage) {
            TLRPC.TL_messages_editMessage request = (TLRPC.TL_messages_editMessage) object;
            peer = peerId(account, request.peer);
            if (state.encryptedId(peer, request.id)) return false;
        }
        return !enabled(account, peer);
    }
}
