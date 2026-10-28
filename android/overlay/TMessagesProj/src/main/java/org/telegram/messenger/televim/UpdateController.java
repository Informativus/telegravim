package org.telegram.messenger.televim;

import android.app.Activity;
import android.content.Context;
import android.content.Intent;
import android.content.pm.PackageInfo;
import android.content.pm.PackageManager;
import android.content.pm.Signature;
import android.os.Build;
import android.os.SystemClock;
import android.util.Base64;

import androidx.core.content.FileProvider;

import org.json.JSONObject;
import org.telegram.messenger.AndroidUtilities;
import org.telegram.messenger.ApplicationLoader;
import org.telegram.messenger.BetaUpdate;
import org.telegram.messenger.LocaleController;
import org.telegram.messenger.NotificationCenter;
import org.telegram.messenger.R;
import org.telegram.ui.Components.BulletinFactory;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.nio.charset.StandardCharsets;
import java.security.KeyFactory;
import java.security.MessageDigest;
import java.security.spec.X509EncodedKeySpec;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.atomic.AtomicInteger;

public final class UpdateController {
    private static final String FEED = "https://informativus.github.io/telegravim/updates/current6";
    private static final String PUBLIC_KEY = "MFkwEwYHKoZIzj0CAQYIKoZIzj0DAQcDQgAEHTE9g5RU5PYxE5KPaJtFm2v0p/f7lQ/wfm/ma4Jtu4ha6h9eh0Xds/8/o+3+fy6a7eXtzd+nhkn09r11UCzbLQ==";
    private static final int MAX_CATALOG = 1024 * 1024;
    private static final long MAX_APK = 512L * 1024 * 1024;
    private static final UpdateController INSTANCE = new UpdateController();
    private final ExecutorService worker = Executors.newSingleThreadExecutor();
    private final AtomicInteger generation = new AtomicInteger();
    private final List<Runnable> callbacks = new ArrayList<>();
    private boolean checking;
    private boolean error;
    private long lastAttempt;
    private long lastSuccess;
    private Entry available;
    private File downloaded;
    private volatile boolean downloading;
    private volatile float progress;
    private volatile HttpURLConnection activeDownload;

    private static final class Entry extends BetaUpdate {
        final String url;
        final String sha256;
        final long size;
        final long expires;

        Entry(JSONObject json, long expires) throws Exception {
            super(json.getString("version"), json.getInt("version_code"), json.getString("changelog"));
            url = json.getString("url");
            sha256 = json.getString("sha256");
            size = json.getLong("size");
            this.expires = expires;
            if (version.length() > 64 || changelog.length() > 16384 || versionCode <= 0
                    || size <= 0 || size > MAX_APK || !sha256.matches("[0-9a-f]{64}")) {
                throw new Exception("Invalid update metadata");
            }
            validateUrl(new URL(url), false);
        }
    }

    private UpdateController() {}

    public static UpdateController getInstance() { return INSTANCE; }
    public BetaUpdate getUpdate() { return available; }
    public File getDownloadedFile() { return downloaded; }
    public boolean isDownloading() { return downloading; }
    public float getProgress() { return progress; }
    public boolean hasError() { return error; }

