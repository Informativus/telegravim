package org.telegram.messenger.televim;

import android.content.Context;
import android.security.keystore.KeyGenParameterSpec;
import android.security.keystore.KeyProperties;
import android.util.AtomicFile;
import android.util.Base64;

import org.json.JSONObject;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.math.BigInteger;
import java.nio.ByteBuffer;
import java.nio.charset.CodingErrorAction;
import java.nio.charset.StandardCharsets;
import java.security.AlgorithmParameters;
import java.security.KeyFactory;
import java.security.KeyPair;
import java.security.KeyPairGenerator;
import java.security.KeyStore;
import java.security.MessageDigest;
import java.security.SecureRandom;
import java.security.Signature;
import java.security.interfaces.ECPrivateKey;
import java.security.interfaces.ECPublicKey;
import java.security.spec.ECGenParameterSpec;
import java.security.spec.ECParameterSpec;
import java.security.spec.ECPoint;
import java.security.spec.ECPublicKeySpec;
import java.security.spec.PKCS8EncodedKeySpec;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Iterator;

import javax.crypto.Cipher;
import javax.crypto.KeyAgreement;
import javax.crypto.KeyGenerator;
import javax.crypto.Mac;
import javax.crypto.SecretKey;
import javax.crypto.spec.GCMParameterSpec;
import javax.crypto.spec.SecretKeySpec;

public final class E2eState {
    private static final int MAX_STATE = 64 * 1024 * 1024;
    private static final byte[] MAGIC = { 'T', 'V', 'S', '2' };
    private static final SecureRandom RANDOM = new SecureRandom();
    private final long accountId;
    private final AtomicFile file;
    private final String alias;
    private final byte[] storageAad;
    private final KeyStore keyStore;
    private JSONObject state;
    private ECPrivateKey privateKey;
    private byte[] publicKey;
    private boolean failed;

    public static final class Received {
        public String text;
        public String kind;
        public boolean positive;
        public final ArrayList<String> replies = new ArrayList<>();
    }

    public E2eState(Context context, long accountId) {
        this.accountId = accountId;
        alias = "televim.e2e.v2." + accountId;
        storageAad = ("telegravim-e2e-state-v2:" + accountId).getBytes(StandardCharsets.UTF_8);
        File directory = new File(context.getNoBackupFilesDir(), "chat-e2e");
        file = new AtomicFile(new File(directory, accountId + ".state"));
        KeyStore loaded = null;
        try {
            require(accountId > 0);
            loaded = KeyStore.getInstance("AndroidKeyStore");
            loaded.load(null);
        } catch (Exception e) {
            failed = true;
        }
        keyStore = loaded;
        if (!failed) {
            try {
                if (file.getBaseFile().exists() || new File(file.getBaseFile() + ".bak").exists()) {
                    load();
                } else {
                    require(!keyStore.containsAlias(alias));
                }
            } catch (Exception e) {
                failed = true;
            }
        }
    }

    private static void require(boolean condition) throws Exception {
        if (!condition) throw new Exception("Invalid encrypted chat state");
    }

    private static long number(String value, boolean zero) throws Exception {
        long result = Long.parseLong(value);
        require(result >= (zero ? 0 : 1) && Long.toString(result).equals(value));
        return result;
    }

    private static byte[] unbase64(String value) throws Exception {
        byte[] result = Base64.decode(value, Base64.NO_WRAP);
        require(base64(result).equals(value));
        return result;
    }

    private static String base64(byte[] value) {
        return Base64.encodeToString(value, Base64.NO_WRAP);
    }

    private static byte[] sha256(byte[] value) throws Exception {
        return MessageDigest.getInstance("SHA-256").digest(value);
    }

    public static String fingerprint(byte[] value) throws Exception {
        byte[] hash = sha256(value);
        char[] output = new char[hash.length * 2];
        String hex = "0123456789ABCDEF";
        for (int i = 0; i < hash.length; i++) {
            output[i * 2] = hex.charAt((hash[i] & 255) >>> 4);
            output[i * 2 + 1] = hex.charAt(hash[i] & 15);
        }
        return new String(output);
    }

    private static ECParameterSpec curve() throws Exception {
        AlgorithmParameters parameters = AlgorithmParameters.getInstance("EC");
        parameters.init(new ECGenParameterSpec("secp256r1"));
        return parameters.getParameterSpec(ECParameterSpec.class);
    }

