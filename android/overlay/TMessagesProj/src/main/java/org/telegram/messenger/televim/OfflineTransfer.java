package org.telegram.messenger.televim;

import android.util.Base64;

import org.json.JSONArray;
import org.json.JSONObject;

import java.io.ByteArrayInputStream;
import java.io.DataInputStream;
import java.io.IOException;
import java.math.BigInteger;
import java.nio.CharBuffer;
import java.nio.ByteBuffer;
import java.nio.charset.CodingErrorAction;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.util.Arrays;
import java.util.HashMap;
import java.util.HashSet;
import java.util.Iterator;
import java.util.LinkedHashMap;
import java.util.Map;
import java.util.Set;
import java.util.UUID;
import java.util.concurrent.atomic.AtomicBoolean;

import javax.crypto.Cipher;
import javax.crypto.SecretKeyFactory;
import javax.crypto.spec.GCMParameterSpec;
import javax.crypto.spec.PBEKeySpec;
import javax.crypto.spec.SecretKeySpec;

public final class OfflineTransfer {
    public static final int MAX_ENCRYPTED = 8 * 1024 * 1024;
    public static final int MAX_JSON = 4 * 1024 * 1024;
    public static final int MAX_MODULE = 2 * 1024 * 1024;
    public static final int MAX_LINK = 8192;
    public static final int MAX_QR = 2900;
    private static final int MAX_SETTINGS = 64 * 1024;
    private static final String LINK_PREFIX = "televim://settings/v1#";
    private static final String SETTINGS_LINK_PREFIX = "televim://settings/v2#";

    public static final class Package {
        public String id;
        public String digest;
        public byte[] original;
        public boolean rawJson;
        public int unsupportedModules;
        public int assets;
        public int records;
        public final Map<String, Boolean> preferences = new LinkedHashMap<>();
        public String folderColorsKey;
        public boolean folderTagsEnabled;
        public final Map<Integer, Integer> folderColors = new LinkedHashMap<>();

        public boolean hasApplicable() {
            return !preferences.isEmpty() || folderColorsKey != null;
        }
        public final Map<String, String> accounts = new LinkedHashMap<>();

        public void clear() {
            if (original != null) {
                Arrays.fill(original, (byte) 0);
                original = null;
            }
        }
    }

    public static byte[] decodeLink(String link, boolean qr) throws Exception {
        int limit = qr ? MAX_QR : MAX_LINK;
        boolean compact = link != null && link.startsWith(SETTINGS_LINK_PREFIX);
        if (link == null || link.length() > limit || !(compact || link.startsWith(LINK_PREFIX))) {
            throw new IOException("Unsupported settings link");
        }
        String encoded = link.substring(compact ? SETTINGS_LINK_PREFIX.length() : LINK_PREFIX.length());
        if (encoded.isEmpty() || !encoded.matches("[A-Za-z0-9_-]+")) {
            throw new IOException("Invalid settings link");
        }
        byte[] bytes = Base64.decode(encoded, Base64.URL_SAFE | Base64.NO_WRAP | Base64.NO_PADDING);
        if (!Base64.encodeToString(bytes, Base64.URL_SAFE | Base64.NO_WRAP | Base64.NO_PADDING).equals(encoded)
                || (compact ? !isEncryptedSettings(bytes) : !isEncryptedArchive(bytes))) {
            Arrays.fill(bytes, (byte) 0);
            throw new IOException("Invalid settings link");
        }
        return bytes;
    }

    public static boolean isEncrypted(byte[] bytes) {
        return isEncryptedArchive(bytes) || isEncryptedSettings(bytes);
    }

    private static boolean isEncryptedArchive(byte[] bytes) {
        return bytes.length >= 56 && startsWith(bytes, "TGVXFR01");
    }

    private static boolean isEncryptedSettings(byte[] bytes) {
        return bytes.length > 56 && bytes.length <= MAX_SETTINGS && startsWith(bytes, "TGVSET01");
    }

