package org.telegram.messenger.televim;

import android.app.PendingIntent;
import android.content.Context;
import android.content.Intent;
import android.widget.Toast;

import androidx.core.app.NotificationCompat;

import org.telegram.messenger.ApplicationLoader;
import org.telegram.messenger.DialogObject;
import org.telegram.messenger.LocaleController;
import org.telegram.messenger.MessageObject;
import org.telegram.messenger.MessagesController;
import org.telegram.messenger.R;
import org.telegram.tgnet.ConnectionsManager;

import java.util.HashMap;
import java.util.Iterator;
import java.util.Map;

import org.telegram.messenger.televim.PrivateReadingController.AccountHandle;
import org.telegram.messenger.televim.PrivateReadingPolicy.Category;
import org.telegram.messenger.televim.PrivateReadingPolicy.Conversation;
import org.telegram.messenger.televim.PrivateReadingPolicy.NotificationAction;
import org.telegram.messenger.televim.PrivateReadingPolicy.ReceiptDecision;
import org.telegram.messenger.televim.PrivateReadingPolicy.ReleaseScope;

public final class PrivateReadingNotificationAction {
    private static final String PREFIX = "org.telegram.messenger.televim.MARK_READ.";
    private static final Map<Integer, Registration> REGISTRATIONS = new HashMap<>();

    private PrivateReadingNotificationAction() {
    }

    public static Origin capture(int account) {
        AccountHandle owner = PrivateReadingController.captureAccount(account);
        if (owner == null || !owner.isCurrent()) {
            return null;
        }
        MessagesController messages = MessagesController.getInstance(account);
        ConnectionsManager connections = ConnectionsManager.getInstance(account);
        return owner.isCurrent() ? new Origin(owner, messages, connections) : null;
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

        public boolean isCurrent() {
            return owner.isCurrent();
        }

        public boolean sameLifetime(Origin other) {
            return other != null && owner.identity() == other.owner.identity();
        }

        public Conversation conversation(MessageObject message, long displayedTopic) {
            Conversation conversation = owner.conversation(message);
            if (conversation == null || message.isReactionPush || message.isStoryReactionPush
                    || message.isStoryPush || message.scheduled) {
                return null;
            }
            if (conversation.category != Category.SECRET && conversation.category != Category.SELF
                    && (conversation.subpeerId != 0 ? conversation.subpeerId : conversation.rootId) != displayedTopic) {
                return null;
            }
            if (conversation.category != Category.SECRET && conversation.category != Category.SELF
                    && (conversation.rootId > Integer.MAX_VALUE
                    || displayedTopic == 0 && (messages.isForum(conversation.dialogId)
                    || messages.isMonoForum(conversation.dialogId)))) {
                return null;
            }
            if (conversation.category == Category.SECRET
                    && messages.getEncryptedChat(DialogObject.getEncryptedChatId(conversation.dialogId)) == null) {
                return null;
            }
            return conversation;
        }
    }

    private static final class Registration {
        final Origin origin;
        final NotificationAction action;
        final int date;
        final PendingIntent intent;

        Registration(Origin origin, NotificationAction action, int date, PendingIntent intent) {
            this.origin = origin;
            this.action = action;
            this.date = date;
            this.intent = intent;
        }

        void retire() {
            origin.owner.policy().retireNotification(origin.owner.identity(), action.actionId);
            intent.cancel();
        }
    }

    public static synchronized void retire(int notificationId) {
        Registration previous = REGISTRATIONS.remove(notificationId);
        if (previous != null) {
            previous.retire();
        }
    }

    public static synchronized void retire(Origin origin) {
        Iterator<Registration> iterator = REGISTRATIONS.values().iterator();
        while (iterator.hasNext()) {
            Registration registration = iterator.next();
            if (!registration.origin.isCurrent() || registration.origin.sameLifetime(origin)) {
                iterator.remove();
                registration.retire();
            }
        }
    }

