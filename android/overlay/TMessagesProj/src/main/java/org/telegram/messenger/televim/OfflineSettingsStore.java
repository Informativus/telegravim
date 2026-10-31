package org.telegram.messenger.televim;

import android.content.Context;
import android.content.SharedPreferences;
import android.os.Looper;
import android.security.keystore.KeyGenParameterSpec;
import android.security.keystore.KeyProperties;
import android.system.Os;
import android.system.OsConstants;

import org.json.JSONArray;
import org.json.JSONException;
import org.json.JSONObject;

import java.io.File;
import java.io.FileDescriptor;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.security.KeyStore;
import java.util.Arrays;
import java.util.ArrayList;
import java.util.HashSet;
import java.util.Map;
import java.util.Set;
import java.util.UUID;
import java.util.concurrent.atomic.AtomicBoolean;

import javax.crypto.Cipher;
import javax.crypto.KeyGenerator;
import javax.crypto.SecretKey;
import javax.crypto.spec.GCMParameterSpec;

public final class OfflineSettingsStore {
    private static final String POINTER = "televim_settings_generation";
    private static final String KEY_ALIAS = "televim.offline.settings.v1";
    private static final long MAX_RETAINED = 64L * 1024 * 1024;
    private static final long MAX_DIRECTORY = 128L * 1024 * 1024;
    private static final int MAX_PACKAGES = 32;
    private static final Object PUBLICATION_LOCK = new Object();
    private static volatile boolean uncertain;

    public static final class Baseline {
        final Map<String, ?> values;

        private Baseline(SharedPreferences preferences) {
            values = preferences.getAll();
        }

        public boolean getBoolean(String key, boolean fallback) {
            Object value = values.get(key);
            return value instanceof Boolean ? (Boolean) value : fallback;
        }
    }

    public static final class Plan {
        public OfflineTransfer.Package transfer;
        public Baseline before;
        public boolean duplicate;
        private String generation;
        private final ArrayList<File> stagedFiles = new ArrayList<>();
        private boolean publicationAttempted;
        private boolean discarded;

        public boolean discard() {
            ArrayList<File> files;
            synchronized (PUBLICATION_LOCK) {
                if (publicationAttempted) {
                    return true;
                }
                discarded = true;
                files = new ArrayList<>(stagedFiles);
            }
            boolean complete = true;
            for (File file : files) {
                if (!file.delete() && file.exists()) {
                    complete = false;
                }
            }
            if (!complete) {
                uncertain = true;
            }
            return complete;
        }
    }

    public enum Applied {
        SUCCESS, DUPLICATE, STALE, UNCERTAIN
    }

    public static Baseline capture(Context context) {
        return new Baseline(preferences(context));
    }

    public static boolean isUncertain() {
        return uncertain;
    }

