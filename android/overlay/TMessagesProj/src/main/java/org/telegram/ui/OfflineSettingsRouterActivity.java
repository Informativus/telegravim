package org.telegram.ui;

import android.app.Activity;
import android.content.Intent;
import android.os.Bundle;

import org.telegram.messenger.televim.OfflineSettingsInput;

public final class OfflineSettingsRouterActivity extends Activity {
    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(null);
        forward(getIntent());
    }

    @Override
    protected void onNewIntent(Intent intent) {
        super.onNewIntent(intent);
        forward(intent);
    }

    private void forward(Intent incoming) {
        Intent destination;
        try {
            destination = incoming == null ? new Intent() : new Intent(incoming);
            destination.setSelector(null);
            boolean recipient = incoming != null && Intent.ACTION_SEND.equals(incoming.getAction())
                    && (incoming.hasExtra("dialogId") || incoming.hasExtra("android.intent.extra.shortcut.ID"));
            destination.setClass(this, !recipient && OfflineSettingsInput.matches(incoming)
                    ? OfflineSettingsActivity.class : LaunchActivity.class);
        } catch (RuntimeException error) {
            destination = new Intent(this, OfflineSettingsActivity.class)
                    .setAction("org.informativus.televim.INVALID_SETTINGS");
        }
        try {
            startActivity(destination);
        } finally {
            setIntent(new Intent());
            finish();
        }
    }
}
