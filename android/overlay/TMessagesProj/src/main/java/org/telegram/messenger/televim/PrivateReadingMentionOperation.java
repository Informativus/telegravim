package org.telegram.messenger.televim;

import android.os.Handler;

import org.telegram.messenger.ApplicationLoader;
import org.telegram.messenger.MessagesController;
import org.telegram.messenger.NotificationCenter;
import org.telegram.messenger.NotificationsController;
import org.telegram.messenger.TopicsController;
import org.telegram.tgnet.ConnectionsManager;
import org.telegram.tgnet.RequestDelegate;
import org.telegram.tgnet.TLObject;
import org.telegram.tgnet.TLRPC;

import org.telegram.messenger.televim.PrivateReadingController.AccountHandle;
import org.telegram.messenger.televim.PrivateReadingPolicy.Conversation;
import org.telegram.messenger.televim.PrivateReadingPolicy.RequestToken;

public final class PrivateReadingMentionOperation {
    public interface Projection {
        void secret();
        boolean committed(PrivateReadingMentionStorage.Result result, boolean baselineUnchanged);
        void refreshed(long topicId, int count);
    }

    private final AccountHandle owner;
    private final RequestToken token;
    private final MessagesController messages;
    private final TopicsController topics;
    private final ConnectionsManager connections;
    private final NotificationCenter notifications;
    private final NotificationsController notificationController;
    private final Handler ui;
    private final TLRPC.InputPeer peer;
    private final TLObject request;
    private final Projection projection;
    private final RequestDelegate completion;
    private final long topicId;
    private final long baselineRevision;
    private final TLRPC.Dialog dialog;
    private final TLRPC.TL_forumTopic topic;
    private boolean refreshDialog;
    private boolean refreshTopic;
    private int refreshCycles;

    private PrivateReadingMentionOperation(AccountHandle owner, RequestToken token,
            MessagesController messages, TopicsController topics, ConnectionsManager connections,
            NotificationsController notificationController, NotificationCenter notifications, TLRPC.InputPeer peer, TLObject request,
            Projection projection, RequestDelegate completion) {
        this.owner = owner;
        this.token = token;
        this.messages = messages;
        this.topics = topics;
        this.connections = connections;
        this.notifications = notifications;
        this.notificationController = notificationController;
        this.peer = peer;
        this.request = request;
        this.projection = projection;
        this.completion = completion;
        ui = ApplicationLoader.applicationHandler;
        topicId = token.conversation.subpeerId != 0 ? token.conversation.subpeerId : token.conversation.rootId;
        baselineRevision = messages.privateReadingMentionRevision();
        dialog = messages.dialogs_dict.get(token.conversation.dialogId);
        topic = topicId == 0 ? null : topics.privateReadingTopic(token.conversation.dialogId, topicId,
                token.conversation.subpeerId != 0);
    }

    public static void start(AccountHandle owner, Conversation conversation, int mid,
            MessagesController messages, TopicsController topics, ConnectionsManager connections,
            NotificationsController notificationController, NotificationCenter notifications, TLRPC.InputPeer peer, TLObject request,
            Projection projection, RequestDelegate completion) {
        PrivateReadingPolicy.Result<RequestToken> reserved = owner.policy().reserveNativeMention(
                owner.identity(), conversation, mid);
        if (!reserved.accepted()) {
            return;
        }
        PrivateReadingMentionOperation operation = null;
        try {
            operation = new PrivateReadingMentionOperation(owner, reserved.value, messages, topics,
                    connections, notificationController, notifications, peer, request, projection, completion);
            PrivateReadingMentionOperation captured = operation;
            if (owner.storage().getStorageQueue().postRunnable(captured::commit)) {
                return;
            }
        } catch (Throwable error) {
            org.telegram.messenger.FileLog.e(error);
        }
        if (operation != null) {
            operation.finish();
        } else {
            owner.policy().finishUnclaimedRequest(owner.identity(), reserved.value);
        }
    }