    private static ECPublicKey decodePublic(byte[] bytes) throws Exception {
        require(bytes.length == 65 && bytes[0] == 4);
        ECParameterSpec curve = curve();
        BigInteger x = new BigInteger(1, Arrays.copyOfRange(bytes, 1, 33));
        BigInteger y = new BigInteger(1, Arrays.copyOfRange(bytes, 33, 65));
        BigInteger p = ((java.security.spec.ECFieldFp) curve.getCurve().getField()).getP();
        require(x.compareTo(p) < 0 && y.compareTo(p) < 0);
        require(y.multiply(y).mod(p).equals(x.multiply(x).multiply(x)
                .add(curve.getCurve().getA().multiply(x)).add(curve.getCurve().getB()).mod(p)));
        return (ECPublicKey) KeyFactory.getInstance("EC").generatePublic(
                new ECPublicKeySpec(new ECPoint(x, y), curve));
    }

    private static byte[] encodePublic(ECPublicKey key) {
        byte[] result = new byte[65];
        result[0] = 4;
        byte[] x = key.getW().getAffineX().toByteArray();
        byte[] y = key.getW().getAffineY().toByteArray();
        System.arraycopy(x, Math.max(0, x.length - 32), result, 33 - Math.min(32, x.length), Math.min(32, x.length));
        System.arraycopy(y, Math.max(0, y.length - 32), result, 65 - Math.min(32, y.length), Math.min(32, y.length));
        return result;
    }

    public static byte[] parsePublic(String input) throws Exception {
        String value = input.trim();
        String expected = null;
        if (value.startsWith("TVE2.KEY.")) {
            String[] parts = value.split("\\.", -1);
            require(parts.length == 4);
            value = parts[2];
            expected = parts[3];
        }
        byte[] result = unbase64(value);
        decodePublic(result);
        require(expected == null || expected.equals(fingerprint(result)));
        return result;
    }

    private SecretKey storageKey(boolean create) throws Exception {
        if (!keyStore.containsAlias(alias)) {
            require(create);
            KeyGenerator generator = KeyGenerator.getInstance(KeyProperties.KEY_ALGORITHM_AES, "AndroidKeyStore");
            generator.init(new KeyGenParameterSpec.Builder(alias,
                    KeyProperties.PURPOSE_ENCRYPT | KeyProperties.PURPOSE_DECRYPT)
                    .setKeySize(256).setBlockModes(KeyProperties.BLOCK_MODE_GCM)
                    .setEncryptionPaddings(KeyProperties.ENCRYPTION_PADDING_NONE).build());
            return generator.generateKey();
        }
        return (SecretKey) keyStore.getKey(alias, null);
    }

    private static String utf8(byte[] value) throws Exception {
        return StandardCharsets.UTF_8.newDecoder().onMalformedInput(CodingErrorAction.REPORT)
                .onUnmappableCharacter(CodingErrorAction.REPORT).decode(ByteBuffer.wrap(value)).toString();
    }

