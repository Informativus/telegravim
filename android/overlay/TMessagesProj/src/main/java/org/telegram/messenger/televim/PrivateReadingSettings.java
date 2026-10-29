package org.telegram.messenger.televim;

import android.util.AtomicFile;
import java.io.DataInputStream;
import java.io.DataOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.RandomAccessFile;
import java.nio.channels.FileChannel;
import java.nio.channels.FileLock;
import java.util.Locale;
import java.util.Objects;
import java.util.concurrent.Executor;
import java.util.concurrent.RejectedExecutionException;
import java.util.zip.CRC32;
import org.telegram.messenger.televim.PrivateReadingPolicy;

public final class PrivateReadingSettings
implements AutoCloseable {
    private static final int MAGIC = 1414942802;
    private static final int VERSION = 2;
    private static final int RECORD_BYTES = 32;
    public static final String FAILED_ENABLE = "Could not save. Protection is active now, but may be off if Android stops or restarts the app. Retry saving before relying on it later.";
    public static final String FAILED_DISABLE = "Could not save. Private reading remains protected; the change was not saved.";
    private final PrivateReadingPolicy policy;
    private final PrivateReadingPolicy.Identity identity;
    private final Executor executor;
    private final File directory;
    private final AtomicFile file;
    private final File lockFile;
    private boolean scheduled;
    private boolean closed;
    private boolean loadRequested;
    private boolean loadAttempted;
    private boolean loadedStorage;
    private Intent latest;
    private RandomAccessFile lockHandle;
    private FileChannel lockChannel;
    private FileLock namespaceLock;

    public PrivateReadingSettings(File file, PrivateReadingPolicy privateReadingPolicy, Executor executor) {
        this.policy = Objects.requireNonNull(privateReadingPolicy);
        this.identity = privateReadingPolicy.identity();
        this.executor = Objects.requireNonNull(executor);
        this.directory = new File(Objects.requireNonNull(file), "private-reading");
        String string = this.identity.environment.name().toLowerCase(Locale.ROOT) + "-" + this.identity.userId;
        this.file = new AtomicFile(new File(this.directory, string + ".policy"));
        this.lockFile = new File(this.directory, string + ".lock");
    }

    public PrivateReadingPolicy.Status status() {
        return this.policy.status();
    }

    public synchronized boolean load() {
        if (this.closed || !this.policy.owns(this.identity) || this.loadAttempted) {
            return false;
        }
        this.loadRequested = true;
        return this.schedule();
    }

    public synchronized boolean requestSave(PrivateReadingPolicy.Selection selection) {
        if (this.closed || !this.policy.owns(this.identity)) {
            return false;
        }
        long l = this.policy.beginSave(this.identity, Objects.requireNonNull(selection));
        if (l < 0L) {
            return false;
        }
        this.latest = new Intent(l, selection);
        if (!this.loadAttempted) {
            this.loadRequested = true;
        }
        return this.schedule();
    }

    private boolean schedule() {
        if (this.scheduled) {
            return true;
        }
        this.scheduled = true;
        try {
            this.executor.execute(this::drain);
            return true;
        }
        catch (RejectedExecutionException rejectedExecutionException) {
            this.scheduled = false;
            if (this.latest != null) {
                this.policy.saved(this.identity, this.latest.sequence, this.latest.selection, false);
                this.latest = null;
            } else {
                this.policy.loadFailed(this.identity);
            }
            return false;
        }
    }

    /*
     * WARNING - Removed try catching itself - possible behaviour change.
     */
    private void drain() {
        while (true) {
            Intent intent;
            boolean bl;
            PrivateReadingSettings privateReadingSettings = this;
            synchronized (privateReadingSettings) {
                if (this.closed || !this.policy.owns(this.identity)) {
                    this.latest = null;
                    this.releaseNamespace();
                    this.scheduled = false;
                    return;
                }
                boolean bl2 = bl = this.loadRequested && !this.loadAttempted;
                if (bl) {
                    this.loadAttempted = true;
                    this.loadRequested = false;
                    intent = null;
                } else {
                    intent = this.latest;
                    this.latest = null;
                }
                if (!bl && intent == null) {
                    this.scheduled = false;
                    return;
                }
            }
            if (bl) {
                this.readPolicy();
                continue;
            }
            this.writePolicy(intent);
        }
    }

    private void acquireNamespace() throws IOException {
        if (this.namespaceLock != null && this.namespaceLock.isValid()) {
            return;
        }
        if (!this.directory.isDirectory() && !this.directory.mkdirs()) {
            throw new IOException("Policy directory unavailable");
        }
        this.lockHandle = new RandomAccessFile(this.lockFile, "rw");
        this.lockChannel = this.lockHandle.getChannel();
        try {
            this.namespaceLock = this.lockChannel.tryLock();
            if (this.namespaceLock == null) {
                throw new IOException("Policy owner still active");
            }
        }
        catch (IOException | RuntimeException exception) {
            this.releaseNamespace();
            throw new IOException("Policy namespace unavailable", exception);
        }
    }

    private void readPolicy() {
        try {
            PrivateReadingPolicy.Selection selection;
            boolean bl;
            this.acquireNamespace();
            File file = this.file.getBaseFile();
            boolean bl2 = bl = file.exists() || new File(file + ".bak").exists() || new File(file + ".new").exists();
            if (!bl) {
                if (!this.directory.canRead()) {
                    throw new IOException("Policy directory unreadable");
                }
                selection = PrivateReadingPolicy.Selection.off();
            } else {
                try (FileInputStream fileInputStream = this.file.openRead();
                     DataInputStream dataInputStream = new DataInputStream(fileInputStream);){
                    if (fileInputStream.getChannel().size() != RECORD_BYTES) {
                        throw new IOException("Invalid policy size");
                    }
                    int magic = dataInputStream.readInt();
                    int version = dataInputStream.readInt();
                    int environment = dataInputStream.readInt();
                    long userId = dataInputStream.readLong();
                    int flags = dataInputStream.readInt();
                    long checksum = dataInputStream.readLong();
                    int allowedFlags = version == 1 ? 15 : 31;
                    if (magic != MAGIC || (version != 1 && version != VERSION)
                            || environment != this.identity.environment.ordinal()
                            || userId != this.identity.userId || (flags & ~allowedFlags) != 0
                            || dataInputStream.read() != -1
                            || checksum != checksum(version, environment, userId, flags)) {
                        throw new IOException("Invalid policy record");
                    }
                    boolean privateChats = (flags & 1) != 0;
                    boolean bots = version == 1 ? privateChats : (flags & 16) != 0;
                    selection = new PrivateReadingPolicy.Selection(
                            (flags & 8) != 0, privateChats, (flags & 2) != 0, (flags & 4) != 0, bots);
                }
            }
            this.loadedStorage = true;
            this.policy.loaded(this.identity, selection);
        }
        catch (IOException | RuntimeException exception) {
            this.policy.loadFailed(this.identity);
        }
    }

    /*
     * WARNING - Removed try catching itself - possible behaviour change.
     */
    private void writePolicy(Intent intent) {
        FileOutputStream fileOutputStream = null;
        boolean bl = false;
        try {
            this.acquireNamespace();
            Object object = this.policy;
            synchronized (object) {
                block17: {
                    if (this.policy.owns(this.identity)) break block17;
                    return;
                }
                fileOutputStream = this.file.startWrite();
            }
            object = new DataOutputStream(fileOutputStream);
            int n = PrivateReadingSettings.flags(intent.selection);
            ((DataOutputStream)object).writeInt(MAGIC);
            ((DataOutputStream)object).writeInt(VERSION);
            ((DataOutputStream)object).writeInt(this.identity.environment.ordinal());
            ((DataOutputStream)object).writeLong(this.identity.userId);
            ((DataOutputStream)object).writeInt(n);
            ((DataOutputStream)object).writeLong(PrivateReadingSettings.checksum(VERSION, this.identity.environment.ordinal(), this.identity.userId, n));
            ((DataOutputStream)object).flush();
            fileOutputStream.getFD().sync();
            PrivateReadingPolicy privateReadingPolicy = this.policy;
            synchronized (privateReadingPolicy) {
                block18: {
                    if (this.policy.owns(this.identity)) break block18;
                    this.file.failWrite(fileOutputStream);
                    fileOutputStream = null;
                    return;
                }
                this.file.finishWrite(fileOutputStream);
                fileOutputStream = null;
                this.verifyPublished(intent.selection);
                bl = true;
                this.loadedStorage = true;
                this.policy.saved(this.identity, intent.sequence, intent.selection, true);
            }
        }
        catch (IOException | RuntimeException exception) {
            if (fileOutputStream != null) {
                this.file.failWrite(fileOutputStream);
            }
        }
        finally {
            if (!bl) {
                this.policy.saved(this.identity, intent.sequence, intent.selection, false);
            }
        }
    }

    private void verifyPublished(PrivateReadingPolicy.Selection selection) throws IOException {
        try (FileInputStream fileInputStream = this.file.openRead();
             DataInputStream dataInputStream = new DataInputStream(fileInputStream);){
            int n = PrivateReadingSettings.flags(selection);
            if (fileInputStream.getChannel().size() != RECORD_BYTES || dataInputStream.readInt() != MAGIC || dataInputStream.readInt() != VERSION || dataInputStream.readInt() != this.identity.environment.ordinal() || dataInputStream.readLong() != this.identity.userId || dataInputStream.readInt() != n || dataInputStream.readLong() != PrivateReadingSettings.checksum(VERSION, this.identity.environment.ordinal(), this.identity.userId, n) || dataInputStream.read() != -1) {
                throw new IOException("Policy publication failed");
            }
        }
    }

    private static int flags(PrivateReadingPolicy.Selection selection) {
        return (selection.master ? 8 : 0) | (selection.privateChats ? 1 : 0) | (selection.groups ? 2 : 0) | (selection.channels ? 4 : 0) | (selection.bots ? 16 : 0);
    }

    private static long checksum(int version, int n, long l, int n2) {
        CRC32 cRC32 = new CRC32();
        PrivateReadingSettings.update(cRC32, MAGIC, 4);
        PrivateReadingSettings.update(cRC32, version, 4);
        PrivateReadingSettings.update(cRC32, n, 4);
        PrivateReadingSettings.update(cRC32, l, 8);
        PrivateReadingSettings.update(cRC32, n2, 4);
        return cRC32.getValue();
    }

    private static void update(CRC32 cRC32, long l, int n) {
        for (int i = n - 1; i >= 0; --i) {
            cRC32.update((int)(l >>> 8 * i) & 0xFF);
        }
    }

    private void releaseNamespace() {
        try {
            if (this.namespaceLock != null) {
                this.namespaceLock.release();
            }
        }
        catch (IOException iOException) {
            this.policy.loadFailed(this.identity);
        }
        try {
            if (this.lockChannel != null) {
                this.lockChannel.close();
            }
            if (this.lockHandle != null) {
                this.lockHandle.close();
            }
        }
        catch (IOException iOException) {
            this.policy.loadFailed(this.identity);
        }
        this.namespaceLock = null;
        this.lockChannel = null;
        this.lockHandle = null;
    }

    @Override
    public synchronized void close() {
        if (this.closed) {
            return;
        }
        this.policy.invalidate(this.identity);
        this.closed = true;
        this.latest = null;
        if (!this.scheduled) {
            this.scheduled = true;
            try {
                this.executor.execute(this::drain);
            }
            catch (RejectedExecutionException rejectedExecutionException) {
                this.releaseNamespace();
                this.scheduled = false;
            }
        }
    }

    private static final class Intent {
        final long sequence;
        final PrivateReadingPolicy.Selection selection;

        Intent(long l, PrivateReadingPolicy.Selection selection) {
            this.sequence = l;
            this.selection = selection;
        }
    }
}