    private void commit() {
        try {
            PrivateReadingMentionStorage.Result result = PrivateReadingMentionStorage.commit(owner, token);
            if (result.committed) {
                owner.storage().applyPrivateReadingMentionFilters(owner, token, messages, notificationController,
                        result.dialogBefore, result.dialogAfter);
            }
            if (!result.committed || !post(() -> committed(result))) {
                finish();
            }
        } catch (Throwable error) {
            finish();
            org.telegram.messenger.FileLog.e(error);
        }
    }

    private void committed(PrivateReadingMentionStorage.Result result) {
        boolean submitted = false;
        try {
            if (!owner.isCurrent()) {
                return;
            }
            boolean unchanged = baselineRevision == messages.privateReadingMentionRevision();
            refreshDialog = !unchanged || dialog == null || messages.dialogs_dict.get(token.conversation.dialogId) != dialog
                    || result.dialogBefore < 0 || result.dialogAfter < 0 || dialog.unread_mentions_count != result.dialogBefore;
            refreshTopic = topicId != 0 && (!unchanged || topic == null
                    || topics.privateReadingTopic(token.conversation.dialogId, topicId, token.conversation.subpeerId != 0) != topic
                    || result.topicBefore < 0 || result.topicAfter < 0 || topic.unread_mentions_count != result.topicBefore);
            if (!refreshDialog) {
                apply(0, result.dialogAfter);
            }
            if (topicId != 0 && !refreshTopic) {
                apply(topicId, result.topicAfter);
            }
            if (projection.committed(result, unchanged)) {
                if (topicId == 0) {
                    refreshDialog = true;
                } else {
                    refreshTopic = true;
                }
            }
            connections.sendPrivateReadingRequest(request, owner, token, (response, error) -> {
                try {
                    completion.run(response, error);
                } finally {
                    if (!post(() -> refreshNext(0))) {
                        finish();
                    }
                }
            });
            submitted = true;
        } finally {
            if (!submitted) {
                finish();
            }
        }
    }

    private Object target(long scope) {
        return scope == 0 ? messages.dialogs_dict.get(token.conversation.dialogId)
                : topics.privateReadingTopic(token.conversation.dialogId, scope, token.conversation.subpeerId != 0);
    }

    private void apply(long scope, int count) {
        if (scope == 0) {
            TLRPC.Dialog current = messages.dialogs_dict.get(token.conversation.dialogId);
            if (current != null) {
                messages.privateReadingMentionChanged();
                current.unread_mentions_count = count;
                messages.sortDialogs(null);
                notifications.postNotificationName(NotificationCenter.dialogsNeedReload);
                notifications.postNotificationName(NotificationCenter.updateInterfaces,
                        MessagesController.UPDATE_MASK_READ_DIALOG_MESSAGE);
            }
        } else {
            TLRPC.TL_forumTopic current = topics.privateReadingTopic(token.conversation.dialogId, scope,
                    token.conversation.subpeerId != 0);
            if (current != null) {
                messages.privateReadingMentionChanged();
                current.unread_mentions_count = count;
                topics.privateReadingMentionApplied(token.conversation.dialogId);
            }
        }
    }

    private void refreshNext(int attempts) {
        if (!refreshDialog && !refreshTopic && owner.policy().takeNativeMentionRefreshDirty(owner.identity(), token)) {
            if (++refreshCycles >= 3) {
                org.telegram.messenger.FileLog.e("Mention reconciliation deferred after concurrent updates");
                finish();
                return;
            }
            refreshDialog = true;
            refreshTopic = topicId != 0;
        }
        if (!owner.isCurrent() || !refreshDialog && !refreshTopic) {
            finish();
            return;
        }
        if (!owner.policy().claimNativeMentionRefresh(owner.identity(), token)) {
            finish();
            return;
        }
        long scope = refreshDialog ? 0 : topicId;
        if (attempts >= 3 || peer == null || scope != 0 && token.conversation.subpeerId != 0) {
            org.telegram.messenger.FileLog.e("Mention count reconciliation requires a fresh supported baseline");
            finish();
            return;
        }
        long revision = messages.privateReadingMentionRevision();
        Object target = target(scope);
        try {
            if (owner.storage().getStorageQueue().postRunnable(() -> {
                try {
                    long databaseRevision = PrivateReadingMentionStorage.revision(owner);
                    if (databaseRevision < 0 || !post(() -> refresh(scope, revision, databaseRevision, attempts, target))) {
                        finish();
                    }
                } catch (Throwable error) {
                    finish();
                }
            })) {
                return;
            }
        } catch (Throwable error) {
            org.telegram.messenger.FileLog.e(error);
        }
        finish();
    }