    public static Plan prepare(Context context, OfflineTransfer.Package transfer, Baseline baseline, AtomicBoolean cancelled) throws Exception {
        OfflineTransfer.checkCancelled(cancelled);
        if (uncertain) {
            throw new IOException("Restart required");
        }
        File directory = directory(context);
        String previous = generation(baseline.values.get(POINTER));
        SecretKey key = key(!previous.isEmpty());
        JSONObject registry = new JSONObject();
        JSONArray archives = new JSONArray();
        if (!previous.isEmpty()) {
            byte[] bytes = readEncrypted(new File(directory, previous + ".registry"), key, 262144, cancelled);
            try {
                registry = StrictJson.parse(bytes, 262144, cancelled);
                if (StrictJson.integer(registry, "version") != 1) {
                    throw new IOException("Unsupported local settings generation");
                }
                archives = registry.getJSONArray("archives");
            } finally {
                Arrays.fill(bytes, (byte) 0);
            }
        }
        Plan result = new Plan();
        result.transfer = transfer;
        result.before = baseline;
        long total = 0;
        Set<String> packageIds = new HashSet<>();
        if (archives.length() > MAX_PACKAGES) {
            throw new IOException("Invalid retained settings registry");
        }
        for (int i = 0; i < archives.length(); ++i) {
            OfflineTransfer.checkCancelled(cancelled);
            JSONObject archive = archives.getJSONObject(i);
            String id = generation(StrictJson.string(archive, "id"));
            String filename = generation(StrictJson.string(archive, "file"));
            long size = StrictJson.integer(archive, "bytes");
            String digest = StrictJson.string(archive, "digest");
            File retained = new File(directory, filename + ".archive");
            if (id.isEmpty() || filename.isEmpty() || !packageIds.add(id) || size < 0
                    || size > OfflineTransfer.MAX_ENCRYPTED || !digest.matches("[0-9a-f]{64}")
                    || !retained.isFile() || retained.length() != size + 28) {
                throw new IOException("Invalid retained settings archive");
            }
            total += size;
            byte[] retainedBytes = readEncrypted(retained, key, OfflineTransfer.MAX_ENCRYPTED, cancelled);
            try {
                if (!OfflineTransfer.digest(retainedBytes).equals(digest)) {
                    throw new IOException("Corrupt retained settings archive");
                }
            } finally {
                Arrays.fill(retainedBytes, (byte) 0);
            }
            if (id.equals(transfer.id)) {
                if (!digest.equals(transfer.digest)) {
                    throw new IOException("Package identity was reused with different content");
                }
                result.duplicate = true;
            }
        }
        if (total > MAX_RETAINED) {
            throw new IOException("Retained settings limit exceeded");
        }
        if (result.duplicate) {
            OfflineTransfer.checkCancelled(cancelled);
            return result;
        }
        if (archives.length() >= MAX_PACKAGES || total + transfer.original.length > MAX_RETAINED) {
            throw new IOException("Retained settings limit exceeded");
        }
        long directoryBytes = 0;
        File[] retainedFiles = directory.listFiles();
        if (retainedFiles == null || retainedFiles.length > 1024) {
            throw new IOException("Local settings storage limit exceeded");
        }
        for (File file : retainedFiles) {
            directoryBytes += file.length();
        }
        if (directoryBytes + transfer.original.length + 262144 > MAX_DIRECTORY) {
            throw new IOException("Local settings storage limit exceeded");
        }
        try {
            OfflineTransfer.checkCancelled(cancelled);
            if (uncertain) {
                throw new IOException("Restart required");
            }
            String archiveName = UUID.randomUUID().toString();
            writeEncrypted(new File(directory, archiveName + ".archive"), key, transfer.original, result, cancelled);
            JSONObject archive = new JSONObject();
            archive.put("id", transfer.id);
            archive.put("file", archiveName);
            archive.put("digest", transfer.digest);
            archive.put("bytes", transfer.original.length);
            archive.put("raw_json", transfer.rawJson);
            archives.put(archive);
            registry.put("version", 1);
            registry.put("archives", archives);
            result.generation = UUID.randomUUID().toString();
            byte[] registryBytes = registry.toString().getBytes(StandardCharsets.UTF_8);
            try {
                if (registryBytes.length > 262144) {
                    throw new IOException("Local settings registry limit exceeded");
                }
                writeEncrypted(new File(directory, result.generation + ".registry"), key, registryBytes, result, cancelled);
                OfflineTransfer.checkCancelled(cancelled);
                syncDirectory(directory);
                OfflineTransfer.checkCancelled(cancelled);
            } finally {
                Arrays.fill(registryBytes, (byte) 0);
            }
            return result;
        } catch (Exception error) {
            result.discard();
            throw error;
        }
    }

