package org.telegram.ui;

import android.Manifest;
import android.app.Activity;
import android.content.Context;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.graphics.Color;
import android.content.res.Configuration;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.os.SystemClock;
import android.text.InputFilter;
import android.text.InputType;
import android.text.Editable;
import android.text.TextWatcher;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.view.WindowInsets;
import android.view.WindowManager;
import android.view.inputmethod.EditorInfo;
import android.widget.EditText;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.ImageView;
import android.widget.ScrollView;
import android.widget.TextView;

import org.telegram.messenger.ApplicationLoader;
import org.telegram.messenger.R;
import org.telegram.messenger.SharedConfig;
import org.telegram.messenger.televim.OfflineSettingsInput;
import org.telegram.messenger.televim.OfflineSettingsStore;
import org.telegram.messenger.televim.OfflineTransfer;
import org.telegram.ui.Components.ChatActivityEnterView;

import java.util.Arrays;
import java.util.Map;
import java.util.Objects;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.atomic.AtomicBoolean;

public final class OfflineSettingsActivity extends Activity {
    private static final int CAMERA_PERMISSION = 65174;
    private static final ExecutorService WORKER = Executors.newSingleThreadExecutor();
    private static final AtomicBoolean WORKER_BUSY = new AtomicBoolean();
    private OfflineSettingsInput incoming;
    private EditText link;
    private EditText password;
    private TextView status;
    private TextView inspect;
    private TextView apply;
    private TextView scan;
    private byte[] source;
    private OfflineSettingsStore.Plan plan;
    private int operation;
    private boolean destroyed;
    private boolean busy;
    private boolean qrSource;
    private AtomicBoolean cancellation;
    private boolean invalidInput;
    private final Handler ui = new Handler(Looper.getMainLooper());
    private final Runnable retryTick = this::updateRetry;
    private FrameLayout body;
    private View form;
    private LinearLayout root;
    private FrameLayout toolbar;
    private ImageView back;
    private OfflineSettingsIntro intro;
    private LinearLayout entry;
    private TextView fileHelp;
    private boolean entryVisible;
    private OfflineSettingsQrView scanner;
    private EditText passcode;
    private TextView lockStatus;
    private TextView unlock;
    private boolean resumed;
    private boolean authorized;
    private boolean interrupted;
    private String authorizedHash;
    private boolean scanRequested;
    private boolean cameraPermissionPending;
    private boolean cameraError;


    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(null);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_SECURE);
        SharedConfig.loadConfig();
        root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setSaveFromParentEnabled(false);
        toolbar = new FrameLayout(this);
        back = OfflineSettingsAppearance.back(this, this::onBackPressed);
        int backSize = getResources().getDimensionPixelSize(R.dimen.televim_back_size);
        toolbar.addView(back, new FrameLayout.LayoutParams(backSize, backSize, Gravity.START | Gravity.CENTER_VERTICAL));
        root.addView(toolbar, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
                getResources().getDimensionPixelSize(R.dimen.televim_toolbar_height)));
        updateHeader();
        body = new FrameLayout(this);
        root.addView(body, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, 0, 1));
        if (Build.VERSION.SDK_INT >= 30) {
            getWindow().setDecorFitsSystemWindows(false);
            root.setOnApplyWindowInsetsListener((view, insets) -> {
                android.graphics.Insets padding = insets.getInsets(WindowInsets.Type.systemBars()
                        | WindowInsets.Type.displayCutout() | WindowInsets.Type.ime());
                view.setPadding(padding.left, padding.top, padding.right, padding.bottom);
                return insets;
            });
        } else {
            root.setFitsSystemWindows(true);
        }
        setContentView(root);
        receive(getIntent());
    }

    @Override
    protected void onNewIntent(Intent intent) {
        super.onNewIntent(intent);
        invalidateAccess();
        scanRequested = false;
        cameraError = false;
        receive(intent);
        if (resumed) {
            showGate();
        }
    }

    private void receive(Intent intent) {
        incoming = null;
        qrSource = false;
        invalidInput = false;
        try {
            if (intent != null && OfflineSettingsInput.matches(intent)) {
                incoming = OfflineSettingsInput.fromIntent(intent);
            } else if (intent != null && intent.getAction() != null) {
                invalidInput = true;
            }
        } catch (Exception error) {
            invalidInput = true;
        }
        if (intent != null) {
            intent.setAction(null);
            intent.setDataAndType(null, null);
            intent.replaceExtras((Bundle) null);
            intent.setClipData(null);
        }
        setIntent(new Intent());
    }

    @Override
    protected void onResume() {
        super.onResume();
        resumed = true;
        SharedConfig.loadConfig();
        showGate();
    }

    @Override
    protected void onPause() {
        resumed = false;
        if (!cameraPermissionPending) {
            scanRequested = false;
        }
        invalidateAccess();
        super.onPause();
    }

    private boolean hasAccess() {
        return resumed && !destroyed && authorized
                && Objects.equals(authorizedHash, SharedConfig.passcodeHash);
    }

    private boolean ensureAccess() {
        if (hasAccess()) {
            return true;
        }
        invalidateAccess();
        if (resumed && !destroyed) {
            showGate();
        }
        return false;
    }

    private void invalidateAccess() {
        interrupted = interrupted || busy || plan != null || scanner != null;
        cancelWork();
        if (!discardPlan()) {
            interrupted = true;
        }
        busy = false;
        authorized = false;
        authorizedHash = null;
        ui.removeCallbacks(retryTick);
        closeScanner();
        if (password != null) {
            password.setText("");
            link.setText("");
            apply.setVisibility(View.GONE);
        }
        if (status != null) {
            status.setText("");
        }
        password = null;
        link = null;
        status = null;
        inspect = null;
        apply = null;
        scan = null;
        if (passcode != null) {
            passcode.setText("");
            passcode = null;
        }
        unlock = null;
        lockStatus = null;
        if (body != null) {
            body.removeAllViews();
        }
        form = null;
        intro = null;
        entry = null;
        fileHelp = null;
    }

    private void showGate() {
        updateHeader();
        body.removeAllViews();
        if (SharedConfig.passcodeHash.isEmpty()) {
            authorized = true;
            authorizedHash = SharedConfig.passcodeHash;
            showForm();
            return;
        }
        if (hasAccess()) {
            showForm();
            return;
        }
        LinearLayout gate = new LinearLayout(this);
        gate.setOrientation(LinearLayout.VERTICAL);
        int spacing = getResources().getDimensionPixelSize(R.dimen.televim_spacing);
        gate.setPadding(spacing, spacing, spacing, spacing);
        TextView title = OfflineSettingsAppearance.text(this, text(R.string.TelevimImportUnlock),
                R.dimen.televim_title_size, R.color.televim_primary_text);
        title.setGravity(Gravity.CENTER);
        gate.addView(title);
        gate.addView(label(this, text(R.string.TelevimImportUnlockAbout)));
        passcode = field(this, R.string.TelevimImportAppPasscode, true);
        if (SharedConfig.passcodeType == SharedConfig.PASSCODE_TYPE_PIN) {
            passcode.setInputType(InputType.TYPE_CLASS_NUMBER | InputType.TYPE_NUMBER_VARIATION_PASSWORD);
        }
        passcode.setFilters(new InputFilter[]{new InputFilter.LengthFilter(1024)});
        gate.addView(passcode);
        unlock = button(this, R.string.TelevimImportUnlock, this::unlockImport);
        gate.addView(unlock);
        lockStatus = label(this, "");
        gate.addView(lockStatus);
        ScrollView scroll = new ScrollView(this);
        scroll.addView(gate);
        body.addView(scroll);
        updateRetry();
    }

    private void updateRetry() {
        ui.removeCallbacks(retryTick);
        if (!resumed || destroyed || authorized || unlock == null) {
            return;
        }
        long now = SystemClock.elapsedRealtime();
        if (SharedConfig.passcodeRetryInMs > 0) {
            if (now > SharedConfig.lastUptimeMillis) {
                SharedConfig.passcodeRetryInMs = Math.max(0,
                        SharedConfig.passcodeRetryInMs - (now - SharedConfig.lastUptimeMillis));
            }
            SharedConfig.lastUptimeMillis = now;
            SharedConfig.saveConfig();
        }
        boolean waiting = SharedConfig.passcodeRetryInMs > 0;
        boolean wasWaiting = !unlock.isEnabled();
        unlock.setEnabled(!waiting);
        if (waiting) {
            long seconds = Math.max(1, (SharedConfig.passcodeRetryInMs + 999) / 1000);
            lockStatus.setText(getString(R.string.TelevimImportRetry, seconds));
            ui.postDelayed(retryTick, 1000);
        } else if (wasWaiting) {
            lockStatus.setText(text(R.string.TelevimImportUnlockAbout));
        }
    }

    private void unlockImport() {
        if (!resumed || destroyed) {
            return;
        }
        updateRetry();
        if (SharedConfig.passcodeRetryInMs > 0) {
            return;
        }
        String value = passcode.getText().toString();
        passcode.setText("");
        if (value.isEmpty()) {
            lockStatus.setText(text(R.string.TelevimImportWrongPasscode));
            return;
        }
        if (!SharedConfig.checkPasscode(value)) {
            SharedConfig.increaseBadPasscodeTries();
            lockStatus.setText(text(R.string.TelevimImportWrongPasscode));
            updateRetry();
            return;
        }
        SharedConfig.badPasscodeTries = 0;
        SharedConfig.saveConfig();
        authorized = true;
        authorizedHash = SharedConfig.passcodeHash;
        ui.removeCallbacks(retryTick);
        showForm();
    }

    private void showForm() {
        body.removeAllViews();
        form = createContent(this);
        body.addView(form);
        if (OfflineSettingsStore.isUncertain()) {
            showEntry(false);
            status.setText(text(R.string.TelevimImportCleanupFailed));
        } else if (interrupted) {
            status.setText(text(R.string.TelevimImportInterrupted));
        }
        interrupted = false;
        resumeRequestedScan();
    }

    private void showScanner() {
        if (!ensureAccess() || busy) {
            return;
        }
        if (checkSelfPermission(Manifest.permission.CAMERA) != PackageManager.PERMISSION_GRANTED) {
            scanRequested = true;
            if (!cameraPermissionPending) {
                cameraPermissionPending = true;
                requestPermissions(new String[]{Manifest.permission.CAMERA}, CAMERA_PERMISSION);
            }
            return;
        }
        scanRequested = false;
        apply.setVisibility(View.GONE);
        if (!discardPlan()) {
            showEntry(false);
            status.setText(text(R.string.TelevimImportCleanupFailed));
            return;
        }
        status.setText("");
        int token = ++operation;
        scanner = new OfflineSettingsQrView(this, value -> {
            if (operation != token || scanner == null || !ensureAccess()) {
                return;
            }
            hideScanner();
            incoming = null;
            apply.setVisibility(View.GONE);
            link.setEnabled(true);
            link.setText(value);
            qrSource = true;
            showEntry(false);
            status.setText(text(R.string.TelevimImportQrRead));
        }, () -> {
            if (operation != token || scanner == null || !ensureAccess()) {
                return;
            }
            hideScanner();
            showEntry(false);
            status.setText(text(R.string.TelevimImportCameraUnavailable));
        });
        body.removeAllViews();
        body.addView(scanner);
        updateHeader();
        scanner.resume();
    }

    private void closeScanner() {
        if (scanner != null) {
            scanner.close();
            scanner = null;
        }
    }

    private void hideScanner() {
        closeScanner();
        body.removeAllViews();
        body.addView(form);
        updateHeader();
    }

    @Override
    public void onRequestPermissionsResult(int request, String[] permissions, int[] results) {
        super.onRequestPermissionsResult(request, permissions, results);
        if (request != CAMERA_PERMISSION || !cameraPermissionPending) {
            return;
        }
        cameraPermissionPending = false;
        if (results.length == 0 || results[0] != PackageManager.PERMISSION_GRANTED) {
            cameraError = scanRequested;
            scanRequested = false;
        }
        resumeRequestedScan();
    }

    private void resumeRequestedScan() {
        if (!hasAccess() || form == null || cameraPermissionPending) {
            return;
        }
        if (cameraError) {
            cameraError = false;
            showEntry(false);
            status.setText(text(R.string.TelevimImportCameraUnavailable));
        } else if (scanRequested) {
            showScanner();
        }
    }

    @Override
    public void onBackPressed() {
        if (scanner != null && ensureAccess()) {
            hideScanner();
            status.setText(text(R.string.TelevimImportCancelled));
        } else if (entryVisible && hasAccess()) {
            cancelTransfer();
        } else {
            super.onBackPressed();
        }
    }

    private void updateHeader() {
        boolean scanning = scanner != null;
        int background = scanning ? Color.BLACK : getColor(R.color.televim_background);
        root.setBackgroundColor(background);
        toolbar.setBackgroundColor(background);
        OfflineSettingsAppearance.setBackColor(back, scanning ? Color.WHITE : getColor(R.color.televim_primary_text));
        boolean light = !scanning && (getResources().getConfiguration().uiMode & Configuration.UI_MODE_NIGHT_MASK)
                != Configuration.UI_MODE_NIGHT_YES;
        int appearance = light ? View.SYSTEM_UI_FLAG_LIGHT_STATUS_BAR | View.SYSTEM_UI_FLAG_LIGHT_NAVIGATION_BAR : 0;
        getWindow().getDecorView().setSystemUiVisibility(appearance);
        getWindow().setStatusBarColor(background);
        getWindow().setNavigationBarColor(background);
    }

    private void showEntry(boolean explainFiles) {
        entryVisible = true;
        intro.setVisibility(View.GONE);
        entry.setVisibility(View.VISIBLE);
        fileHelp.setVisibility(explainFiles ? View.VISIBLE : View.GONE);
    }

    private void cancelTransfer() {
        if (!ensureAccess()) {
            return;
        }
        cancelWork();
        busy = false;
        boolean discarded = discardPlan();
        incoming = null;
        qrSource = false;
        password.setText("");
        link.setText("");
        apply.setVisibility(View.GONE);
        setBusy(false);
        status.setText(text(discarded ? R.string.TelevimImportCancelled : R.string.TelevimImportCleanupFailed));
        if (discarded) {
            entryVisible = false;
            entry.setVisibility(View.GONE);
            intro.setVisibility(View.VISIBLE);
        }
    }

    private View createContent(Context context) {
        ScrollView scroll = new ScrollView(context);
        scroll.setFillViewport(true);
        scroll.setBackgroundColor(getColor(R.color.televim_background));
        scroll.setImportantForAutofill(View.IMPORTANT_FOR_AUTOFILL_NO_EXCLUDE_DESCENDANTS);
        FrameLayout pages = new FrameLayout(context);
        scroll.addView(pages, new ScrollView.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));
        intro = new OfflineSettingsIntro(context, this::showScanner, () -> showEntry(false), () -> showEntry(true));
        pages.addView(intro, new FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));
        LinearLayout content = new LinearLayout(context);
        entry = content;
        content.setOrientation(LinearLayout.VERTICAL);
        int spacing = context.getResources().getDimensionPixelSize(R.dimen.televim_page_inset);
        content.setPadding(spacing, spacing, spacing, spacing);
        pages.addView(content, new FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));
        TextView title = OfflineSettingsAppearance.text(context, text(R.string.TelevimImportTitle),
                R.dimen.televim_title_size, R.color.televim_primary_text);
        title.setGravity(Gravity.CENTER);
        content.addView(title);
        content.addView(label(context, text(R.string.TelevimImportAbout)));
        link = field(context, R.string.TelevimImportLink, false);
        link.setFilters(new InputFilter[]{new InputFilter.LengthFilter(OfflineTransfer.MAX_LINK + 1)});
        content.addView(link);
        password = field(context, R.string.TelevimImportPassword, true);
        password.setFilters(new InputFilter[]{new InputFilter.LengthFilter(1025)});
        content.addView(password);
        scan = OfflineSettingsAppearance.button(context, R.string.TelevimImportScan, this::showScanner, false);
        content.addView(scan);
        inspect = button(context, R.string.TelevimImportPreview, this::inspect);
        content.addView(inspect);
        status = label(context, invalidInput ? text(R.string.TelevimImportInvalid) : "");
        content.addView(status);
        apply = button(context, R.string.TelevimImportApply, this::apply);
        apply.setVisibility(View.GONE);
        content.addView(apply);
        content.addView(OfflineSettingsAppearance.button(context, android.R.string.cancel, this::cancelTransfer, false));
        fileHelp = label(context, text(R.string.TelevimImportFiles));
        fileHelp.setVisibility(View.GONE);
        content.addView(OfflineSettingsAppearance.button(context, R.string.TelevimImportFromFile, () -> {
            fileHelp.setVisibility(fileHelp.getVisibility() == View.VISIBLE ? View.GONE : View.VISIBLE);
        }, false));
        content.addView(fileHelp);
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
        link.addTextChangedListener(inputChanged(true));
        password.addTextChangedListener(inputChanged(false));
        entryVisible = entryVisible || incoming != null || invalidInput || interrupted;
        intro.setVisibility(entryVisible ? View.GONE : View.VISIBLE);
        content.setVisibility(entryVisible ? View.VISIBLE : View.GONE);
        return scroll;
    }

    private TextWatcher inputChanged(boolean linkChanged) {
        return new TextWatcher() {
            @Override
            public void beforeTextChanged(CharSequence value, int start, int count, int after) {
            }

            @Override
            public void onTextChanged(CharSequence value, int start, int before, int count) {
                if (linkChanged) {
                    qrSource = false;
                }
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
    }

    private EditText field(Context context, int hint, boolean secret) {
        EditText field = new EditText(context);
        field.setHint(text(hint));
        OfflineSettingsAppearance.styleField(field);
        field.setInputType(InputType.TYPE_CLASS_TEXT | (secret ? InputType.TYPE_TEXT_VARIATION_PASSWORD : InputType.TYPE_TEXT_VARIATION_URI));
        field.setSingleLine(true);
        field.setSaveEnabled(false);
        field.setImportantForAutofill(View.IMPORTANT_FOR_AUTOFILL_NO);
        field.setImeOptions(EditorInfo.IME_FLAG_NO_PERSONALIZED_LEARNING | EditorInfo.IME_ACTION_DONE);
        return field;
    }

    private TextView label(Context context, String value) {
        TextView label = OfflineSettingsAppearance.text(context, value,
                R.dimen.televim_body_size, R.color.televim_secondary_text);
        int spacing = context.getResources().getDimensionPixelSize(R.dimen.televim_spacing);
        label.setPadding(0, spacing, 0, spacing);
        return label;
    }

    private TextView button(Context context, int title, Runnable clicked) {
        return OfflineSettingsAppearance.button(context, title, clicked, true);
    }

    private void inspect() {
        if (!ensureAccess() || busy) {
            return;
        }
        cancellation = null;
        if (OfflineSettingsStore.isUncertain()) {
            status.setText(text(R.string.TelevimImportRestart));
            return;
        }
        if (!WORKER_BUSY.compareAndSet(false, true)) {
            status.setText(text(R.string.TelevimImportFinishing));
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
        Context context = this.getApplicationContext();
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
                runOnUiThread(() -> {
                    boolean current = token == operation && cancellation == cancelled;
                    if (destroyed || !current || cancelled.get() || !hasAccess()) {
                        boolean discarded = prepared.discard();
                        prepared.transfer.clear();
                        if (current && !destroyed) {
                            cancellation = null;
                            if (!ensureAccess()) {
                                return;
                            }
                            setBusy(false);
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
                runOnUiThread(() -> {
                    if (destroyed || token != operation || cancellation != cancelled) {
                        return;
                    }
                    cancellation = null;
                    if (!ensureAccess()) {
                        return;
                    }
                    setBusy(false);
                    status.setText(text(cleanupFailed
                            ? R.string.TelevimImportCleanupFailed : R.string.TelevimImportInvalid));
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
            preview.append("\n\n").append(text(enter ? R.string.TelevimImportSendByEnter : R.string.TelevimImportLargeEmoji))
                    .append(": ").append(text(previous ? R.string.TelevimImportOn : R.string.TelevimImportOff))
                    .append(" → ").append(text(preference.getValue() ? R.string.TelevimImportOn : R.string.TelevimImportOff));
        }
        if (plan.transfer.preferences.isEmpty()) {
            preview.append("\n\n").append(text(R.string.TelevimImportNoSupported));
        }
        if (plan.transfer.unsupportedModules != 0 || plan.transfer.assets != 0 || !plan.transfer.accounts.isEmpty()) {
            preview.append("\n\n").append(getString(R.string.TelevimImportRetained,
                    plan.transfer.unsupportedModules, plan.transfer.assets, plan.transfer.accounts.size(), plan.transfer.original.length));
            for (String identity : plan.transfer.accounts.values()) {
                preview.append("\n").append(identity);
            }
            preview.append("\n\n").append(text(R.string.TelevimImportInactive));
        }
        if (plan.transfer.rawJson) {
            preview.append("\n\n").append(text(R.string.TelevimImportPlaintext));
        }
        status.setText(preview.toString());
        apply.setText(text(plan.transfer.preferences.isEmpty() ? R.string.TelevimImportRetainOnly : R.string.TelevimImportApply));
        apply.setVisibility(View.VISIBLE);
    }

    private void apply() {
        if (!ensureAccess() || plan == null || busy) {
            return;
        }
        OfflineSettingsStore.Applied result = OfflineSettingsStore.apply(this, plan);
        if (result == OfflineSettingsStore.Applied.SUCCESS || result == OfflineSettingsStore.Applied.UNCERTAIN) {
            OfflineSettingsStore.Baseline current = OfflineSettingsStore.capture(this);
            SharedConfig.allowBigEmoji = current.getBoolean("allowBigEmoji", true);
            if (ApplicationLoader.isApplicationInitialized()) {
                ChatActivityEnterView.refreshPortablePreferences(current.getBoolean("send_by_enter", false));
            }
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
        AtomicBoolean previous = cancellation;
        cancellation = null;
        if (previous != null) {
            previous.set(true);
        }
        ++operation;
    }

    private String text(int resource) {
        return getString(resource);
    }

    @Override
    protected void onDestroy() {
        destroyed = true;
        invalidateAccess();
        incoming = null;
        super.onDestroy();
    }
}