    public static Package inspect(byte[] source, char[] password, AtomicBoolean cancelled) throws Exception {
        checkCancelled(cancelled);
        if (source.length > MAX_ENCRYPTED) {
            throw new IOException("Transfer size limit exceeded");
        }
        Package result = new Package();
        result.original = source;
        result.digest = digest(source);
        if (isEncryptedSettings(source)) {
            byte[] plain = decrypt(source, password);
            try {
                checkCancelled(cancelled);
                readSettings(StrictJson.parse(plain, MAX_MODULE, cancelled), result);
                result.id = UUID.nameUUIDFromBytes(source).toString();
            } finally {
                Arrays.fill(plain, (byte) 0);
            }
        } else if (isEncryptedArchive(source)) {
            byte[] plain = decrypt(source, password);
            try {
                checkCancelled(cancelled);
                readArchive(plain, result, cancelled);
            } finally {
                Arrays.fill(plain, (byte) 0);
            }
        } else {
            result.rawJson = true;
            readSettings(StrictJson.parse(source, MAX_MODULE, cancelled), result);
            result.id = UUID.nameUUIDFromBytes(source).toString();
        }
        return result;
    }

    private static byte[] decrypt(byte[] bytes, char[] password) throws Exception {
        if (password == null || password.length == 0) {
            throw new IOException("Password required");
        }
        ByteBuffer utf8 = StandardCharsets.UTF_8.newEncoder()
                .onMalformedInput(CodingErrorAction.REPORT)
                .onUnmappableCharacter(CodingErrorAction.REPORT)
                .encode(CharBuffer.wrap(password));
        int passwordBytes = utf8.remaining();
        while (utf8.hasRemaining()) {
            utf8.put(utf8.position(), (byte) 0);
            utf8.position(utf8.position() + 1);
        }
        if (passwordBytes > 1024 || ByteBuffer.wrap(bytes, 8, 4).getInt() != 600000) {
            throw new IOException("Unsupported password or KDF");
        }
        byte[] salt = Arrays.copyOfRange(bytes, 12, 28);
        PBEKeySpec spec = new PBEKeySpec(password, salt, 600000, 256);
        byte[] key = null;
        try {
            key = SecretKeyFactory.getInstance("PBKDF2WithHmacSHA256").generateSecret(spec).getEncoded();
            Cipher cipher = Cipher.getInstance("AES/GCM/NoPadding");
            cipher.init(Cipher.DECRYPT_MODE, new SecretKeySpec(key, "AES"),
                    new GCMParameterSpec(128, bytes, 28, 12));
            cipher.updateAAD(bytes, 0, 40);
            return cipher.doFinal(bytes, 40, bytes.length - 40);
        } finally {
            spec.clearPassword();
            Arrays.fill(salt, (byte) 0);
            if (key != null) {
                Arrays.fill(key, (byte) 0);
            }
        }
    }