    private void load() throws Exception {
        byte[] encoded;
        try (FileInputStream input = file.openRead(); ByteArrayOutputStream output = new ByteArrayOutputStream()) {
            byte[] chunk = new byte[8192];
            int count;
            while ((count = input.read(chunk)) != -1) {
                require(output.size() + count <= MAX_STATE);
                output.write(chunk, 0, count);
            }
            encoded = output.toByteArray();
        }
        require(encoded.length >= 32 && Arrays.equals(MAGIC, Arrays.copyOfRange(encoded, 0, 4)));
        Cipher cipher = Cipher.getInstance("AES/GCM/NoPadding");
        cipher.init(Cipher.DECRYPT_MODE, storageKey(false), new GCMParameterSpec(128, encoded, 4, 12));
        cipher.updateAAD(storageAad);
        byte[] plain = cipher.doFinal(encoded, 16, encoded.length - 16);
        try {
            state = new JSONObject(utf8(plain));
        } finally {
            Arrays.fill(plain, (byte) 0);
        }
        require(state.getInt("version") == 2 && number(state.getString("account"), false) == accountId);
        byte[] privateBytes = unbase64(state.getString("private"));
        try {
            privateKey = (ECPrivateKey) KeyFactory.getInstance("EC").generatePrivate(new PKCS8EncodedKeySpec(privateBytes));
        } finally {
            Arrays.fill(privateBytes, (byte) 0);
        }
        require(privateKey.getS().signum() > 0 && privateKey.getS().compareTo(curve().getOrder()) < 0);
        publicKey = unbase64(state.getString("public"));
        Signature proof = Signature.getInstance("SHA256withECDSA");
        proof.initSign(privateKey);
        proof.update(storageAad);
        byte[] signed = proof.sign();
        proof.initVerify(decodePublic(publicKey));
        proof.update(storageAad);
        require(proof.verify(signed));
        JSONObject peers = state.getJSONObject("peers");
        for (Iterator<String> i = peers.keys(); i.hasNext();) {
            String id = i.next();
            require(number(id, false) != accountId);
            JSONObject peer = peers.getJSONObject(id);
            JSONObject keys = peer.getJSONObject("keys");
            require(keys.has(peer.getString("active")));
            boolean requested = peer.getBoolean("requested");
            boolean ready = peer.getBoolean("ready");
            boolean changed = peer.getBoolean("changed");
            String challenge = peer.getString("challenge");
            String last = peer.getString("lastChallenge");
            require((!requested || validChallenge(challenge)) && (!ready || requested && !changed));
            require(challenge.isEmpty() || validChallenge(challenge));
            require(last.isEmpty() || validChallenge(last));
            for (Iterator<String> k = keys.keys(); k.hasNext();) {
                String fingerprint = k.next();
                JSONObject key = keys.getJSONObject(fingerprint);
                byte[] bytes = unbase64(key.getString("public"));
                decodePublic(bytes);
                require(fingerprint(bytes).equals(fingerprint));
                long sent = number(key.getString("sent"), true);
                long control = number(key.getString("control"), true);
                validateReceipts(key.getJSONObject("incoming"), false, 0);
                validateReceipts(key.getJSONObject("outgoing"), true, sent);
                require(control == 0 || key.getJSONObject("incoming").has(Long.toString(control)));
            }
        }
    }

    private static void validateReceipts(JSONObject receipts, boolean outgoing, long sent) throws Exception {
        for (Iterator<String> i = receipts.keys(); i.hasNext();) {
            String sequence = i.next();
            long n = number(sequence, false);
            require(!outgoing || n <= sent);
            JSONObject receipt = receipts.getJSONObject(sequence);
            number(receipt.getString("id"), outgoing);
            require(unbase64(receipt.getString("digest")).length == 32);
        }
    }

    private void persist() throws Exception {
        require(!failed && state != null);
        FileOutputStream output = null;
        byte[] plain = state.toString().getBytes(StandardCharsets.UTF_8);
        try {
            require(plain.length + 32 <= MAX_STATE);
            File directory = file.getBaseFile().getParentFile();
            require(directory.isDirectory() || directory.mkdirs());
            require(directory.setReadable(false, false) && directory.setWritable(false, false)
                    && directory.setExecutable(false, false) && directory.setReadable(true, true)
                    && directory.setWritable(true, true) && directory.setExecutable(true, true));
            Cipher cipher = Cipher.getInstance("AES/GCM/NoPadding");
            cipher.init(Cipher.ENCRYPT_MODE, storageKey(true));
            cipher.updateAAD(storageAad);
            byte[] encrypted = cipher.doFinal(plain);
            require(cipher.getIV().length == 12);
            output = file.startWrite();
            output.write(MAGIC);
            output.write(cipher.getIV());
            output.write(encrypted);
            output.getFD().sync();
            file.finishWrite(output);
            output = null;
            try (FileInputStream saved = file.openRead()) {
                ByteArrayOutputStream expected = new ByteArrayOutputStream();
                expected.write(MAGIC);
                expected.write(cipher.getIV());
                expected.write(encrypted);
                byte[] bytes = expected.toByteArray();
                byte[] actual = new byte[8192];
                int offset = 0;
                int count;
                while ((count = saved.read(actual)) != -1) {
                    require(offset + count <= bytes.length);
                    for (int i = 0; i < count; i++) require(actual[i] == bytes[offset + i]);
                    offset += count;
                }
                require(offset == bytes.length);
            }
            require(file.getBaseFile().setReadable(false, false)
                    && file.getBaseFile().setWritable(false, false)
                    && file.getBaseFile().setReadable(true, true)
                    && file.getBaseFile().setWritable(true, true));
        } catch (Exception e) {
            if (output != null) file.failWrite(output);
            failed = true;
            throw e;
        } finally {
            Arrays.fill(plain, (byte) 0);
        }
    }

