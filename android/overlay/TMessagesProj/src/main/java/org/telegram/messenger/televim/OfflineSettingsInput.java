package org.telegram.messenger.televim;

import android.content.Context;
import android.content.Intent;
import android.content.pm.ApplicationInfo;
import android.content.pm.PackageManager;
import android.content.pm.ProviderInfo;
import android.database.Cursor;
import android.net.Uri;
import android.os.ParcelFileDescriptor;
import android.os.Process;
import android.provider.DocumentsContract;
import android.system.Os;
import android.system.OsConstants;

import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.util.Arrays;
import java.util.List;
import java.util.Locale;
import java.util.concurrent.atomic.AtomicBoolean;

public final class OfflineSettingsInput {
    public final String link;
    public final Uri file;
    private final int flags;

    private OfflineSettingsInput(String link, Uri file, int flags) {
        this.link = link;
        this.file = file;
        this.flags = flags;
    }

    public static boolean matches(Intent intent) {
        try {
            return matchesReadable(intent);
        } catch (RuntimeException error) {
            return true;
        }
    }

    private static boolean matchesReadable(Intent intent) {
        if (intent == null) {
            return false;
        }
        Uri uri = intent.getData();
        if (uri != null && "televim".equalsIgnoreCase(uri.getScheme())) {
            return true;
        }
        if (Intent.ACTION_SEND.equals(intent.getAction())) {
            CharSequence text = intent.getCharSequenceExtra(Intent.EXTRA_TEXT);
            if (text != null && text.toString().trim().toLowerCase(Locale.ROOT).startsWith("televim:")) {
                return true;
            }
            uri = intent.getParcelableExtra(Intent.EXTRA_STREAM);
        }
        if (!Intent.ACTION_VIEW.equals(intent.getAction()) && !Intent.ACTION_SEND.equals(intent.getAction())) {
            return false;
        }
        String type = intent.getType();
        if ("application/x-televim-settings".equals(type) || "application/json".equals(type)) {
            return true;
        }
        String name = uri == null ? null : uri.getLastPathSegment();
        return name != null && (name.toLowerCase(Locale.ROOT).endsWith(".tgv")
                || name.toLowerCase(Locale.ROOT).endsWith(".json"));
    }

    public static OfflineSettingsInput fromIntent(Intent intent) throws IOException {
        if (!Intent.ACTION_VIEW.equals(intent.getAction()) && !Intent.ACTION_SEND.equals(intent.getAction())) {
            throw new IOException("Unsupported settings action");
        }
        Uri uri = intent.getData();
        if (uri != null && "televim".equalsIgnoreCase(uri.getScheme())) {
            return new OfflineSettingsInput(uri.toString(), null, 0);
        }
        if (Intent.ACTION_SEND.equals(intent.getAction())) {
            CharSequence text = intent.getCharSequenceExtra(Intent.EXTRA_TEXT);
            if (text != null && text.toString().trim().toLowerCase(Locale.ROOT).startsWith("televim:")) {
                return new OfflineSettingsInput(text.toString().trim(), null, 0);
            }
            uri = intent.getParcelableExtra(Intent.EXTRA_STREAM);
        }
        if (uri == null) {
            throw new IOException("Missing local file");
        }
        return new OfflineSettingsInput(null, uri, intent.getFlags());
    }