    public void check(boolean force, Runnable done) {
        if (!ApplicationLoader.isApplicationInitialized()) {
            error = true;
            if (done != null) done.run();
            return;
        }
        long now = SystemClock.elapsedRealtime();
        if (!force && (now - lastAttempt < 60000 && lastAttempt != 0
                || now - lastSuccess < 8 * 3600000L && lastSuccess != 0)) {
            if (done != null) done.run();
            return;
        }
        if (done != null) callbacks.add(done);
        if (checking) return;
        checking = true;
        lastAttempt = now;
        worker.execute(() -> {
            Entry result = null;
            File cached = null;
            boolean failed = false;
            try {
                JSONObject wrapper = StrictJson.parse(fetchCatalog(), MAX_CATALOG);
                byte[] payload = Base64.decode(wrapper.getString("payload"), Base64.NO_WRAP);
                byte[] signature = Base64.decode(wrapper.getString("signature"), Base64.NO_WRAP);
                java.security.Signature verifier = java.security.Signature.getInstance("SHA256withECDSA");
                verifier.initVerify(KeyFactory.getInstance("EC").generatePublic(new X509EncodedKeySpec(Base64.decode(PUBLIC_KEY, Base64.NO_WRAP))));
                verifier.update("televim-catalog-v1\n".getBytes(StandardCharsets.US_ASCII));
                verifier.update(payload);
                if (!verifier.verify(derSignature(signature))) throw new Exception("Invalid catalog signature");
                JSONObject catalog = StrictJson.parse(payload, MAX_CATALOG);
                long time = System.currentTimeMillis() / 1000;
                long issued = catalog.getLong("issued");
                long expires = catalog.getLong("expires");
                if (catalog.getInt("format") != 1 || issued > time + 300 || expires <= time
                        || expires <= issued || expires - issued > 366L * 86400) {
                    throw new Exception("Expired update catalog");
                }
                JSONObject apk = catalog.getJSONObject("android").getJSONObject("stable");
                if (!ApplicationLoader.applicationContext.getPackageName().equals(apk.getString("package"))) {
                    throw new Exception("Wrong update package");
                }
                if (apk.getInt("min_sdk") > Build.VERSION.SDK_INT) throw new Exception("Unsupported Android version");
                Entry entry = new Entry(apk, expires);
                if (entry.versionCode > installed().versionCode) {
                    result = entry;
                    File candidate = new File(ApplicationLoader.applicationContext.getFilesDir(),
                            "cache/televim-updates/update-" + entry.versionCode + ".apk");
                    if (candidate.isFile()) {
                        try {
                            validateApk(candidate, entry);
                            cached = candidate;
                        } catch (Exception invalidCache) {
                            candidate.delete();
                        }
                    }
                }
            } catch (Exception failure) {
                failed = true;
            }
            final Entry checked = result;
            final File verifiedCache = cached;
            final boolean checkFailed = failed;
            AndroidUtilities.runOnUIThread(() -> {
                checking = false;
                error = checkFailed;
                if (!checkFailed) {
                    lastSuccess = SystemClock.elapsedRealtime();
                    if (available == null || checked == null || available.versionCode != checked.versionCode
                            || !available.sha256.equals(checked.sha256)) {
                        cancel();
                        downloaded = null;
                    }
                    available = checked;
                    if (!downloading) downloaded = verifiedCache;
                    notifyAvailable();
                }
                List<Runnable> ready = new ArrayList<>(callbacks);
                callbacks.clear();
                for (Runnable callback : ready) callback.run();
            });
        });
    }