    public synchronized String invitation() throws Exception {
        require(!failed);
        if (state == null) {
            KeyPairGenerator generator = KeyPairGenerator.getInstance("EC");
            generator.initialize(new ECGenParameterSpec("secp256r1"), RANDOM);
            KeyPair pair = generator.generateKeyPair();
            privateKey = (ECPrivateKey) pair.getPrivate();
            publicKey = encodePublic((ECPublicKey) pair.getPublic());
            state = new JSONObject().put("version", 2).put("account", Long.toString(accountId))
                    .put("private", base64(privateKey.getEncoded())).put("public", base64(publicKey))
                    .put("peers", new JSONObject());
            persist();
        }
        return "TVE2.KEY." + base64(publicKey) + "." + fingerprint(publicKey);
    }

    private JSONObject peer(long id) {
        if (state == null) return null;
        return state.optJSONObject("peers").optJSONObject(Long.toString(id));
    }

    public synchronized boolean enabled(long id) {
        if (id <= 0 || id == accountId) return false;
        JSONObject peer = peer(id);
        return failed || peer != null && peer.optBoolean("requested");
    }

    public synchronized boolean ready(long id) {
        JSONObject peer = peer(id);
        return !failed && peer != null && peer.optBoolean("requested")
                && peer.optBoolean("ready") && !peer.optBoolean("changed");
    }

    public synchronized boolean hasPeer(long id) {
        return !failed && peer(id) != null;
    }

    public synchronized String peerKey(long id) throws Exception {
        JSONObject peer = peer(id);
        return peer == null ? "" : peer.getJSONObject("keys").getJSONObject(peer.getString("active")).getString("public");
    }

    public synchronized void trust(long id, byte[] keyBytes) throws Exception {
        require(id > 0 && id != accountId);
        decodePublic(keyBytes);
        invitation();
        String fingerprint = fingerprint(keyBytes);
        JSONObject peer = peer(id);
        if (peer == null) {
            peer = new JSONObject().put("keys", new JSONObject()).put("requested", false);
            state.getJSONObject("peers").put(Long.toString(id), peer);
        } else if (fingerprint.equals(peer.getString("active")) && !peer.getBoolean("changed")) {
            return;
        }
        JSONObject keys = peer.getJSONObject("keys");
        if (!keys.has(fingerprint)) {
            keys.put(fingerprint, new JSONObject().put("public", base64(keyBytes))
                    .put("sent", "0").put("control", "0")
                    .put("incoming", new JSONObject()).put("outgoing", new JSONObject()));
        }
        peer.put("active", fingerprint).put("ready", false).put("changed", false)
                .put("challenge", peer.getBoolean("requested") ? challenge() : "").put("lastChallenge", "");
        persist();
    }

    public synchronized String toggle(long id, boolean enable) throws Exception {
        JSONObject peer = peer(id);
        require(!failed && peer != null && (!enable || !peer.getBoolean("changed")));
        peer.put("requested", enable).put("ready", false).put("lastChallenge", "")
                .put("challenge", enable ? challenge() : "");
        persist();
        return control(id);
    }

    public synchronized String control(long id) throws Exception {
        JSONObject peer = peer(id);
        require(!failed && peer != null);
        return encrypt(id, peer.getBoolean("requested") ? "ON" : "OFF", peer.getString("challenge"));
    }

    private static String challenge() {
        byte[] bytes = new byte[16];
        RANDOM.nextBytes(bytes);
        return base64(bytes);
    }

    private static boolean validChallenge(String value) {
        try {
            return unbase64(value).length == 16;
        } catch (Exception e) {
            return false;
        }
    }

