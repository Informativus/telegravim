package org.telegram.messenger.televim;

import android.os.Looper;

import org.telegram.SQLite.SQLiteDatabase;
import org.telegram.SQLite.SQLiteException;
import org.telegram.SQLite.SQLitePreparedStatement;
import org.telegram.messenger.ApplicationLoader;
import org.telegram.messenger.DialogObject;
import org.telegram.messenger.FileLog;
import org.telegram.tgnet.ConnectionsManager;
import org.telegram.tgnet.RequestDelegate;
import org.telegram.tgnet.TLObject;

import org.telegram.messenger.televim.PrivateReadingController.AccountHandle;
import org.telegram.messenger.televim.PrivateReadingPolicy.Conversation;
import org.telegram.messenger.televim.PrivateReadingPolicy.RequestToken;

public final class PrivateReadingMarkerScope {
    private PrivateReadingMarkerScope() {
    }

    public static boolean start(AccountHandle owner, Conversation conversation, boolean reactions,
            ConnectionsManager connections, TLObject request, Runnable projection,
            Runnable pending, RequestDelegate completion) {
        if (owner == null || !owner.isCurrent() || conversation == null || request == null
                || !reactions && (conversation.subpeerId != 0 || owner.storage().isMonoForum(conversation.dialogId))) {
            return false;
        }
        PrivateReadingPolicy.Result<RequestToken> reserved = owner.policy().reserveNativeMarkerScope(
                owner.identity(), conversation, reactions);
        if (!reserved.accepted()) {
            return false;
        }
        RequestToken token = reserved.value;
        final TLObject captured = connections.prepareNativeMarkerScopeRequest(request, owner, token);
        if (captured == null) {
            finish(owner, token);
            return false;
        }
        try {
            if (owner.storage().getStorageQueue().postRunnable(() -> {
                if (!commit(owner, token, reactions)) {
                    finish(owner, token);
                    postPending(owner, pending);
                    return;
                }
                try {
                    if (ApplicationLoader.applicationHandler.post(() -> {
                        boolean submitted = false;
                        try {
                            if (!owner.isCurrent() || owner.storage().getDatabase() != owner.database()) {
                                return;
                            }
                            projection.run();
                            connections.sendPrivateReadingRequest(captured, owner, token, (response, error) -> {
                                try {
                                    completion.run(response, error);
                                } finally {
                                    owner.policy().finishNativeContentProjection(owner.identity(), token);
                                }
                            });
                            submitted = true;
                        } finally {
                            if (!submitted) {
                                finish(owner, token);
                            }
                        }
                    })) {
                        return;
                    }
                } catch (Throwable error) {
                    FileLog.e(error);
                }
                finish(owner, token);
            })) {
                return true;
            }
        } catch (Throwable error) {
            FileLog.e(error);
        }
        finish(owner, token);
        return false;
    }

    private static void postPending(AccountHandle owner, Runnable pending) {
        if (pending != null) {
            try {
                ApplicationLoader.applicationHandler.post(() -> {
                    if (owner.isCurrent()) {
                        pending.run();
                    }
                });
            } catch (Throwable error) {
                FileLog.e(error);
            }
        }
    }

    private static void finish(AccountHandle owner, RequestToken token) {
        owner.policy().finishNativeContentProjection(owner.identity(), token);
        owner.policy().finishUnclaimedRequest(owner.identity(), token);
    }

    private static boolean commit(AccountHandle owner, RequestToken token, boolean reactions) {
        SQLiteDatabase lease = owner.database();
        if (!owner.isCurrent() || !token.nativeMarkerScope(reactions) || lease == null
                || owner.storage().getDatabase() != lease
                || owner.storage().getStorageQueue().getHandler().getLooper() != Looper.myLooper()
                || !owner.policy().reserveNativeContentCommit(owner.identity(), token)) {
            return false;
        }
        boolean transaction = false;
        boolean committed = false;
        try {
            Conversation key = token.conversation;
            long topic = key.subpeerId != 0 ? key.subpeerId : key.rootId;
            if (DialogObject.isEncryptedDialog(key.dialogId) || owner.storage().getDatabase() != lease) {
                throw new SQLiteException("Invalid marker scope or database lease");
            }
            lease.beginTransactionChecked();
            transaction = true;
            String table = reactions ? "reaction_mentions" : "poll_votes_mentions";
            String counter = reactions ? "unread_reactions" : "unread_poll_votes";
            if (topic == 0) {
                execute(lease, "UPDATE dialogs SET " + counter + " = 0 WHERE did = " + key.dialogId);
                execute(lease, "UPDATE " + table + " SET state = 0 WHERE dialog_id = " + key.dialogId);
            }
            execute(lease, "UPDATE topics SET " + counter + " = 0 WHERE did = " + key.dialogId
                    + (topic == 0 ? "" : " AND topic_id = " + topic));
            execute(lease, "UPDATE " + table + "_topics SET state = 0 WHERE dialog_id = " + key.dialogId
                    + (topic == 0 ? "" : " AND topic_id = " + topic)
                    + " AND EXISTS (SELECT 1 FROM messages_topics m WHERE m.uid = " + table + "_topics.dialog_id"
                    + " AND m.mid = " + table + "_topics.message_id AND m.topic_id = " + table + "_topics.topic_id)");
            lease.commitTransactionChecked();
            transaction = false;
            committed = true;
        } catch (Throwable error) {
            FileLog.e(error);
        } finally {
            try {
                if (transaction) {
                    lease.rollbackTransactionChecked();
                }
            } catch (Throwable error) {
                FileLog.e(error);
            } finally {
                owner.policy().completeNativeContentCommit(owner.identity(), token, committed);
            }
        }
        return committed;
    }

    private static void execute(SQLiteDatabase database, String sql) throws SQLiteException {
        SQLitePreparedStatement statement = database.executeFast(sql);
        try {
            statement.stepDone();
        } finally {
            statement.dispose();
        }
    }
}