    private static void readArchive(byte[] plain, Package result, AtomicBoolean cancelled) throws Exception {
        if (!startsWith(plain, "TGVPKG01")) {
            throw new IOException("Invalid package stream");
        }
        try (DataInputStream input = new DataInputStream(new ByteArrayInputStream(plain))) {
            byte[] magic = new byte[8];
            input.readFully(magic);
            int manifestSize = input.readInt();
            if (manifestSize < 2 || manifestSize > MAX_MODULE || manifestSize > input.available()) {
                throw new IOException("Invalid manifest size");
            }
            byte[] manifestBytes = new byte[manifestSize];
            input.readFully(manifestBytes);
            JSONObject manifest;
            try {
                manifest = StrictJson.parse(manifestBytes, MAX_MODULE, cancelled);
            } finally {
                Arrays.fill(manifestBytes, (byte) 0);
            }
            require("telegravim-transfer".equals(StrictJson.string(manifest, "format"))
                    && StrictJson.integer(manifest, "version") == 1);
            result.id = uuid(StrictJson.string(manifest, "package_id"));
            uuid(StrictJson.string(manifest, "device_id"));
            uuid(StrictJson.string(manifest, "generation"));
            manifest.getJSONObject("extra");
            retained(manifest, "manifest");
            Map<String, String[]> targets = new HashMap<>();
            Map<String, Long> resources = new HashMap<>();
            Set<String> profileIds = new HashSet<>();
            JSONArray profiles = manifest.getJSONArray("profiles");
            require(profiles.length() <= 32);
            for (int i = 0; i < profiles.length(); ++i) {
                checkCancelled(cancelled);
                JSONObject profile = profiles.getJSONObject(i);
                String id = uuid(StrictJson.string(profile, "profile_id"));
                require(profileIds.add(id));
                String prefix = "profiles/" + id + "/";
                String accountPath = StrictJson.string(profile, "account");
                require(accountPath.equals(prefix + "account.json"));
                require(targets.put(accountPath, new String[]{id, ""}) == null);
                JSONArray modules = profile.getJSONArray("modules");
                require(modules.length() <= 128);
                for (int m = 0; m < modules.length(); ++m) {
                    checkCancelled(cancelled);
                    JSONObject module = modules.getJSONObject(m);
                    String moduleId = StrictJson.string(module, "module_id");
                    require(moduleId.matches("[a-z0-9_]{1,96}"));
                    String path = StrictJson.string(module, "path");
                    require(path.equals(prefix + "modules/" + moduleId + ".json"));
                    require(targets.put(path, new String[]{id, moduleId}) == null);
                }
            }
            JSONArray assets = manifest.getJSONArray("assets");
            require(assets.length() <= 1024);
            for (int i = 0; i < assets.length(); ++i) {
                checkCancelled(cancelled);
                JSONObject asset = assets.getJSONObject(i);
                String hash = hash(StrictJson.string(asset, "sha256"));
                long size = size(asset);
                asset.getJSONObject("extra");
                retained(asset, "registry", "file");
                require(resources.put("assets/" + hash, size) == null);
            }
            result.assets = assets.length();
            JSONArray files = manifest.getJSONArray("files");
            require(files.length() == targets.size() + resources.size() && files.length() <= 5200);
            Set<String> seen = new HashSet<>();
            Set<String> identities = new HashSet<>();
            int jsonBytes = manifestSize;
            for (int i = 0; i < files.length(); ++i) {
                checkCancelled(cancelled);
                JSONObject file = files.getJSONObject(i);
                String path = StrictJson.string(file, "path");
                String hash = hash(StrictJson.string(file, "sha256"));
                long length = size(file);
                require(seen.add(path) && length <= input.available());
                String[] target = targets.get(path);
                Long resource = resources.get(path);
                require(target != null || resource != null);
                if (target != null) {
                    require(length <= MAX_MODULE && jsonBytes + length <= MAX_JSON);
                    jsonBytes += (int) length;
                } else {
                    require(resource == length && path.equals("assets/" + hash));
                }
                byte[] bytes = new byte[(int) length];
                input.readFully(bytes);
                try {
                    require(digest(bytes).equals(hash));
                    if (target == null) {
                        continue;
                    }
                    JSONObject json = StrictJson.parse(bytes, MAX_MODULE, cancelled);
                    if (target[1].isEmpty()) {
                        require(target[0].equals(StrictJson.string(json, "profile_id")));
                        String environment = StrictJson.string(json, "environment");
                        String userId = StrictJson.string(json, "user_id");
                        require(environment.equals("production") || environment.equals("test"));
                        require(userId.matches("[1-9][0-9]{0,19}") && new BigInteger(userId).bitLength() <= 64);
                        require(identities.add(environment + ":" + userId));
                        json.getJSONObject("extra");
                        retained(json, "account", "profile", "modules", "files");
                        result.accounts.put(target[0], environment + ":" + userId);
                    } else {
                        require(target[1].equals(StrictJson.string(json, "module_id")));
                        long version = StrictJson.integer(json, "schema_version");
                        require(version >= 1 && version <= Integer.MAX_VALUE);
                        if (json.opt("records") instanceof JSONArray) {
                            result.records += ((JSONArray) json.get("records")).length();
                            require(result.records <= 10000);
                        }
                        if (target[1].equals("portable_preferences") && version == 1) {
                            readPreferences(json, result, false);
                        } else {
                            result.unsupportedModules++;
                        }
                    }
                } finally {
                    Arrays.fill(bytes, (byte) 0);
                }
            }
            require(input.available() == 0 && result.accounts.size() == profiles.length());
        }
    }

    private static void readSettings(JSONObject object, Package result) throws Exception {
        if (!object.has("format")) {
            readPreferences(object, result, true);
            return;
        }
        require("televim-settings".equals(StrictJson.string(object, "format"))
                && StrictJson.integer(object, "version") == 1);
        require(object.length() == (object.has("account") ? 4 : 3));
        String account = null;
        if (object.has("account")) {
            JSONObject identity = object.getJSONObject("account");
            String environment = StrictJson.string(identity, "environment");
            String user = StrictJson.string(identity, "user_id");
            require(identity.length() == 2
                    && (environment.equals("production") || environment.equals("test"))
                    && user.matches("[1-9][0-9]{0,19}"));
            account = TelevimFolderColors.key(environment, user);
        }
        JSONArray modules = object.getJSONArray("modules");
        require(modules.length() > 0 && modules.length() <= 16);
        Set<String> seen = new HashSet<>();
        for (int i = 0; i < modules.length(); ++i) {
            JSONObject module = modules.getJSONObject(i);
            String id = StrictJson.string(module, "module_id");
            require(seen.add(id));
            if (id.equals("portable_preferences")) {
                readPreferences(module, result, true);
            } else if (id.equals("folder_colors")) {
                require(account != null);
                readFolderColors(module, account, result);
            } else {
                result.unsupportedModules++;
            }
        }
    }