    private byte[] derive(byte[] peerKey, long peerId, boolean outgoing) throws Exception {
        KeyAgreement agreement = KeyAgreement.getInstance("ECDH");
        agreement.init(privateKey);
        agreement.doPhase(decodePublic(peerKey), true);
        byte[] shared = agreement.generateSecret();
        byte[] prk = null;
        try {
            require(shared.length == 32);
            Mac mac = Mac.getInstance("HmacSHA256");
            mac.init(new SecretKeySpec(new byte[32], "HmacSHA256"));
            prk = mac.doFinal(shared);
            mac.init(new SecretKeySpec(prk, "HmacSHA256"));
            mac.update("telegravim-e2e-v2\0".getBytes(StandardCharsets.UTF_8));
            mac.update(outgoing ? publicKey : peerKey);
            mac.update(outgoing ? peerKey : publicKey);
            mac.update(((outgoing ? accountId : peerId) + ":" + (outgoing ? peerId : accountId)).getBytes(StandardCharsets.UTF_8));
            mac.update((byte) 1);
            return mac.doFinal();
        } finally {
            Arrays.fill(shared, (byte) 0);
            if (prk != null) Arrays.fill(prk, (byte) 0);
        }
    }

    private byte[] crypt(int mode, byte[] key, byte[] nonce, byte[] data, String header) throws Exception {
        try {
            Cipher cipher = Cipher.getInstance("AES/GCM/NoPadding");
            cipher.init(mode, new SecretKeySpec(key, "AES"), new GCMParameterSpec(128, nonce));
            cipher.updateAAD(header.getBytes(StandardCharsets.UTF_8));
            return cipher.doFinal(data);
        } finally {
            Arrays.fill(key, (byte) 0);
        }
    }

    private String encrypt(long id, String kind, String plain) throws Exception {
        JSONObject peer = peer(id);
        require(!failed && peer != null && !peer.getBoolean("changed"));
        JSONObject key = peer.getJSONObject("keys").getJSONObject(peer.getString("active"));
        long sent = number(key.getString("sent"), true);
        require(sent < Long.MAX_VALUE);
        String sequence = Long.toString(sent + 1);
        String header = "TVE2." + kind + "." + accountId + "." + id + "."
                + fingerprint(publicKey) + "." + peer.getString("active") + "." + sequence;
        byte[] nonce = new byte[12];
        RANDOM.nextBytes(nonce);
        byte[] encrypted = crypt(Cipher.ENCRYPT_MODE, derive(unbase64(key.getString("public")), id, true),
                nonce, plain.getBytes(StandardCharsets.UTF_8), header);
        String wire = header + "." + base64(nonce) + "." + base64(encrypted);
        key.put("sent", sequence);
        key.getJSONObject("outgoing").put(sequence, new JSONObject().put("id", "0")
                .put("digest", base64(sha256(wire.getBytes(StandardCharsets.UTF_8)))));
        persist();
        return wire;
    }

    public synchronized String encryptText(long id, String text) throws Exception {
        require(ready(id) && text != null && !text.isEmpty());
        return encrypt(id, "MSG", text);
    }

    public synchronized boolean owns(long id, String wire) {
        try {
            JSONObject peer = peer(id);
            if (failed || peer == null || wire == null) return false;
            String[] parts = wire.split("\\.", -1);
            if (parts.length != 9 || !parts[0].equals("TVE2")
                    || number(parts[2], false) != accountId || number(parts[3], false) != id
                    || !parts[4].equals(fingerprint(publicKey))) return false;
            JSONObject key = peer.getJSONObject("keys").getJSONObject(parts[5]);
            JSONObject receipt = key.getJSONObject("outgoing").optJSONObject(parts[6]);
            return receipt != null && receipt.getString("digest").equals(base64(sha256(wire.getBytes(StandardCharsets.UTF_8))));
        } catch (Exception e) {
            return false;
        }
    }

    public synchronized boolean encryptedId(long id, int messageId) {
        JSONObject peer = peer(id);
        if (peer == null || messageId <= 0) return false;
        try {
            JSONObject keys = peer.getJSONObject("keys");
            for (Iterator<String> k = keys.keys(); k.hasNext();) {
                JSONObject key = keys.getJSONObject(k.next());
                for (String direction : new String[] { "incoming", "outgoing" }) {
                    JSONObject receipts = key.getJSONObject(direction);
                    for (Iterator<String> r = receipts.keys(); r.hasNext();) {
                        if (receipts.getJSONObject(r.next()).getString("id").equals(Integer.toString(messageId))) return true;
                    }
                }
            }
        } catch (Exception e) {
            failed = true;
        }
        return false;
    }

