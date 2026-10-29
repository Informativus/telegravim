package org.telegram.messenger.televim;

import android.os.Looper;

import org.telegram.SQLite.SQLiteCursor;
import org.telegram.SQLite.SQLiteDatabase;
import org.telegram.SQLite.SQLiteException;
import org.telegram.SQLite.SQLitePreparedStatement;
import org.telegram.messenger.DialogObject;
import org.telegram.messenger.MessageObject;
import org.telegram.messenger.MessagesStorage;
import org.telegram.tgnet.NativeByteBuffer;
import org.telegram.tgnet.TLRPC;

import org.telegram.messenger.televim.PrivateReadingController.AccountHandle;
import org.telegram.messenger.televim.PrivateReadingPolicy.CommitReservation;
import org.telegram.messenger.televim.PrivateReadingPolicy.Completion;
import org.telegram.messenger.televim.PrivateReadingPolicy.Conversation;
import org.telegram.messenger.televim.PrivateReadingPolicy.Failure;
import org.telegram.messenger.televim.PrivateReadingPolicy.ReleaseScope;

public final class PrivateReadingStorage {
    private PrivateReadingStorage() {
    }

    public interface CommitCallback {
        void complete(Result result);
    }

    public static final class Result {
        public final ReleaseScope scope;
        public final boolean committed;
        public final Completion completion;
        public final Throwable failure;
        public final Failure policyFailure;
        public final int changedHistory;
        public final int changedMentions;
        public final int changedReactions;
        public final int remainingUnread = -1;
        public final boolean completeBaseline = false;
        public final boolean pollsSupported = false;

        private Result(ReleaseScope scope, boolean committed, Completion completion,
                       Throwable failure, Failure policyFailure,
                       int changedHistory, int changedMentions, int changedReactions) {
            this.scope = scope;
            this.committed = committed;
            this.completion = completion;
            this.failure = failure;
            this.policyFailure = policyFailure;
            this.changedHistory = changedHistory;
            this.changedMentions = changedMentions;
            this.changedReactions = changedReactions;
        }
    }

    public static Result unavailable(ReleaseScope scope, Failure failure) {
        return new Result(scope, false, null, null, failure, 0, 0, 0);
    }

