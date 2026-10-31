package org.telegram.messenger.televim;

import android.util.SparseIntArray;

import org.json.JSONObject;
import org.telegram.messenger.ApplicationLoader;
import org.telegram.messenger.MessagesController;
import org.telegram.messenger.UserConfig;
import org.telegram.tgnet.ConnectionsManager;

import java.util.Iterator;

public final class TelevimFolderColors {
    public static final int NO_COLOR = -1;
    private static final String KEY_PREFIX = "televim_folder_colors_";
    private static final Object LOCK = new Object();
    private static String cachedKey;
    private static String cachedValue;
    private static Entry cachedEntry;

    private static final class Entry {
        final boolean tagsEnabled;
        final SparseIntArray colors = new SparseIntArray();

        Entry(boolean tagsEnabled) {
            this.tagsEnabled = tagsEnabled;
        }
    }

    private TelevimFolderColors() {
    }

    public static String key(String environment, String userId) {
        return KEY_PREFIX + environment + "_" + userId;
    }

    public static boolean isKey(String name) {
        return name.startsWith(KEY_PREFIX);
    }

    public static boolean tagsEnabled(int account) {
        Entry entry = entry(account);
        return entry != null && entry.tagsEnabled;
    }

    public static int color(int account, MessagesController.DialogFilter filter) {
        Entry entry = entry(account);
        if (entry == null) {
            return filter.color;
        }
        int index = entry.colors.indexOfKey(filter.id);
        return index < 0 ? filter.color : entry.colors.valueAt(index);
    }

    private static Entry entry(int account) {
        UserConfig config = UserConfig.getInstance(account);
        if (!config.isClientActivated()) {
            return null;
        }
        String environment = ConnectionsManager.getInstance(account).isTestBackend() ? "test" : "production";
        String key = key(environment, String.valueOf(config.getClientUserId()));
        String value = OfflineSettingsStore.preferences(ApplicationLoader.applicationContext).getString(key, null);
        synchronized (LOCK) {
            if (key.equals(cachedKey) && (value == null ? cachedValue == null : value.equals(cachedValue))) {
                return cachedEntry;
            }
            cachedKey = key;
            cachedValue = value;
            cachedEntry = parse(value);
            return cachedEntry;
        }
    }

    private static Entry parse(String value) {
        if (value == null) {
            return null;
        }
        try {
            JSONObject object = new JSONObject(value);
            Entry entry = new Entry(object.getBoolean("tags_enabled"));
            JSONObject colors = object.getJSONObject("colors");
            Iterator<String> keys = colors.keys();
            while (keys.hasNext()) {
                String id = keys.next();
                entry.colors.put(Integer.parseInt(id), colors.getInt(id));
            }
            return entry;
        } catch (Exception error) {
            return null;
        }
    }
}
