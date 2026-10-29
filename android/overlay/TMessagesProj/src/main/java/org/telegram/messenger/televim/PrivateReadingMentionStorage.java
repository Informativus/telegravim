package org.telegram.messenger.televim;

import android.os.Looper;

import org.telegram.SQLite.SQLiteCursor;
import org.telegram.SQLite.SQLiteDatabase;
import org.telegram.SQLite.SQLiteException;
import org.telegram.SQLite.SQLitePreparedStatement;
import org.telegram.messenger.DialogObject;
import org.telegram.messenger.MessagesStorage;

import org.telegram.messenger.televim.PrivateReadingController.AccountHandle;
import org.telegram.messenger.televim.PrivateReadingPolicy.Conversation;
import org.telegram.messenger.televim.PrivateReadingPolicy.RequestToken;

public final class PrivateReadingMentionStorage {
    private PrivateReadingMentionStorage() {
    }

    public static final class Result {
        public final boolean committed;
        public final int dialogBefore;
        public final int dialogAfter;
        public final int topicBefore;
        public final int topicAfter;
        public final Throwable failure;

        private Result(boolean committed, int dialogBefore, int dialogAfter,
                       int topicBefore, int topicAfter, Throwable failure) {
            this.committed = committed;
            this.dialogBefore = dialogBefore;
            this.dialogAfter = dialogAfter;
            this.topicBefore = topicBefore;
            this.topicAfter = topicAfter;
            this.failure = failure;
        }
    }

    public static Result commit(AccountHandle owner, RequestToken token) {
        if (owner == null || token == null || token.identity != owner.identity()
                || token.contentCount() != 1 || token.historyCutoff != 0 || !owner.isCurrent()) {
            return failed(null);
        }
        MessagesStorage storage = owner.storage();
        SQLiteDatabase lease = owner.database();
        if (lease == null || storage.getDatabase() != lease
                || storage.getStorageQueue().getHandler().getLooper() != Looper.myLooper()
                || !owner.policy().reserveNativeContentCommit(owner.identity(), token)) {
            return failed(null);
        }
        boolean transaction = false;
        boolean committed = false;
        int dialogBefore = -1;
        int dialogAfter = -1;
        int topicBefore = -1;
        int topicAfter = -1;
        Throwable failure = null;
        try {
            Conversation key = token.conversation;
            int mid = token.contentIdAt(0);
            long topic = key.subpeerId != 0 ? key.subpeerId : key.rootId;
            if (storage.getDatabase() != lease || DialogObject.isEncryptedDialog(key.dialogId)
                    || mid <= 0 || key.rootId > Integer.MAX_VALUE) {
                throw new SQLiteException("Invalid mention commit scope or database lease");
            }
            lease.beginTransactionChecked();
            transaction = true;
            String parent = "uid = " + key.dialogId + " AND mid = " + mid;
            String scoped = parent + " AND topic_id = " + topic;
            int parentUnread = mentionUnread(lease, "messages_v2", parent);
            int topicUnread = topic == 0 ? -1 : mentionUnread(lease, "messages_topics", scoped);
            if (topic == 0 ? parentUnread < 0 : topicUnread < 0) {
                throw new SQLiteException("Mention membership is unavailable");
            }
            dialogBefore = scalar(lease, "SELECT unread_count_i FROM dialogs WHERE did = " + key.dialogId);
            if (parentUnread >= 0) {
                execute(lease, "UPDATE messages_v2 SET read_state = read_state | 2 WHERE " + parent);
                if (dialogBefore >= parentUnread) {
                    dialogAfter = dialogBefore - parentUnread;
                    execute(lease, "UPDATE dialogs SET unread_count_i = " + dialogAfter
                            + " WHERE did = " + key.dialogId);
                }
            }
            if (topic != 0) {
                topicBefore = scalar(lease, "SELECT unread_mentions FROM topics WHERE did = "
                        + key.dialogId + " AND topic_id = " + topic);
                execute(lease, "UPDATE messages_topics SET read_state = read_state | 2 WHERE " + scoped);
                if (topicBefore >= topicUnread) {
                    topicAfter = topicBefore - topicUnread;
                    execute(lease, "UPDATE topics SET unread_mentions = " + topicAfter
                            + " WHERE did = " + key.dialogId + " AND topic_id = " + topic);
                }
            }
            lease.commitTransactionChecked();
            transaction = false;
            committed = true;
        } catch (Throwable error) {
            failure = error;
            if (transaction) {
                try {
                    lease.rollbackTransactionChecked();
                } catch (Throwable rollbackError) {
                    if (rollbackError != error) {
                        error.addSuppressed(rollbackError);
                    }
                }
            }
        } finally {
            owner.policy().completeNativeContentCommit(owner.identity(), token, committed);
        }
        return committed
                ? new Result(true, dialogBefore, dialogAfter, topicBefore, topicAfter, null)
                : failed(failure);
    }

