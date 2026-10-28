package org.telegram.ui;

import android.content.Context;
import android.content.DialogInterface;
import android.text.Editable;
import android.text.InputFilter;
import android.text.InputType;
import android.text.TextWatcher;
import android.util.TypedValue;
import android.view.View;
import android.widget.CheckBox;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

import org.telegram.messenger.LocaleController;
import org.telegram.messenger.R;
import org.telegram.messenger.televim.E2eChat;
import org.telegram.messenger.televim.E2eState;
import org.telegram.ui.ActionBar.AlertDialog;
import org.telegram.ui.ActionBar.BaseFragment;
import org.telegram.ui.ActionBar.Theme;

public final class E2eChatDialog {
    private static String text(int resource) {
        return LocaleController.getString(resource);
    }

    public static void show(BaseFragment fragment, long peer) {
        if (fragment.getParentActivity() == null) return;
        int account = fragment.getCurrentAccount();
        E2eState state = E2eChat.state(account);
        if (!state.hasPeer(peer)) {
            keys(fragment, peer);
            return;
        }
        boolean enabled = state.enabled(peer);
        AlertDialog.Builder builder = new AlertDialog.Builder(fragment.getParentActivity(), fragment.getResourceProvider());
        builder.setTitle(text(R.string.TelevimE2eTitle));
        builder.setMessage(text(enabled ? state.ready(peer) ? R.string.TelevimE2eActive : R.string.TelevimE2ePending
                : R.string.TelevimE2eOff));
        builder.setPositiveButton(text(enabled ? R.string.TelevimE2eDisable : R.string.TelevimE2eEnable), (dialog, which) -> {
            try {
                E2eChat.sendControl(account, peer, state.toggle(peer, !enabled));
            } catch (Exception e) {
                E2eChat.error(R.string.TelevimE2eFailed);
            }
        });
        builder.setNeutralButton(text(R.string.TelevimE2eKeys), (dialog, which) -> keys(fragment, peer));
        if (enabled && !state.ready(peer)) {
            builder.setNegativeButton(text(R.string.TelevimE2eRetry), (dialog, which) -> {
                try {
                    E2eChat.sendControl(account, peer, state.control(peer));
                } catch (Exception e) {
                    E2eChat.error(R.string.TelevimE2eFailed);
                }
            });
        } else {
            builder.setNegativeButton(text(R.string.Cancel), null);
        }
        fragment.showDialog(builder.create());
    }

    private static TextView label(Context context, CharSequence value) {
        TextView result = new TextView(context);
        result.setText(value);
        result.setTextColor(Theme.getColor(Theme.key_dialogTextBlack));
        result.setTextSize(TypedValue.COMPLEX_UNIT_PX, context.getResources().getDimension(R.dimen.televim_text_size));
        result.setTextIsSelectable(true);
        return result;
    }

    private static void keys(BaseFragment fragment, long peer) {
        Context context = fragment.getParentActivity();
        if (context == null) return;
        int account = fragment.getCurrentAccount();
        E2eState state = E2eChat.state(account);
        final String invitation;
        final String known;
        try {
            invitation = state.invitation();
            known = state.peerKey(peer);
        } catch (Exception e) {
            E2eChat.error(R.string.TelevimE2eFailed);
            return;
        }
        LinearLayout content = new LinearLayout(context);
        content.setOrientation(LinearLayout.VERTICAL);
        int spacing = context.getResources().getDimensionPixelSize(R.dimen.televim_spacing);
        content.setPadding(spacing, spacing, spacing, spacing);
        content.setImportantForAutofill(View.IMPORTANT_FOR_AUTOFILL_NO_EXCLUDE_DESCENDANTS);
        content.addView(label(context, text(R.string.TelevimE2eSetup)));
        content.addView(label(context, invitation));
        EditText input = new EditText(context);
        input.setTextColor(Theme.getColor(Theme.key_dialogTextBlack));
        input.setHintTextColor(Theme.getColor(Theme.key_dialogTextHint));
        input.setInputType(InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS);
        input.setFilters(new InputFilter[] { new InputFilter.LengthFilter(512) });
        input.setHint(text(R.string.TelevimE2ePeerKey));
        input.setText(known);
        content.addView(input);
        TextView fingerprint = label(context, "");
        content.addView(fingerprint);
        CheckBox verified = new CheckBox(context);
        verified.setText(text(R.string.TelevimE2eVerified));
        verified.setTextColor(Theme.getColor(Theme.key_dialogTextBlack));
        content.addView(verified);
        Runnable update = () -> {
            verified.setChecked(false);
            try {
                fingerprint.setText(text(R.string.TelevimE2eFingerprint) + "\n"
                        + E2eState.fingerprint(E2eState.parsePublic(input.getText().toString())));
            } catch (Exception e) {
                fingerprint.setText(text(R.string.TelevimE2ePeerKey));
            }
        };
        update.run();
        input.addTextChangedListener(new TextWatcher() {
            @Override public void beforeTextChanged(CharSequence value, int start, int count, int after) { }
            @Override public void onTextChanged(CharSequence value, int start, int before, int count) { update.run(); }
            @Override public void afterTextChanged(Editable value) { }
        });
        ScrollView scroll = new ScrollView(context);
        scroll.addView(content);
        AlertDialog.Builder builder = new AlertDialog.Builder(context, fragment.getResourceProvider());
        builder.setTitle(text(R.string.TelevimE2eKeys));
        builder.setView(scroll);
        builder.setPositiveButton(text(R.string.TelevimE2eEnable), null);
        builder.setNeutralButton(text(R.string.TelevimE2eSendKey), null);
        builder.setNegativeButton(text(R.string.Cancel), null);
        AlertDialog dialog = builder.create();
        fragment.showDialog(dialog);
        dialog.getButton(DialogInterface.BUTTON_NEUTRAL).setOnClickListener(view -> E2eChat.sendControl(account, peer, invitation));
        dialog.getButton(DialogInterface.BUTTON_POSITIVE).setOnClickListener(view -> {
            if (!verified.isChecked()) {
                E2eChat.error(R.string.TelevimE2eVerifyFirst);
                return;
            }
            try {
                state.trust(peer, E2eState.parsePublic(input.getText().toString()));
                E2eChat.sendControl(account, peer, invitation);
                E2eChat.sendControl(account, peer, state.toggle(peer, true));
                dialog.dismiss();
            } catch (Exception e) {
                E2eChat.error(R.string.TelevimE2eFailed);
            }
        });
    }
}