    public static Applied apply(Context context, Plan plan) {
        if (Looper.myLooper() != Looper.getMainLooper()) {
            throw new IllegalStateException("Settings publication requires the UI thread");
        }
        synchronized (PUBLICATION_LOCK) {
            if (uncertain) {
                return Applied.UNCERTAIN;
            }
            if (plan.discarded || plan.publicationAttempted) {
                return Applied.STALE;
            }
            SharedPreferences preferences = preferences(context);
            Map<String, ?> now = preferences.getAll();
            String[] names = tracked(plan);
            for (String name : names) {
                if (now.containsKey(name) != plan.before.values.containsKey(name)
                        || (now.containsKey(name) && !now.get(name).equals(plan.before.values.get(name)))) {
                    return Applied.STALE;
                }
            }
            if (plan.duplicate) {
                return Applied.DUPLICATE;
            }
            SharedPreferences.Editor editor = preferences.edit().putString(POINTER, plan.generation);
            for (Map.Entry<String, Boolean> preference : plan.transfer.preferences.entrySet()) {
                String name = preference.getKey().equals("large_emoji") ? "allowBigEmoji" : "send_by_enter";
                editor.putBoolean(name, preference.getValue());
            }
            if (plan.transfer.folderColorsKey != null) {
                editor.putString(plan.transfer.folderColorsKey, folderColors(plan.transfer));
            }
            plan.publicationAttempted = true;
            try {
                if (editor.commit()) {
                    return Applied.SUCCESS;
                }
            } catch (RuntimeException error) {
            }
            uncertain = true;
            try {
                SharedPreferences.Editor rollback = preferences.edit();
                for (String name : names) {
                    Object previous = plan.before.values.get(name);
                    if (!plan.before.values.containsKey(name)) {
                        rollback.remove(name);
                    } else if (previous instanceof Boolean) {
                        rollback.putBoolean(name, (Boolean) previous);
                    } else if (previous instanceof String) {
                        rollback.putString(name, (String) previous);
                    } else if (previous instanceof Integer) {
                        rollback.putInt(name, (Integer) previous);
                    } else if (previous instanceof Long) {
                        rollback.putLong(name, (Long) previous);
                    } else if (previous instanceof Float) {
                        rollback.putFloat(name, (Float) previous);
                    } else if (previous instanceof Set) {
                        Set<String> copy = new HashSet<>();
                        for (Object value : (Set<?>) previous) {
                            copy.add((String) value);
                        }
                        rollback.putStringSet(name, copy);
                    } else {
                        throw new IllegalStateException("Unsupported previous preference value");
                    }
                }
                if (!rollback.commit()) {
                    return Applied.UNCERTAIN;
                }
            } catch (RuntimeException error) {
                return Applied.UNCERTAIN;
            }
            return Applied.UNCERTAIN;
        }
    }

    private static String[] tracked(Plan plan) {
        return plan.transfer.folderColorsKey == null
                ? new String[]{POINTER, "send_by_enter", "allowBigEmoji"}
                : new String[]{POINTER, "send_by_enter", "allowBigEmoji", plan.transfer.folderColorsKey};
    }

    private static String folderColors(OfflineTransfer.Package transfer) {
        try {
            JSONObject colors = new JSONObject();
            for (Map.Entry<Integer, Integer> color : transfer.folderColors.entrySet()) {
                colors.put(String.valueOf(color.getKey()), color.getValue());
            }
            return new JSONObject()
                    .put("tags_enabled", transfer.folderTagsEnabled)
                    .put("colors", colors)
                    .toString();
        } catch (JSONException error) {
            throw new IllegalStateException(error);
        }
    }

    public static SharedPreferences preferences(Context context) {
        return context.getSharedPreferences("mainconfig", Context.MODE_PRIVATE);
    }

    private static String generation(Object value) throws IOException {
        if (value == null) {
            return "";
        }
        if (!(value instanceof String) || !((String) value).matches("[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}")) {
            throw new IOException("Invalid local settings generation");
        }
        return (String) value;
    }

    private static File directory(Context context) throws Exception {
        File directory = new File(context.getNoBackupFilesDir(), "televim-settings");
        if (!directory.isDirectory()) {
            if (!directory.mkdir()) {
                throw new IOException("Could not create local settings storage");
            }
            syncDirectory(context.getNoBackupFilesDir());
        }
        return directory;
    }