    public void download() {
        if (!ApplicationLoader.isApplicationInitialized() || available == null || downloading) return;
        Entry entry = available;
        if (entry.expires <= System.currentTimeMillis() / 1000) {
            showError();
            return;
        }
        int token = generation.incrementAndGet();
        downloading = true;
        progress = 0;
        downloaded = null;
        notifyAvailable();
        worker.execute(() -> {
            File partial = null;
            File complete = null;
            boolean failed = false;
            try {
                Context context = ApplicationLoader.applicationContext;
                File directory = new File(context.getFilesDir(), "cache/televim-updates");
                if (!directory.isDirectory() && !directory.mkdirs()) throw new Exception("Cannot create update directory");
                File[] obsolete = directory.listFiles();
                if (obsolete != null) {
                    for (File file : obsolete) {
                        if (file.getName().matches("(?:download-[0-9]+\\.part|update-[0-9]+\\.apk)")) file.delete();
                    }
                }
                partial = new File(directory, "download-" + token + ".part");
                complete = new File(directory, "update-" + entry.versionCode + ".apk");
                HttpURLConnection connection = open(new URL(entry.url), true);
                activeDownload = connection;
                long received = 0;
                long started = SystemClock.elapsedRealtime();
                long notified = 0;
                try (InputStream input = connection.getInputStream(); FileOutputStream output = new FileOutputStream(partial)) {
                    byte[] buffer = new byte[65536];
                    int count;
                    while ((count = input.read(buffer)) != -1) {
                        if (token != generation.get() || SystemClock.elapsedRealtime() - started > 30 * 60000L) {
                            throw new Exception("Update download cancelled or timed out");
                        }
                        received += count;
                        if (received > entry.size) throw new Exception("Update too large");
                        output.write(buffer, 0, count);
                        progress = (float) received / entry.size;
                        long now = SystemClock.elapsedRealtime();
                        if (now - notified >= 200) {
                            notified = now;
                            AndroidUtilities.runOnUIThread(this::notifyProgress);
                        }
                    }
                    output.getFD().sync();
                } finally {
                    connection.disconnect();
                    activeDownload = null;
                }
                validateApk(partial, entry);
                if (token != generation.get()) throw new Exception("Update download cancelled");
                if (!partial.renameTo(complete)) throw new Exception("Cannot publish verified update");
            } catch (Exception failure) {
                failed = true;
            } finally {
                if (partial != null && partial.exists()) partial.delete();
            }
            final File ready = complete;
            final boolean downloadFailed = failed;
            AndroidUtilities.runOnUIThread(() -> {
                if (token != generation.get()) return;
                downloading = false;
                downloaded = downloadFailed ? null : ready;
                if (downloadFailed) showError();
                notifyAvailable();
            });
        });
    }

    public void cancel() {
        generation.incrementAndGet();
        HttpURLConnection connection = activeDownload;
        if (connection != null) connection.disconnect();
        downloading = false;
        progress = 0;
        notifyAvailable();
    }