    public static Result commit(MessagesStorage storage, SQLiteDatabase lease, int account,
                                AccountHandle owner, ReleaseScope scope) {
        if (owner == null || scope == null || owner.account() != account
                || scope.identity != owner.identity() || !owner.isCurrent()
                || owner.storage() != storage || owner.database() != lease
                || lease == null || storage.getDatabase() != lease
                || storage.getStorageQueue().getHandler().getLooper() != Looper.myLooper()) {
            return unavailable(scope, Failure.INACTIVE);
        }
        PrivateReadingPolicy policy = owner.policy();
        PrivateReadingPolicy.Result<CommitReservation> reserved = policy.reserveCommit(
                owner.identity(), scope, 0);
        if (!reserved.accepted()) {
            return unavailable(scope, reserved.failure);
        }
        boolean transaction = false;
        boolean committed = false;
        Throwable failure = null;
        int changedHistory = 0;
        int changedMentions = 0;
        int changedReactions = 0;
        try {
            if (storage.getDatabase() != lease) {
                throw new SQLiteException("Private reading database lease expired");
            }
            lease.beginTransactionChecked();
            transaction = true;
            Conversation key = scope.conversation;
            long topic = key.subpeerId != 0 ? key.subpeerId : key.rootId;
            if (scope.historyCutoff <= 0 || DialogObject.isEncryptedDialog(key.dialogId)
                    || key.dialogId == owner.identity().userId
                    || key.rootId > Integer.MAX_VALUE) {
                throw new SQLiteException("Unsupported private reading scope");
            }
            if (topic != 0 && scalar(lease, "SELECT COUNT(*) FROM topics WHERE did = "
                    + key.dialogId + " AND topic_id = " + topic) != 1) {
                throw new SQLiteException("Private reading topic membership is unavailable");
            }
            String table = topic == 0 ? "messages_v2" : "messages_topics";
            String predicate = "uid = " + key.dialogId
                    + (topic == 0 ? "" : " AND topic_id = " + topic);
            String membership = "EXISTS (SELECT 1 FROM messages_topics AS scoped WHERE scoped.uid = "
                    + key.dialogId + " AND scoped.topic_id = " + topic + " AND scoped.mid = messages_v2.mid)";
            String historyRows = predicate + " AND mid > 0 AND mid <= " + scope.historyCutoff;
            changedHistory = scalar(lease, "SELECT COUNT(*) FROM " + table + " WHERE " + historyRows
                    + " AND " + unreadPredicate(topic, false));
            execute(lease, "UPDATE " + table + " SET read_state = read_state | 1 WHERE "
                    + historyRows + " AND out = 0 AND read_state IN (0, 2)");
            if (topic != 0) {
                execute(lease, "UPDATE messages_v2 SET read_state = read_state | 1 WHERE uid = "
                        + key.dialogId + " AND mid > 0 AND mid <= " + scope.historyCutoff
                        + " AND out = 0 AND read_state IN (0, 2) AND " + membership);
                execute(lease, "UPDATE topics SET max_read_id = MAX(max_read_id, " + scope.historyCutoff
                        + ") WHERE did = " + key.dialogId + " AND topic_id = " + topic);
            } else {
                execute(lease, "UPDATE dialogs SET inbox_max = MAX(inbox_max, " + scope.historyCutoff
                        + ") WHERE did = " + key.dialogId);
            }
            for (int i = 0; i < scope.contentCount(); i++) {
                int id = scope.contentIdAt(i);
                if (id <= 0 || id > scope.historyCutoff) {
                    throw new SQLiteException("Invalid private reading content identity");
                }
                String row = predicate + " AND mid = " + id;
                changedMentions += scalar(lease, "SELECT COUNT(*) FROM " + table + " WHERE "
                        + row + " AND " + unreadPredicate(topic, true));
                execute(lease, "UPDATE " + table + " SET read_state = read_state | 2 WHERE "
                        + row + " AND read_state IN (0, 1)");
                if (topic != 0) {
                    execute(lease, "UPDATE messages_v2 SET read_state = read_state | 2 WHERE uid = "
                            + key.dialogId + " AND mid = " + id + " AND read_state IN (0, 1) AND " + membership);
                }
                String reactionTable = topic == 0 ? "reaction_mentions" : "reaction_mentions_topics";
                String reactionRow = "dialog_id = " + key.dialogId + " AND message_id = " + id
                        + (topic == 0 ? "" : " AND topic_id = " + topic);
                boolean reactionChanged = execute(lease, "UPDATE " + reactionTable
                        + " SET state = 0 WHERE " + reactionRow + " AND state = 1") != 0;
                reactionChanged |= writeReactions(lease, table, row, owner.identity().userId);
                if (topic != 0) {
                    reactionChanged |= execute(lease, "UPDATE reaction_mentions SET state = 0 WHERE dialog_id = "
                            + key.dialogId + " AND message_id = " + id + " AND state = 1 AND "
                            + "(EXISTS (SELECT 1 FROM messages_topics WHERE uid = " + key.dialogId
                            + " AND mid = " + id + " AND topic_id = " + topic + ")"
                            + " OR EXISTS (SELECT 1 FROM reaction_mentions_topics WHERE " + reactionRow + "))") != 0;
                    reactionChanged |= writeReactions(lease, "messages_v2", "uid = " + key.dialogId
                            + " AND mid = " + id + " AND " + membership, owner.identity().userId);
                }
                if (reactionChanged) {
                    changedReactions++;
                }
                execute(lease, "DELETE FROM unread_push_messages WHERE uid = " + key.dialogId
                        + " AND mid = " + id + " AND is_reaction = 1" + pushMembership(key, topic));
            }
            execute(lease, "DELETE FROM unread_push_messages WHERE uid = " + key.dialogId
                    + " AND mid > 0 AND mid <= " + scope.historyCutoff
                    + " AND is_reaction = 0" + pushMembership(key, topic));
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
        }
        PrivateReadingPolicy.Result<Completion> completed = policy.completeCommit(owner.identity(), reserved.value,
                committed, committed ? changedHistory : 0, -1, false);
        return new Result(scope, committed, completed.value, failure, completed.failure,
                committed ? changedHistory : 0, committed ? changedMentions : 0, committed ? changedReactions : 0);
    }