    private static SecretKey key(boolean existing) throws Exception {
        KeyStore store = KeyStore.getInstance("AndroidKeyStore");
        store.load(null);
        if (store.containsAlias(KEY_ALIAS)) {
            return (SecretKey) store.getKey(KEY_ALIAS, null);
        }
        if (existing) {
            throw new IOException("Local settings key is unavailable");
        }
        KeyGenerator generator = KeyGenerator.getInstance(KeyProperties.KEY_ALGORITHM_AES, "AndroidKeyStore");
        generator.init(new KeyGenParameterSpec.Builder(KEY_ALIAS,
                KeyProperties.PURPOSE_ENCRYPT | KeyProperties.PURPOSE_DECRYPT)
                .setBlockModes(KeyProperties.BLOCK_MODE_GCM)
                .setEncryptionPaddings(KeyProperties.ENCRYPTION_PADDING_NONE)
                .setKeySize(256).build());
        return generator.generateKey();
    }

    private static void writeEncrypted(File file, SecretKey key, byte[] plain, Plan plan, AtomicBoolean cancelled) throws Exception {
        OfflineTransfer.checkCancelled(cancelled);
        if (!file.createNewFile()) {
            throw new IOException("Local settings generation already exists");
        }
        plan.stagedFiles.add(file);
        Cipher cipher = Cipher.getInstance("AES/GCM/NoPadding");
        cipher.init(Cipher.ENCRYPT_MODE, key);
        if (cipher.getIV().length != 12) {
            throw new IOException("Unsupported local encryption nonce");
        }
        cipher.updateAAD(file.getName().getBytes(StandardCharsets.UTF_8));
        OfflineTransfer.checkCancelled(cancelled);
        byte[] encrypted = cipher.doFinal(plain);
        try (FileOutputStream stream = new FileOutputStream(file)) {
            OfflineTransfer.checkCancelled(cancelled);
            stream.write(cipher.getIV());
            stream.write(encrypted);
            stream.getFD().sync();
        } finally {
            Arrays.fill(encrypted, (byte) 0);
        }
    }

    private static byte[] readEncrypted(File file, SecretKey key, int maximum, AtomicBoolean cancelled) throws Exception {
        OfflineTransfer.checkCancelled(cancelled);
        long size = file.length();
        if (size < 28 || size > maximum + 28) {
            throw new IOException("Invalid local settings file");
        }
        byte[] encrypted = new byte[(int) size];
        try (FileInputStream stream = new FileInputStream(file)) {
            int offset = 0;
            while (offset < encrypted.length) {
                OfflineTransfer.checkCancelled(cancelled);
                int count = stream.read(encrypted, offset, Math.min(16384, encrypted.length - offset));
                if (count < 0) {
                    throw new IOException("Incomplete local settings file");
                }
                offset += count;
            }
            if (stream.read() != -1) {
                throw new IOException("Changed local settings file");
            }
            Cipher cipher = Cipher.getInstance("AES/GCM/NoPadding");
            cipher.init(Cipher.DECRYPT_MODE, key, new GCMParameterSpec(128, encrypted, 0, 12));
            cipher.updateAAD(file.getName().getBytes(StandardCharsets.UTF_8));
            OfflineTransfer.checkCancelled(cancelled);
            return cipher.doFinal(encrypted, 12, encrypted.length - 12);
        } finally {
            Arrays.fill(encrypted, (byte) 0);
        }
    }

    private static void syncDirectory(File directory) throws Exception {
        FileDescriptor descriptor = Os.open(directory.getAbsolutePath(), OsConstants.O_RDONLY | OsConstants.O_NOFOLLOW, 0);
        try {
            if (!OsConstants.S_ISDIR(Os.fstat(descriptor).st_mode)) {
                throw new IOException("Settings storage path is not a directory");
            }
            Os.fsync(descriptor);
        } finally {
            Os.close(descriptor);
        }
    }

    private OfflineSettingsStore() {
    }
}