    public synchronized boolean observeKey(long id, String wire, boolean live) throws Exception {
        byte[] bytes = parsePublic(wire);
        JSONObject peer = peer(id);
        boolean changed = peer != null && !peer.getString("active").equals(fingerprint(bytes));
        if (changed && live && !peer.getBoolean("changed")) {
            peer.put("changed", true).put("ready", false);
            persist();
        }
        return changed;
    }

    public synchronized Received receive(long id, int messageId, boolean outgoing, boolean live, String wire) throws Exception {
        JSONObject peer = peer(id);
        require(!failed && peer != null && privateKey != null && (messageId > 0 || outgoing && owns(id, wire)));
        String[] parts = wire.split("\\.", -1);
        require(parts.length == 9 && parts[0].equals("TVE2"));
        String kind = parts[1];
        require(kind.equals("MSG") || kind.equals("ON") || kind.equals("ACK") || kind.equals("OFF"));
        require(number(parts[2], false) == (outgoing ? accountId : id)
                && number(parts[3], false) == (outgoing ? id : accountId)
                && parts[outgoing ? 4 : 5].equals(fingerprint(publicKey)));
        long sequence = number(parts[6], false);
        String fingerprint = parts[outgoing ? 5 : 4];
        JSONObject key = peer.getJSONObject("keys").getJSONObject(fingerprint);
        byte[] nonce = unbase64(parts[7]);
        byte[] ciphertext = unbase64(parts[8]);
        require(nonce.length == 12 && ciphertext.length >= 16);
        String header = wire.substring(0, wire.length() - parts[7].length() - parts[8].length() - 2);
        String plain = utf8(crypt(Cipher.DECRYPT_MODE, derive(unbase64(key.getString("public")), id, outgoing), nonce, ciphertext, header));
        require(!kind.equals("ON") || validChallenge(plain));
        require(!kind.equals("OFF") || plain.isEmpty());
        require(!kind.equals("ACK") || plain.length() > 2 && (plain.endsWith(":0") || plain.endsWith(":1"))
                && validChallenge(plain.substring(0, plain.length() - 2)));
        JSONObject receipts = key.getJSONObject(outgoing ? "outgoing" : "incoming");
        JSONObject receipt = receipts.optJSONObject(parts[6]);
        String digest = base64(sha256(wire.getBytes(StandardCharsets.UTF_8)));
        boolean fresh = receipt == null;
        int boundId = Math.max(0, messageId);
        require(fresh || receipt.getString("digest").equals(digest)
                && (receipt.getString("id").equals("0") || receipt.getString("id").equals(Integer.toString(boundId))));
        boolean apply = fresh && live && !outgoing && !kind.equals("MSG")
                && fingerprint.equals(peer.getString("active")) && !peer.getBoolean("changed")
                && sequence > number(key.getString("control"), true);
        boolean ack = false;
        boolean repeat = false;
        if (apply) {
            key.put("control", parts[6]);
            if (kind.equals("ON")) {
                ack = true;
                repeat = peer.getBoolean("requested") && !peer.getBoolean("ready") && !plain.equals(peer.getString("lastChallenge"));
                peer.put("lastChallenge", plain);
            } else if (kind.equals("OFF")) {
                peer.put("ready", false);
            } else if (peer.getBoolean("requested") && plain.substring(0, plain.length() - 2).equals(peer.getString("challenge"))) {
                peer.put("ready", plain.endsWith(":1"));
            }
        }
        if (fresh || !receipt.getString("id").equals(Integer.toString(boundId))) {
            receipts.put(parts[6], new JSONObject().put("digest", digest).put("id", Integer.toString(boundId)));
            if (outgoing) key.put("sent", Long.toString(Math.max(sequence, number(key.getString("sent"), true))));
            persist();
        }
        Received result = new Received();
        result.kind = kind;
        result.text = plain;
        result.positive = kind.equals("ACK") && plain.endsWith(":1");
        if (ack) result.replies.add(encrypt(id, "ACK", plain + (peer.getBoolean("requested") ? ":1" : ":0")));
        if (repeat) result.replies.add(control(id));
        return result;
    }

    public synchronized void clear() {
        failed = true;
        state = null;
        privateKey = null;
        publicKey = null;
        file.delete();
        try {
            if (keyStore != null) keyStore.deleteEntry(alias);
        } catch (Exception ignored) {
        }
    }
}