    private void refresh(long scope, long revision, long databaseRevision, int attempts, Object target) {
        if (!owner.isCurrent()) {
            finish();
            return;
        }
        if (messages.privateReadingMentionRevision() != revision || target(scope) != target) {
            refreshNext(attempts + 1);
            return;
        }
        TLRPC.TL_messages_getUnreadMentions read = new TLRPC.TL_messages_getUnreadMentions();
        read.peer = peer;
        read.limit = 1;
        if (scope != 0) {
            read.flags |= 1;
            read.top_msg_id = (int) scope;
        }
        try {
            connections.sendRequest(read, (response, error) -> {
                if (error != null || !(response instanceof TLRPC.messages_Messages)) {
                    finish();
                    return;
                }
                TLRPC.messages_Messages result = (TLRPC.messages_Messages) response;
                int count = result.count != 0 ? result.count : result.messages.size();
                if (!post(() -> refreshed(scope, revision, databaseRevision, count, attempts, target))) {
                    finish();
                }
            });
        } catch (Throwable error) {
            finish();
        }
    }

    private void refreshed(long scope, long revision, long databaseRevision, int count, int attempts, Object target) {
        if (!owner.isCurrent()) {
            finish();
            return;
        }
        if (messages.privateReadingMentionRevision() != revision || target(scope) != target) {
            refreshNext(attempts + 1);
            return;
        }
        try {
            if (owner.storage().getStorageQueue().postRunnable(() -> {
                boolean stored;
                try {
                    int before = scope == 0 ? PrivateReadingMentionStorage.dialogCount(owner, token.conversation.dialogId) : -1;
                    stored = messages.privateReadingMentionRevision() == revision
                            && PrivateReadingMentionStorage.refresh(owner, token, scope, count, databaseRevision);
                    if (stored && scope == 0) {
                        owner.storage().applyPrivateReadingMentionFilters(owner, token, messages,
                                notificationController, before, count);
                    }
                } catch (Throwable error) {
                    finish();
                    return;
                }
                if (!post(() -> {
                    if (!owner.isCurrent()) {
                        finish();
                    } else if (!stored || messages.privateReadingMentionRevision() != revision || target(scope) != target) {
                        refreshNext(attempts + 1);
                    } else {
                        apply(scope, count);
                        projection.refreshed(scope, count);
                        notifications.postNotificationName(NotificationCenter.updateMentionsCount,
                                token.conversation.dialogId, scope, count, owner);
                        if (scope == 0) {
                            refreshDialog = false;
                        } else {
                            refreshTopic = false;
                        }
                        refreshNext(0);
                    }
                })) {
                    finish();
                }
            })) {
                return;
            }
        } catch (Throwable error) {
            org.telegram.messenger.FileLog.e(error);
        }
        finish();
    }

    private boolean post(Runnable action) {
        try {
            return ui != null && ui.post(() -> {
                try {
                    action.run();
                } catch (Throwable error) {
                    finish();
                    org.telegram.messenger.FileLog.e(error);
                }
            });
        } catch (Throwable error) {
            return false;
        }
    }

    private void finish() {
        owner.policy().finishNativeContentProjection(owner.identity(), token);
        owner.policy().finishUnclaimedRequest(owner.identity(), token);
    }
}
