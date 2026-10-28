package org.telegram.ui;

import android.app.Activity;
import android.os.Bundle;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.widget.CompoundButton;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.Switch;
import android.widget.TextView;
import android.widget.Toast;

import org.telegram.messenger.R;
import org.telegram.messenger.UserConfig;
import org.telegram.messenger.televim.PrivateReadingController;
import org.telegram.messenger.televim.PrivateReadingPolicy;
import org.telegram.ui.ActionBar.Theme;

/** Native account setting for the local ordinary-chat receipt policy. */
public final class PrivateReadingSettingsActivity extends Activity {
    private Switch master;
    private Switch privateChats;
    private Switch groups;
    private Switch channels;
    private boolean updating;

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        getWindow().addFlags(android.view.WindowManager.LayoutParams.FLAG_SECURE);
        int account = getIntent() == null ? UserConfig.selectedAccount
                : getIntent().getIntExtra("account", UserConfig.selectedAccount);
        PrivateReadingPolicy.Status status = PrivateReadingController.status(account);
        if (status == null) {
            finish();
            return;
        }

        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setPadding(dp(16), dp(12), dp(16), dp(12));
        root.setBackgroundColor(Theme.getColor(Theme.key_windowBackgroundWhite));

        TextView title = new TextView(this);
        title.setText(R.string.TelevimPrivateReading);
        title.setTextSize(20);
        title.setTextColor(Theme.getColor(Theme.key_windowBackgroundWhiteBlackText));
        root.addView(title, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, dp(48)));

        TextView about = new TextView(this);
        about.setText(R.string.TelevimPrivateReadingAbout);
        about.setTextSize(15);
        about.setTextColor(Theme.getColor(Theme.key_windowBackgroundWhiteGrayText));
        root.addView(about, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, dp(0), 0));
        LinearLayout.LayoutParams aboutParams = (LinearLayout.LayoutParams) about.getLayoutParams();
        aboutParams.height = ViewGroup.LayoutParams.WRAP_CONTENT;
        aboutParams.bottomMargin = dp(12);
        about.setLayoutParams(aboutParams);

        master = addSwitch(root, R.string.TelevimPrivateReadingMaster);
        privateChats = addSwitch(root, R.string.TelevimPrivateReadingPrivate);
        groups = addSwitch(root, R.string.TelevimPrivateReadingGroups);
        channels = addSwitch(root, R.string.TelevimPrivateReadingChannels);

        TextView help = new TextView(this);
        help.setText(R.string.TelevimPrivateReadingHelp);
        help.setTextSize(13);
        help.setTextColor(Theme.getColor(Theme.key_windowBackgroundWhiteGrayText));
        help.setPadding(0, dp(16), 0, 0);
        root.addView(help, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));

        PrivateReadingPolicy.Selection selection = status.desired;
        updating = true;
        master.setChecked(selection.master);
        privateChats.setChecked(selection.privateChats);
        groups.setChecked(selection.groups);
        channels.setChecked(selection.channels);
        updating = false;
        CompoundButton.OnCheckedChangeListener listener = (button, checked) -> {
            if (!updating) {
                save(account);
            }
        };
        master.setOnCheckedChangeListener(listener);
        privateChats.setOnCheckedChangeListener(listener);
        groups.setOnCheckedChangeListener(listener);
        channels.setOnCheckedChangeListener(listener);

        ScrollView scroll = new ScrollView(this);
        scroll.addView(root);
        setContentView(scroll);
    }

    private Switch addSwitch(LinearLayout root, int label) {
        Switch result = new Switch(this);
        result.setText(label);
        result.setTextSize(16);
        result.setTextColor(Theme.getColor(Theme.key_windowBackgroundWhiteBlackText));
        result.setGravity(Gravity.CENTER_VERTICAL);
        result.setMinHeight(dp(52));
        root.addView(result, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, dp(52)));
        return result;
    }

    private void save(int account) {
        if (!PrivateReadingController.saveSelection(account, master.isChecked(), privateChats.isChecked(),
                groups.isChecked(), channels.isChecked())) {
            Toast.makeText(this, R.string.TelevimPrivateReadingHelp, Toast.LENGTH_SHORT).show();
        }
    }

    private int dp(int value) {
        return (int) (value * getResources().getDisplayMetrics().density + 0.5f);
    }
}
