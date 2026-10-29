package org.telegram.messenger.televim;

import android.os.Looper;

import org.telegram.SQLite.SQLiteCursor;
import org.telegram.SQLite.SQLiteDatabase;
import org.telegram.SQLite.SQLiteException;
import org.telegram.SQLite.SQLitePreparedStatement;
import org.telegram.messenger.ApplicationLoader;
import org.telegram.messenger.DialogObject;
import org.telegram.messenger.FileLog;
import org.telegram.messenger.MessageObject;
import org.telegram.tgnet.NativeByteBuffer;
import org.telegram.tgnet.TLRPC;

import org.telegram.messenger.televim.PrivateReadingController.AccountHandle;
import org.telegram.messenger.televim.PrivateReadingPolicy.Conversation;
import org.telegram.messenger.televim.PrivateReadingPolicy.RequestToken;

public final class PrivateReadingMarkerOperation {
    private PrivateReadingMarkerOperation() {
    }

    public static void start(AccountHandle owner, Conversation conversation, int mid,
            boolean reactions, Runnable projection) {
        if (owner == null || !owner.isCurrent() || conversation == null || !reactions && conversation.subpeerId != 0) {
            return;
        }
        if (reactions && owner.blocksContent(conversation.dialogId, conversation.rootId, conversation.subpeerId, mid)) {
            return;
        }
        PrivateReadingPolicy.Result<RequestToken> reserved = owner.policy().reserveNativeMarker(
                owner.identity(), conversation, mid, reactions);
        if (!reserved.accepted()) {
            return;
        }
        RequestToken token = reserved.value;
        try {
            if (owner.storage().getStorageQueue().postRunnable(() -> {
                if (!commit(owner, token, reactions)) {
                    finish(owner, token);
                    return;
                }
                try {
                    if (ApplicationLoader.applicationHandler.post(() -> {
                        try {
                            if (owner.isCurrent() && owner.storage().getDatabase() == owner.database() && projection != null) {
                                projection.run();
                            }
                        } finally {
                            finish(owner, token);
                        }
                    })) {
                        return;
                    }
                } catch (Throwable error) {
                    FileLog.e(error);
                }
                finish(owner, token);
            })) {
                return;
            }
        } catch (Throwable error) {
            FileLog.e(error);
        }
        finish(owner, token);
    }

    private static void finish(AccountHandle owner, RequestToken token) {
        owner.policy().finishNativeContentProjection(owner.identity(), token);
        owner.policy().finishUnclaimedRequest(owner.identity(), token);
    }

    private static boolean commit(AccountHandle owner, RequestToken token, boolean reactions) {
        SQLiteDatabase lease = owner.database();
        if (!owner.isCurrent() || lease == null || owner.storage().getDatabase() != lease
                || owner.storage().getStorageQueue().getHandler().getLooper() != Looper.myLooper()
                || !owner.policy().reserveNativeContentCommit(owner.identity(), token)) {
            return false;
        }
        boolean transaction = false;
        boolean committed = false;
        try {
            Conversation key = token.conversation;
            int mid = token.contentIdAt(0);
            long topic = key.subpeerId != 0 ? key.subpeerId : key.rootId;
            if (DialogObject.isEncryptedDialog(key.dialogId) || mid <= 0 || key.rootId < 0
                    || key.rootId > Integer.MAX_VALUE || owner.storage().getDatabase() != lease) {
                throw new SQLiteException("Invalid marker scope or database lease");
            }
            lease.beginTransactionChecked();
            transaction = true;
            String parent = "uid = " + key.dialogId + " AND mid = " + mid;
            String scoped = parent + " AND topic_id = " + topic;
            TLRPC.Message parentMessage = read(lease, "messages_v2", parent, owner.identity().userId);
            TLRPC.Message topicMessage = topic == 0 ? null
                    : read(lease, "messages_topics", scoped, owner.identity().userId);
            if (topic == 0 ? parentMessage == null : topicMessage == null) {
                throw new SQLiteException("Marker membership is unavailable");
            }
            String table = reactions ? "reaction_mentions" : "poll_votes_mentions";
            int unread = markerState(lease, table + (topic == 0 ? "" : "_topics"), key.dialogId, topic, mid);
            TLRPC.Message scopedMessage = topic == 0 ? parentMessage : topicMessage;
            if (unread != 1 && !(unread < 0 && reactions && MessageObject.hasUnreadReactions(scopedMessage))) {
                return false;
            }
            execute(lease, "UPDATE " + table + " SET state = 0 WHERE dialog_id = "
                    + key.dialogId + " AND message_id = " + mid);
            if (topic != 0) {
                execute(lease, "UPDATE " + table + "_topics SET state = 0 WHERE dialog_id = "
                        + key.dialogId + " AND message_id = " + mid + " AND topic_id = " + topic);
            }
            if (reactions) {
                writeReactions(lease, "messages_v2", parent, parentMessage);
                if (topic != 0) {
                    writeReactions(lease, "messages_topics", scoped, topicMessage);
                }
            }
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

    private static int markerState(SQLiteDatabase database, String table, long dialog, long topic,
            int mid) throws SQLiteException {
        SQLitePreparedStatement statement = database.executeFast("SELECT state FROM " + table
                + " WHERE dialog_id = " + dialog + " AND message_id = " + mid
                + (topic == 0 ? "" : " AND topic_id = " + topic));
        try {
            SQLiteCursor cursor = statement.query(new Object[0]);
            return cursor.nextChecked() ? cursor.intValue(0) : -1;
        } finally {
            statement.dispose();
        }
    }

    private static TLRPC.Message read(SQLiteDatabase database, String table, String predicate,
            long userId) throws Exception {
        SQLitePreparedStatement statement = database.executeFast("SELECT data FROM " + table + " WHERE " + predicate);
        NativeByteBuffer data = null;
        try {
            SQLiteCursor cursor = statement.query(new Object[0]);
            if (!cursor.nextChecked() || (data = cursor.byteBufferValue(0)) == null) {
                return null;
            }
            TLRPC.Message message = TLRPC.Message.TLdeserialize(data, data.readInt32(true), true);
            message.readAttachPath(data, userId);
            return message;
        } finally {
            if (data != null) {
                data.reuse();
            }
            statement.dispose();
        }
    }

    private static void writeReactions(SQLiteDatabase database, String table, String predicate,
            TLRPC.Message message) throws Exception {
        if (message == null || message.reactions == null || message.reactions.recent_reactions == null) {
            return;
        }
        for (TLRPC.MessagePeerReaction reaction : message.reactions.recent_reactions) {
            reaction.unread = false;
        }
        MessageObject.normalizeFlags(message);
        NativeByteBuffer data = new NativeByteBuffer(message.getObjectSize());
        try {
            message.serializeToStream(data);
            SQLitePreparedStatement statement = database.executeFast("UPDATE " + table + " SET data = ? WHERE " + predicate);
            try {
                statement.bindByteBuffer(1, data);
                statement.stepDone();
            } finally {
                statement.dispose();
            }
        } finally {
            data.reuse();
        }
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