    public byte[] readLocalFile(Context context, AtomicBoolean cancelled) throws Exception {
        OfflineTransfer.checkCancelled(cancelled);
        if (file == null || !"content".equals(file.getScheme())
                || !"com.android.externalstorage.documents".equals(file.getAuthority())
                || file.getQuery() != null || file.getFragment() != null
                || (flags & Intent.FLAG_GRANT_READ_URI_PERMISSION) == 0
                || context.checkUriPermission(file, Process.myPid(), Process.myUid(),
                        Intent.FLAG_GRANT_READ_URI_PERMISSION) != PackageManager.PERMISSION_GRANTED) {
            throw new IOException("A granted local system file is required");
        }
        List<String> segments = file.getPathSegments();
        if (segments.size() != 2 || !"document".equals(segments.get(0))) {
            throw new IOException("Unsupported document URI");
        }
        String documentId = segments.get(1);
        int separator = documentId.indexOf(':');
        if (separator <= 0 || documentId.indexOf(':', separator + 1) >= 0) {
            throw new IOException("Invalid local document");
        }
        String volume = documentId.substring(0, separator);
        if (!volume.equals("primary") && !volume.matches("[0-9A-Fa-f]{4}-[0-9A-Fa-f]{4}")) {
            throw new IOException("Unsupported storage volume");
        }
        String path = documentId.substring(separator + 1);
        if (path.isEmpty() || path.startsWith("/") || path.contains("\\") || path.indexOf('\0') >= 0) {
            throw new IOException("Invalid document path");
        }
        for (String component : path.split("/", -1)) {
            if (component.isEmpty() || component.equals(".") || component.equals("..")) {
                throw new IOException("Invalid document path");
            }
        }
        PackageManager manager = context.getPackageManager();
        ProviderInfo provider = manager.resolveContentProvider(file.getAuthority(), 0);
        if (provider == null || !"com.android.externalstorage".equals(provider.packageName)
                || (provider.applicationInfo.flags & (ApplicationInfo.FLAG_SYSTEM | ApplicationInfo.FLAG_UPDATED_SYSTEM_APP)) == 0
                || manager.checkSignatures("android", provider.packageName) != PackageManager.SIGNATURE_MATCH) {
            throw new IOException("Untrusted document provider");
        }
        String[] projection = {DocumentsContract.Document.COLUMN_FLAGS, DocumentsContract.Document.COLUMN_SIZE};
        OfflineTransfer.checkCancelled(cancelled);
        try (Cursor cursor = context.getContentResolver().query(file, projection, null, null, null)) {
            if (cursor == null || !cursor.moveToFirst()) {
                throw new IOException("Local file is unavailable");
            }
            int documentFlags = cursor.getInt(cursor.getColumnIndexOrThrow(DocumentsContract.Document.COLUMN_FLAGS));
            if ((documentFlags & DocumentsContract.Document.FLAG_VIRTUAL_DOCUMENT) != 0) {
                throw new IOException("Virtual files are not supported");
            }
            int sizeColumn = cursor.getColumnIndexOrThrow(DocumentsContract.Document.COLUMN_SIZE);
            if (!cursor.isNull(sizeColumn) && cursor.getLong(sizeColumn) > OfflineTransfer.MAX_ENCRYPTED) {
                throw new IOException("Transfer size limit exceeded");
            }
        }
        OfflineTransfer.checkCancelled(cancelled);
        try (ParcelFileDescriptor descriptor = context.getContentResolver().openFileDescriptor(file, "r")) {
            OfflineTransfer.checkCancelled(cancelled);
            if (descriptor == null || !OsConstants.S_ISREG(Os.fstat(descriptor.getFileDescriptor()).st_mode)
                    || descriptor.getStatSize() < 0 || descriptor.getStatSize() > OfflineTransfer.MAX_ENCRYPTED) {
                throw new IOException("A bounded regular local file is required");
            }
            try (ParcelFileDescriptor.AutoCloseInputStream stream = new ParcelFileDescriptor.AutoCloseInputStream(descriptor)) {
                ByteArrayOutputStream output = new ByteArrayOutputStream();
                byte[] buffer = new byte[16384];
                try {
                    int count;
                    while ((count = stream.read(buffer)) != -1) {
                        OfflineTransfer.checkCancelled(cancelled);
                        if (output.size() + count > OfflineTransfer.MAX_ENCRYPTED) {
                            throw new IOException("Transfer cancelled or too large");
                        }
                        output.write(buffer, 0, count);
                    }
                    return output.toByteArray();
                } finally {
                    Arrays.fill(buffer, (byte) 0);
                }
            }
        }
    }
}