    private static void readFolderColors(JSONObject module, String account, Package result) throws Exception {
        require(StrictJson.integer(module, "schema_version") == 1
                && "account".equals(StrictJson.string(module, "scope"))
                && module.length() == 5);
        Object tags = module.get("tags_enabled");
        require(tags instanceof Boolean);
        JSONArray folders = module.getJSONArray("folders");
        require(folders.length() <= 4096);
        for (int i = 0; i < folders.length(); ++i) {
            JSONObject folder = folders.getJSONObject(i);
            require(folder.length() == 3);
            long id = StrictJson.integer(folder, "filter_id");
            String title = StrictJson.string(folder, "title");
            require(id > 0 && id <= Integer.MAX_VALUE && title.length() <= 256
                    && !result.folderColors.containsKey((int) id));
            Object color = folder.get("color");
            int value;
            if (color == JSONObject.NULL) {
                value = TelevimFolderColors.NO_COLOR;
            } else {
                long index = StrictJson.integer(folder, "color");
                require(index >= 0 && index <= 255);
                if (index > 6) {
                    continue;
                }
                value = (int) index;
            }
            result.folderColors.put((int) id, value);
        }
        result.folderColorsKey = account;
        result.folderTagsEnabled = (Boolean) tags;
    }

    private static void readPreferences(JSONObject module, Package result, boolean strict) throws Exception {
        require("portable_preferences".equals(StrictJson.string(module, "module_id"))
                && StrictJson.integer(module, "schema_version") == 1
                && "application".equals(StrictJson.string(module, "scope"))
                && module.getJSONArray("records").length() == 0);
        if (strict) {
            require(module.length() == 5);
        }
        JSONObject values = module.getJSONObject("values");
        require(values.length() > 0 && values.length() <= 2);
        Iterator<String> keys = values.keys();
        while (keys.hasNext()) {
            String key = keys.next();
            require(key.equals("send_by_enter") || key.equals("large_emoji"));
            Object value = values.get(key);
            require(value instanceof Boolean);
            Boolean previous = result.preferences.get(key);
            require(previous == null || previous.equals(value));
            result.preferences.put(key, (Boolean) value);
        }
    }

    private static void retained(JSONObject object, String... structural) throws Exception {
        if (object.has("retained_transfer")) {
            JSONObject retained = object.getJSONObject("retained_transfer");
            for (String key : structural) {
                require(!retained.has(key));
            }
        }
    }

    private static long size(JSONObject object) throws Exception {
        String text = StrictJson.string(object, "size");
        require(text.matches("0|[1-9][0-9]{0,9}"));
        long value = Long.parseLong(text);
        require(value <= MAX_ENCRYPTED);
        return value;
    }

    private static String hash(String text) throws Exception {
        require(text.matches("[0-9a-f]{64}"));
        return text;
    }

    private static String uuid(String text) throws Exception {
        require(text.matches("[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}")
                && !text.equals("00000000-0000-0000-0000-000000000000"));
        return text;
    }

    static String digest(byte[] bytes) throws Exception {
        byte[] digest = MessageDigest.getInstance("SHA-256").digest(bytes);
        StringBuilder result = new StringBuilder(64);
        for (byte b : digest) {
            result.append(Character.forDigit((b >>> 4) & 15, 16));
            result.append(Character.forDigit(b & 15, 16));
        }
        return result.toString();
    }

    private static boolean startsWith(byte[] bytes, String prefix) {
        byte[] magic = prefix.getBytes(StandardCharsets.US_ASCII);
        if (bytes.length < magic.length) {
            return false;
        }
        for (int i = 0; i < magic.length; ++i) {
            if (bytes[i] != magic[i]) {
                return false;
            }
        }
        return true;
    }

    private static void require(boolean condition) throws IOException {
        if (!condition) {
            throw new IOException("Invalid or incompatible settings package");
        }
    }

    public static void checkCancelled(AtomicBoolean cancelled) throws IOException {
        if ((cancelled != null && cancelled.get()) || Thread.currentThread().isInterrupted()) {
            throw new IOException("Import cancelled");
        }
    }

    private OfflineTransfer() {
    }
}
