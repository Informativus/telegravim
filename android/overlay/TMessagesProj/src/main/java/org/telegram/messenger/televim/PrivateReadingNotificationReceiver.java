package org.telegram.messenger.televim;

import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;

public final class PrivateReadingNotificationReceiver extends BroadcastReceiver {
    @Override
    public void onReceive(Context context, Intent intent) {
        PrivateReadingNotificationAction.receive(intent);
    }
}
