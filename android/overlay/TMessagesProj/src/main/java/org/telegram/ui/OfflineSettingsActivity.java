package org.telegram.ui;

import android.content.Context;
import android.text.InputFilter;
import android.text.InputType;
import android.text.Editable;
import android.text.TextWatcher;
import android.util.TypedValue;
import android.view.View;
import android.view.ViewGroup;
import android.view.inputmethod.EditorInfo;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

import org.telegram.messenger.AndroidUtilities;
import org.telegram.messenger.FlagSecureReason;
import org.telegram.messenger.LocaleController;
import org.telegram.messenger.R;
import org.telegram.messenger.SharedConfig;
import org.telegram.messenger.televim.OfflineSettingsInput;
import org.telegram.messenger.televim.OfflineSettingsStore;
import org.telegram.messenger.televim.OfflineTransfer;
import org.telegram.ui.ActionBar.ActionBar;
import org.telegram.ui.ActionBar.BaseFragment;
import org.telegram.ui.ActionBar.Theme;
import org.telegram.ui.Cells.TextSettingsCell;
import org.telegram.ui.Components.ChatActivityEnterView;

import java.util.Arrays;
import java.util.Map;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.atomic.AtomicBoolean;

public final class OfflineSettingsActivity extends BaseFragment {
    private static final ExecutorService WORKER = Executors.newSingleThreadExecutor();
    private static final AtomicBoolean WORKER_BUSY = new AtomicBoolean();
    private OfflineSettingsInput incoming;
    private EditText link;
    private EditText password;
    private TextView status;
    private TextSettingsCell inspect;
    private TextSettingsCell apply;
    private TextSettingsCell scan;
    private byte[] source;
    private OfflineSettingsStore.Plan plan;
    private int operation;
    private boolean destroyed;
    private boolean busy;
    private boolean qrSource;
    private AtomicBoolean cancellation;
    private final boolean invalidInput;
    private FlagSecureReason secureWindow;

    public OfflineSettingsActivity() {
        this(null, false);
    }

    public OfflineSettingsActivity(OfflineSettingsInput input, boolean invalidInput) {
        incoming = input;
        this.invalidInput = invalidInput;
    }

    @Override
    protected boolean shouldCancelAccountRequestsOnDestroy() {
        return false;
    }