    public void install(Activity activity) {
        File file = downloaded;
        Entry entry = available;
        if (file == null || entry == null || activity.isFinishing()) return;
        if (!ApplicationLoader.applicationLoaderInstance.checkApkInstallPermissions(activity)) return;
        worker.execute(() -> {
            try {
                validateApk(file, entry);
                AndroidUtilities.runOnUIThread(() -> {
                    if (activity.isFinishing() || activity.isDestroyed() || downloaded != file) return;
                    try {
                        Intent intent = new Intent(Intent.ACTION_VIEW);
                        intent.setDataAndType(FileProvider.getUriForFile(activity, ApplicationLoader.getApplicationId() + ".provider", file), "application/vnd.android.package-archive");
                        intent.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);
                        activity.startActivityForResult(intent, 500);
                    } catch (Exception failure) {
                        showError();
                    }
                });
            } catch (Exception failure) {
                AndroidUtilities.runOnUIThread(() -> {
                    downloaded = null;
                    showError();
                    notifyAvailable();
                });
            }
        });
    }

    private static PackageInfo installed() throws Exception {
        Context context = ApplicationLoader.applicationContext;
        return context.getPackageManager().getPackageInfo(context.getPackageName(), PackageManager.GET_SIGNATURES);
    }

    private static void validateApk(File file, Entry entry) throws Exception {
        if (entry.expires <= System.currentTimeMillis() / 1000 || file.length() != entry.size) {
            throw new Exception("Invalid update size or expiry");
        }
        MessageDigest digest = MessageDigest.getInstance("SHA-256");
        try (InputStream input = new FileInputStream(file)) {
            byte[] buffer = new byte[65536];
            int count;
            while ((count = input.read(buffer)) != -1) digest.update(buffer, 0, count);
        }
        StringBuilder hex = new StringBuilder();
        for (byte value : digest.digest()) hex.append(String.format(java.util.Locale.ROOT, "%02x", value & 255));
        if (!entry.sha256.equals(hex.toString())) throw new Exception("Invalid update digest");
        PackageInfo current = installed();
        PackageInfo archive = ApplicationLoader.applicationContext.getPackageManager().getPackageArchiveInfo(file.getAbsolutePath(), PackageManager.GET_SIGNATURES);
        if (archive == null || !current.packageName.equals(archive.packageName)
                || archive.versionCode != entry.versionCode || archive.versionCode <= current.versionCode
                || archive.applicationInfo == null || archive.applicationInfo.minSdkVersion > Build.VERSION.SDK_INT
                || archive.signatures == null || current.signatures == null
                || archive.signatures.length != current.signatures.length || archive.signatures.length == 0) {
            throw new Exception("Incompatible update");
        }
        for (Signature signature : current.signatures) {
            if (!Arrays.asList(archive.signatures).contains(signature)) throw new Exception("Untrusted update signer");
        }
    }

    private static byte[] fetchCatalog() throws Exception {
        HttpURLConnection connection = open(new URL(FEED), false);
        long started = SystemClock.elapsedRealtime();
        try (InputStream input = connection.getInputStream(); ByteArrayOutputStream output = new ByteArrayOutputStream()) {
            byte[] buffer = new byte[8192];
            int count;
            while ((count = input.read(buffer)) != -1) {
                if (output.size() + count > MAX_CATALOG || SystemClock.elapsedRealtime() - started > 30000) {
                    throw new Exception("Catalog limit exceeded");
                }
                output.write(buffer, 0, count);
            }
            return output.toByteArray();
        } finally {
            connection.disconnect();
        }
    }

    private static HttpURLConnection open(URL url, boolean artifact) throws Exception {
        for (int redirect = 0; redirect <= 3; redirect++) {
            if (artifact) validateUrl(url, redirect != 0);
            HttpURLConnection connection = (HttpURLConnection) url.openConnection();
            connection.setInstanceFollowRedirects(false);
            connection.setConnectTimeout(15000);
            connection.setReadTimeout(15000);
            connection.setUseCaches(false);
            connection.setRequestProperty("User-Agent", "Televim-Updater");
            try {
                int status = connection.getResponseCode();
                if (status == 200) return connection;
                if (artifact && (status == 301 || status == 302 || status == 303 || status == 307 || status == 308)) {
                    String location = connection.getHeaderField("Location");
                    if (location == null) throw new Exception("Missing redirect location");
                    url = new URL(url, location);
                } else {
                    throw new Exception("Update HTTP failure");
                }
            } catch (Exception failure) {
                connection.disconnect();
                throw failure;
            }
            connection.disconnect();
        }
        throw new Exception("Too many redirects");
    }

    private static void validateUrl(URL url, boolean redirected) throws Exception {
        boolean github = "github.com".equals(url.getHost())
                && url.getPath().startsWith("/Informativus/telegravim/releases/download/") && url.getQuery() == null;
        boolean asset = redirected && "release-assets.githubusercontent.com".equals(url.getHost());
        if (!"https".equals(url.getProtocol()) || url.getUserInfo() != null || url.getPort() != -1
                || url.getRef() != null || (!github && !asset)) {
            throw new Exception("Untrusted update address");
        }
    }

    private static byte[] derSignature(byte[] raw) throws Exception {
        if (raw.length != 64) throw new Exception("Invalid signature length");
        byte[] r = new java.math.BigInteger(1, Arrays.copyOfRange(raw, 0, 32)).toByteArray();
        byte[] s = new java.math.BigInteger(1, Arrays.copyOfRange(raw, 32, 64)).toByteArray();
        ByteArrayOutputStream result = new ByteArrayOutputStream();
        result.write(48);
        result.write(4 + r.length + s.length);
        result.write(2);
        result.write(r.length);
        result.write(r);
        result.write(2);
        result.write(s.length);
        result.write(s);
        return result.toByteArray();
    }

    private void notifyProgress() {
        if (downloading) NotificationCenter.getGlobalInstance().postNotificationName(NotificationCenter.appUpdateLoading);
    }

    private void notifyAvailable() {
        NotificationCenter.getGlobalInstance().postNotificationName(NotificationCenter.appUpdateAvailable);
    }

    private static void showError() {
        BulletinFactory.global().createErrorBulletin(LocaleController.getString(R.string.televim_update_failed)).show();
    }
}