    /**
     * All native publishers use the actual Android notification ID as the key.
     * The policy reserves 2048 bytes for each record and its fixed-size wrapper;
     * only references, scalar scope, and an immutable PendingIntent are retained.
     * Replacement and display run under the same lock as one-shot consumption,
     * so a retired holder cannot publish an old action after its replacement.
     */
    public static synchronized void publish(Origin origin, int notificationId, Conversation conversation,
                                            int cutoff, int date, boolean markRead,
                                            NotificationCompat.Builder builder, Runnable display) {
        if (origin == null || !origin.isCurrent()) {
            return;
        }
        retire(notificationId);
        retire(null);
        Registration registration = null;
        try {
            if (markRead && conversation != null
                    && (conversation.category != Category.SECRET || date > 0)) {
                PrivateReadingPolicy.Result<NotificationAction> result = origin.owner.policy().registerNotification(
                        origin.owner.identity(), conversation, cutoff);
                if (result.accepted()) {
                    NotificationAction action = result.value;
                    try {
                        Intent intent = new Intent(ApplicationLoader.applicationContext, PrivateReadingNotificationReceiver.class);
                        intent.setAction(PREFIX + action.actionId);
                        intent.putExtra("currentAccount", origin.owner.account());
                        PendingIntent pending = PendingIntent.getBroadcast(ApplicationLoader.applicationContext, 0,
                                intent, PendingIntent.FLAG_IMMUTABLE);
                        registration = new Registration(origin, action, date, pending);
                        REGISTRATIONS.put(notificationId, registration);
                        builder.addAction(new NotificationCompat.Action.Builder(R.drawable.msg_markread,
                                LocaleController.getString(R.string.MarkAsRead), pending)
                                .setSemanticAction(NotificationCompat.Action.SEMANTIC_ACTION_MARK_AS_READ)
                                .setShowsUserInterface(false).build());
                    } catch (RuntimeException error) {
                        origin.owner.policy().retireNotification(origin.owner.identity(), action.actionId);
                        throw error;
                    }
                }
            }
            if (markRead && registration == null) {
                addOpenApp(builder);
            }
            if (!origin.isCurrent()) {
                retire(notificationId);
                return;
            }
            display.run();
        } catch (RuntimeException error) {
            retire(notificationId);
            throw error;
        }
    }

    private static void addOpenApp(NotificationCompat.Builder builder) {
        Context context = ApplicationLoader.applicationContext;
        Intent launch = context.getPackageManager().getLaunchIntentForPackage(context.getPackageName());
        if (launch != null) {
            launch.setAction("org.telegram.messenger.televim.OPEN_PENDING_READ");
            PendingIntent intent = PendingIntent.getActivity(context, 0, launch, PendingIntent.FLAG_IMMUTABLE);
            builder.addAction(new NotificationCompat.Action.Builder(R.drawable.msg_markread,
                    LocaleController.getString(R.string.TelevimReadOpenAppAction), intent).setShowsUserInterface(true).build());
        }
    }

    static synchronized void receive(Intent intent) {
        String action = intent == null ? null : intent.getAction();
        if (action == null || !action.startsWith(PREFIX) || action.length() != PREFIX.length() + 36
                || !intent.hasExtra("currentAccount")) {
            openApp();
            return;
        }
        String token = action.substring(PREFIX.length());
        Registration registration = null;
        Iterator<Registration> iterator = REGISTRATIONS.values().iterator();
        while (iterator.hasNext()) {
            Registration candidate = iterator.next();
            if (candidate.action.actionId.equals(token)) {
                if (intent.getIntExtra("currentAccount", -1) != candidate.origin.owner.account()) {
                    openApp();
                    return;
                }
                registration = candidate;
                iterator.remove();
                break;
            }
        }
        if (registration == null) {
            openApp();
            return;
        }
        Origin origin = registration.origin;
        AccountHandle owner = origin.owner;
        NotificationAction captured = registration.action;
        if (!owner.isCurrent()) {
            registration.retire();
            openApp();
            return;
        }
        Conversation current = owner.conversation(captured.conversation.dialogId,
                captured.conversation.rootId, captured.conversation.subpeerId);
        PrivateReadingPolicy.Result<ReleaseScope> result = owner.policy().consumeNotification(
                owner.identity(), token, current, captured.cutoff);
        registration.intent.cancel();
        if (!result.accepted() || !owner.isCurrent()) {
            if (result.accepted() && result.value != null) {
                owner.policy().abandonRelease(owner.identity(), result.value);
            }
            openApp();
            return;
        }
        if (result.value != null) {
            PrivateReadingRelease.notification(owner, origin.messages, origin.connections, result.value);
            return;
        }
        if (current == null || owner.policy().decision(owner.identity(), current) != ReceiptDecision.NATIVE
                || !owner.isCurrent()) {
            openApp();
            return;
        }
        boolean monoForum = owner.storage().isMonoForum(current.dialogId);
        if (current.category != Category.SECRET && current.category != Category.SELF
                && ((current.subpeerId != 0) != monoForum || current.rootId != 0 && monoForum)) {
            openApp();
            return;
        }
        long thread = current.subpeerId != 0 ? current.subpeerId : current.rootId;
        origin.messages.markDialogAsRead(current.dialogId, captured.cutoff, captured.cutoff,
                current.category == Category.SECRET ? registration.date : 0,
                false, thread, 0, true, 0);
        if (current.category != Category.SECRET && owner.isCurrent()
                && owner.policy().decision(owner.identity(), current) == ReceiptDecision.NATIVE) {
            origin.messages.markReactionsAsRead(current.dialogId, thread);
        }
    }

    public static void openApp() {
        Toast.makeText(ApplicationLoader.applicationContext,
                LocaleController.getString(R.string.TelevimReadOpenAppGuidance), Toast.LENGTH_LONG).show();
    }
}