    @Override
    public View createView(Context context) {
        if (secureWindow == null && getParentActivity() != null) {
            secureWindow = new FlagSecureReason(getParentActivity().getWindow(), () -> true);
            secureWindow.attach();
        }
        actionBar.setTitle(text(R.string.TelevimImportTitle));
        actionBar.setBackButtonImage(R.drawable.ic_ab_back);
        actionBar.setActionBarMenuOnItemClick(new ActionBar.ActionBarMenuOnItemClick() {
            @Override
            public void onItemClick(int id) {
                if (id == -1) {
                    finishFragment();
                }
            }
        });
        ScrollView scroll = new ScrollView(context);
        scroll.setFillViewport(true);
        scroll.setBackgroundColor(Theme.getColor(Theme.key_windowBackgroundWhite));
        scroll.setImportantForAutofill(View.IMPORTANT_FOR_AUTOFILL_NO_EXCLUDE_DESCENDANTS);
        LinearLayout content = new LinearLayout(context);
        content.setOrientation(LinearLayout.VERTICAL);
        int spacing = context.getResources().getDimensionPixelSize(R.dimen.televim_spacing);
        content.setPadding(spacing, spacing, spacing, spacing);
        scroll.addView(content, new ScrollView.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));
        content.addView(label(context, text(R.string.TelevimImportAbout)));
        link = field(context, R.string.TelevimImportLink, false);
        link.setFilters(new InputFilter[]{new InputFilter.LengthFilter(OfflineTransfer.MAX_LINK + 1)});
        content.addView(link);
        password = field(context, R.string.TelevimImportPassword, true);
        password.setFilters(new InputFilter[]{new InputFilter.LengthFilter(1025)});
        content.addView(password);
        scan = button(context, R.string.TelevimImportScan, () -> presentFragment(new OfflineSettingsQrActivity(value -> {
            cancellation = null;
            incoming = null;
            discardPlan();
            apply.setVisibility(View.GONE);
            qrSource = true;
            link.setText(value);
            status.setText(text(R.string.TelevimImportQrRead));
        })));
        content.addView(scan);
        inspect = button(context, R.string.TelevimImportPreview, this::inspect);
        content.addView(inspect);
        status = label(context, text(invalidInput ? R.string.TelevimImportInvalid : R.string.TelevimImportFiles));
        content.addView(status);
        apply = button(context, R.string.TelevimImportApply, this::apply);
        apply.setVisibility(View.GONE);
        content.addView(apply);
        content.addView(button(context, R.string.Cancel, () -> {
            cancelWork();
            busy = false;
            boolean discarded = discardPlan();
            incoming = null;
            password.setText("");
            link.setText("");
            apply.setVisibility(View.GONE);
            setBusy(false);
            status.setText(text(discarded ? R.string.TelevimImportCancelled : R.string.TelevimImportCleanupFailed));
        }));
        if (incoming != null && incoming.link != null) {
            if (incoming.link.length() <= OfflineTransfer.MAX_LINK) {
                link.setText(incoming.link);
            } else {
                status.setText(text(R.string.TelevimImportInvalid));
                incoming = null;
            }
        } else if (incoming != null) {
            link.setHint(text(R.string.TelevimImportLocalFile));
            link.setEnabled(false);
            status.setText(text(R.string.TelevimImportFileReady));
        }
        fragmentView = scroll;
        TextWatcher changed = new TextWatcher() {
            @Override
            public void beforeTextChanged(CharSequence value, int start, int count, int after) {
            }

            @Override
            public void onTextChanged(CharSequence value, int start, int before, int count) {
                if (plan != null) {
                    if (!discardPlan()) {
                        status.setText(text(R.string.TelevimImportCleanupFailed));
                    }
                    apply.setVisibility(View.GONE);
                }
            }

            @Override
            public void afterTextChanged(Editable value) {
            }
        };
        link.addTextChangedListener(changed);
        password.addTextChangedListener(changed);
        return fragmentView;
    }

    private EditText field(Context context, int hint, boolean secret) {
        EditText field = new EditText(context);
        field.setHint(text(hint));
        field.setTextColor(Theme.getColor(Theme.key_windowBackgroundWhiteBlackText));
        field.setHintTextColor(Theme.getColor(Theme.key_windowBackgroundWhiteHintText));
        field.setInputType(InputType.TYPE_CLASS_TEXT | (secret ? InputType.TYPE_TEXT_VARIATION_PASSWORD : InputType.TYPE_TEXT_VARIATION_URI));
        field.setSingleLine(true);
        field.setSaveEnabled(false);
        field.setImportantForAutofill(View.IMPORTANT_FOR_AUTOFILL_NO);
        field.setImeOptions(EditorInfo.IME_FLAG_NO_PERSONALIZED_LEARNING | EditorInfo.IME_ACTION_DONE);
        return field;
    }

    private TextView label(Context context, String value) {
        TextView label = new TextView(context);
        label.setText(value);
        label.setTextColor(Theme.getColor(Theme.key_windowBackgroundWhiteBlackText));
        label.setTextSize(TypedValue.COMPLEX_UNIT_PX, context.getResources().getDimension(R.dimen.televim_text_size));
        int spacing = context.getResources().getDimensionPixelSize(R.dimen.televim_spacing);
        label.setPadding(0, spacing, 0, spacing);
        return label;
    }

    private TextSettingsCell button(Context context, int title, Runnable clicked) {
        TextSettingsCell button = new TextSettingsCell(context);
        button.setText(text(title), false);
        button.setBackground(Theme.getSelectorDrawable(false));
        button.setOnClickListener(view -> clicked.run());
        return button;
    }

    private void inspect() {
        if (busy || destroyed) {
            return;
        }
        cancellation = null;
        if (OfflineSettingsStore.isUncertain()) {
            status.setText(text(R.string.TelevimImportRestart));
            return;
        }
        if (!WORKER_BUSY.compareAndSet(false, true)) {
            status.setText(text(R.string.TelevimImportWorking));
            return;
        }
        if (!discardPlan()) {
            WORKER_BUSY.set(false);
            status.setText(text(R.string.TelevimImportCleanupFailed));
            return;
        }
        apply.setVisibility(View.GONE);
        OfflineSettingsInput input = incoming;
        String value = link.getText().toString().trim();
        char[] secret = new char[password.length()];
        password.getText().getChars(0, password.length(), secret, 0);
        password.setText("");
        Context context = getParentActivity().getApplicationContext();
        OfflineSettingsStore.Baseline baseline = OfflineSettingsStore.capture(context);
        int token = ++operation;
        AtomicBoolean cancelled = new AtomicBoolean();
        cancellation = cancelled;
        boolean fromQr = qrSource;
        setBusy(true);
        status.setText(text(R.string.TelevimImportWorking));
        WORKER.execute(() -> {
            OfflineTransfer.Package transfer = null;
            OfflineSettingsStore.Plan staged = null;
            byte[] bytes = null;
            try {
                bytes = input != null && input.file != null
                        ? input.readLocalFile(context, cancelled)
                        : OfflineTransfer.decodeLink(value, fromQr);
                transfer = OfflineTransfer.inspect(bytes, secret, cancelled);
                staged = OfflineSettingsStore.prepare(context, transfer, baseline, cancelled);
                OfflineSettingsStore.Plan prepared = staged;
                AndroidUtilities.runOnUIThread(() -> {
                    if (destroyed || token != operation || cancelled.get()) {
                        boolean discarded = prepared.discard();
                        prepared.transfer.clear();
                        if (!destroyed && cancellation == cancelled) {
                            cancellation = null;
                            if (!discarded) {
                                status.setText(text(R.string.TelevimImportCleanupFailed));
                            }
                        }
                        return;
                    }
                    cancellation = null;
                    clearSource();
                    source = prepared.transfer.original;
                    plan = prepared;
                    setBusy(false);
                    showPreview();
                });
            } catch (Exception error) {
                if (staged != null) {
                    staged.discard();
                }
                if (transfer != null) {
                    transfer.clear();
                } else if (bytes != null) {
                    Arrays.fill(bytes, (byte) 0);
                }
                boolean cleanupFailed = OfflineSettingsStore.isUncertain();
                AndroidUtilities.runOnUIThread(() -> {
                    if (destroyed || cancellation != cancelled) {
                        return;
                    }
                    cancellation = null;
                    if (token == operation) {
                        setBusy(false);
                        status.setText(text(cleanupFailed
                                ? R.string.TelevimImportCleanupFailed : R.string.TelevimImportInvalid));
                    } else if (cancelled.get() && cleanupFailed) {
                        status.setText(text(R.string.TelevimImportCleanupFailed));
                    }
                });
            } finally {
                Arrays.fill(secret, '\0');
                WORKER_BUSY.set(false);
            }
        });
    }

    private void showPreview() {
        if (plan.duplicate) {
            status.setText(text(R.string.TelevimImportDuplicate));
            discardPlan();
            return;
        }
        StringBuilder preview = new StringBuilder(text(R.string.TelevimImportGlobal));
        for (Map.Entry<String, Boolean> preference : plan.transfer.preferences.entrySet()) {
            boolean enter = preference.getKey().equals("send_by_enter");
            boolean previous = plan.before.getBoolean(enter ? "send_by_enter" : "allowBigEmoji", !enter);
            preview.append("\n\n").append(text(enter ? R.string.SendByEnter : R.string.TelevimImportLargeEmoji))
                    .append(": ").append(text(previous ? R.string.TelevimImportOn : R.string.TelevimImportOff))
                    .append(" → ").append(text(preference.getValue() ? R.string.TelevimImportOn : R.string.TelevimImportOff));
        }
        if (plan.transfer.preferences.isEmpty()) {
            preview.append("\n\n").append(text(R.string.TelevimImportNoSupported));
        }
        preview.append("\n\n").append(LocaleController.formatString(R.string.TelevimImportRetained,
                plan.transfer.unsupportedModules, plan.transfer.assets, plan.transfer.accounts.size(), plan.transfer.original.length));
        for (String identity : plan.transfer.accounts.values()) {
            preview.append("\n").append(identity);
        }
        preview.append("\n\n").append(text(R.string.TelevimImportInactive));
        if (plan.transfer.rawJson) {
            preview.append("\n\n").append(text(R.string.TelevimImportPlaintext));
        }
        status.setText(preview.toString());
        apply.setText(text(plan.transfer.preferences.isEmpty() ? R.string.TelevimImportRetainOnly : R.string.TelevimImportApply), false);
        apply.setVisibility(View.VISIBLE);
    }

    private void apply() {
        if (plan == null || busy) {
            return;
        }
        OfflineSettingsStore.Applied result = OfflineSettingsStore.apply(getParentActivity(), plan);
        if (result == OfflineSettingsStore.Applied.SUCCESS || result == OfflineSettingsStore.Applied.UNCERTAIN) {
            OfflineSettingsStore.Baseline current = OfflineSettingsStore.capture(getParentActivity());
            SharedConfig.allowBigEmoji = current.getBoolean("allowBigEmoji", true);
            ChatActivityEnterView.refreshPortablePreferences(current.getBoolean("send_by_enter", false));
        }
        switch (result) {
            case SUCCESS:
                status.setText(text(plan.transfer.preferences.isEmpty()
                        ? R.string.TelevimImportRetainedOnly : R.string.TelevimImportDone));
                break;
            case DUPLICATE:
                status.setText(text(R.string.TelevimImportDuplicate));
                break;
            case STALE:
                status.setText(text(R.string.TelevimImportStale));
                break;
            case UNCERTAIN:
                status.setText(text(R.string.TelevimImportRestart));
                break;
        }
        apply.setVisibility(View.GONE);
        if (!discardPlan()) {
            status.setText(text(R.string.TelevimImportCleanupFailed));
        }
        password.setText("");
        if (result == OfflineSettingsStore.Applied.STALE) {
            return;
        }
        link.setText("");
        incoming = null;
        link.setEnabled(true);
    }

    private void setBusy(boolean value) {
        busy = value;
        inspect.setEnabled(!value);
        scan.setEnabled(!value);
        link.setEnabled(!value && (incoming == null || incoming.file == null));
        password.setEnabled(!value);
    }

    private void clearSource() {
        if (source != null) {
            Arrays.fill(source, (byte) 0);
            source = null;
        }
    }

    private boolean discardPlan() {
        OfflineSettingsStore.Plan previous = plan;
        plan = null;
        boolean discarded = previous == null || previous.discard();
        if (previous != null) {
            previous.transfer.clear();
        }
        clearSource();
        return discarded;
    }

    private void cancelWork() {
        if (cancellation != null) {
            cancellation.set(true);
        }
        ++operation;
    }

    private String text(int resource) {
        return LocaleController.getString(resource);
    }

    @Override
    public void onFragmentDestroy() {
        destroyed = true;
        cancelWork();
        discardPlan();
        incoming = null;
        if (secureWindow != null) {
            secureWindow.detach();
            secureWindow = null;
        }
        if (password != null) {
            password.setText("");
            link.setText("");
        }
        super.onFragmentDestroy();
    }
}