    private static String pushMembership(Conversation key, long topic) {
        return topic == 0 ? "" : " AND EXISTS (SELECT 1 FROM messages_topics AS scoped WHERE scoped.uid = "
                + key.dialogId + " AND scoped.topic_id = " + topic + " AND scoped.mid = unread_push_messages.mid)";
    }

    private static String unreadPredicate(long topic, boolean mention) {
        String local = mention ? "out = 0 AND mention = 1 AND read_state IN (0, 1)"
                : "out = 0 AND read_state IN (0, 2)";
        if (topic == 0) {
            return "(" + local + ")";
        }
        return "(" + local + " OR EXISTS (SELECT 1 FROM messages_v2 AS parent WHERE "
                + "parent.uid = messages_topics.uid AND parent.mid = messages_topics.mid AND parent.out = 0 AND "
                + (mention ? "parent.mention = 1 AND parent.read_state IN (0, 1)" : "parent.read_state IN (0, 2)")
                + "))";
    }

    private static int execute(SQLiteDatabase database, String sql) throws SQLiteException {
        try (Statement statement = new Statement(database, sql)) {
            statement.value.stepDone();
            return scalar(database, "SELECT changes()");
        }
    }

    private static int scalar(SQLiteDatabase database, String sql) throws SQLiteException {
        try (Statement statement = new Statement(database, sql)) {
            SQLiteCursor cursor = statement.value.query(new Object[0]);
            if (!cursor.nextChecked()) {
                throw new SQLiteException("Missing private reading scalar result");
            }
            return cursor.intValue(0);
        }
    }

    private static boolean writeReactions(SQLiteDatabase database, String table, String row, long userId)
            throws Exception {
        TLRPC.Message message;
        try (Statement statement = new Statement(database, "SELECT data FROM " + table + " WHERE " + row)) {
            SQLiteCursor cursor = statement.value.query(new Object[0]);
            if (!cursor.nextChecked()) {
                return false;
            }
            try (Buffer buffer = new Buffer(cursor.byteBufferValue(0))) {
                if (buffer.value == null) {
                    throw new SQLiteException("Missing private reading message data");
                }
                message = TLRPC.Message.TLdeserialize(buffer.value, buffer.value.readInt32(true), true);
                if (message == null) {
                    throw new SQLiteException("Invalid private reading message data");
                }
                message.readAttachPath(buffer.value, userId);
            }
        }
        boolean changed = false;
        if (message.reactions != null && message.reactions.recent_reactions != null) {
            for (int i = 0; i < message.reactions.recent_reactions.size(); i++) {
                if (message.reactions.recent_reactions.get(i).unread) {
                    message.reactions.recent_reactions.get(i).unread = false;
                    changed = true;
                }
            }
        }
        if (!changed) {
            return false;
        }
        MessageObject.normalizeFlags(message);
        try (Buffer buffer = new Buffer(new NativeByteBuffer(message.getObjectSize()))) {
            message.serializeToStream(buffer.value);
            try (Statement statement = new Statement(database, "UPDATE " + table + " SET data = ? WHERE " + row)) {
                statement.value.bindByteBuffer(1, buffer.value);
                statement.value.stepDone();
                if (scalar(database, "SELECT changes()") != 1) {
                    throw new SQLiteException("Private reading message row changed");
                }
            }
        }
        return true;
    }

    private static final class Buffer implements AutoCloseable {
        final NativeByteBuffer value;

        Buffer(NativeByteBuffer value) {
            this.value = value;
        }

        @Override
        public void close() {
            if (value != null) {
                value.reuse();
            }
        }
    }

    private static final class Statement implements AutoCloseable {
        final SQLitePreparedStatement value;

        Statement(SQLiteDatabase database, String sql) throws SQLiteException {
            value = database.executeFast(sql);
        }

        @Override
        public void close() {
            value.dispose();
        }
    }
}