    private static Result failed(Throwable error) {
        return new Result(false, -1, -1, -1, -1, error);
    }

    public static long revision(AccountHandle owner) throws SQLiteException {
        if (!owner.isCurrent() || owner.database() == null || owner.storage().getDatabase() != owner.database()
                || owner.storage().getStorageQueue().getHandler().getLooper() != Looper.myLooper()) {
            return -1;
        }
        SQLitePreparedStatement statement = owner.database().executeFast("SELECT total_changes()");
        try {
            SQLiteCursor cursor = statement.query(new Object[0]);
            return cursor.nextChecked() ? cursor.longValue(0) : -1;
        } finally {
            statement.dispose();
        }
    }

    public static int dialogCount(AccountHandle owner, long dialogId) throws SQLiteException {
        return revision(owner) < 0 ? -1 : scalar(owner.database(),
                "SELECT unread_count_i FROM dialogs WHERE did = " + dialogId);
    }

    public static boolean refresh(AccountHandle owner, RequestToken token, long topic,
                                  int count, long expectedRevision) {
        SQLiteDatabase lease = owner.database();
        if (count < 0 || expectedRevision < 0 || !owner.isCurrent() || lease == null
                || owner.storage().getDatabase() != lease
                || owner.storage().getStorageQueue().getHandler().getLooper() != Looper.myLooper()
                || !owner.policy().reserveNativeContentReconciliation(owner.identity(), token)) {
            return false;
        }
        boolean transaction = false;
        try {
            lease.beginTransactionChecked();
            transaction = true;
            if (revision(owner) != expectedRevision) {
                return false;
            }
            String row = "did = " + token.conversation.dialogId;
            if (topic == 0) {
                execute(lease, "UPDATE dialogs SET unread_count_i = " + count + " WHERE " + row);
            } else {
                execute(lease, "UPDATE topics SET unread_mentions = " + count + " WHERE "
                        + row + " AND topic_id = " + topic);
            }
            lease.commitTransactionChecked();
            transaction = false;
            return true;
        } catch (Throwable error) {
            return false;
        } finally {
            try {
                if (transaction) {
                    lease.rollbackTransactionChecked();
                }
            } catch (Throwable error) {
                org.telegram.messenger.FileLog.e(error);
            } finally {
                owner.policy().completeNativeContentReconciliation(owner.identity(), token);
            }
        }
    }

    private static int mentionUnread(SQLiteDatabase database, String table, String predicate)
            throws SQLiteException {
        return scalar(database, "SELECT CASE WHEN mention = 1 AND read_state IN (0, 1)"
                + " THEN 1 ELSE 0 END FROM " + table + " WHERE " + predicate);
    }

    private static int scalar(SQLiteDatabase database, String sql) throws SQLiteException {
        SQLitePreparedStatement statement = database.executeFast(sql);
        try {
            SQLiteCursor cursor = statement.query(new Object[0]);
            return cursor.nextChecked() ? cursor.intValue(0) : -1;
        } finally {
            statement.dispose();
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
