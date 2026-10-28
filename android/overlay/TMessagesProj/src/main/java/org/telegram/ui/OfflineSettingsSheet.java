package org.telegram.ui;

import android.app.Activity;
import android.app.Dialog;
import android.graphics.Color;
import android.graphics.Typeface;
import android.graphics.drawable.ColorDrawable;
import android.graphics.drawable.Drawable;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.view.Window;
import android.view.WindowManager;
import android.widget.FrameLayout;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

import org.telegram.messenger.R;

final class OfflineSettingsSheet extends Dialog {
    private final TextView title;
    private final boolean camera;
    private boolean silent;

    OfflineSettingsSheet(Activity activity, int titleResource, View content, boolean camera, Runnable cancelled) {
        super(activity, R.style.TransparentDialog);
        this.camera = camera;
        Window window = getWindow();
        window.addFlags(WindowManager.LayoutParams.FLAG_SECURE | WindowManager.LayoutParams.FLAG_DIM_BEHIND);
        window.setBackgroundDrawable(new ColorDrawable(Color.TRANSPARENT));
        window.setSoftInputMode(WindowManager.LayoutParams.SOFT_INPUT_ADJUST_RESIZE);
        window.setDimAmount(0.35f);
        window.setGravity(Gravity.BOTTOM);
        setCanceledOnTouchOutside(true);
        setOnDismissListener(dialog -> {
            if (!silent) {
                cancelled.run();
            }
        });
        if (content.getParent() instanceof ViewGroup) {
            ((ViewGroup) content.getParent()).removeView(content);
        }

        int headerHeight = OfflineSettingsAppearance.dimension(activity, R.dimen.televim_toolbar_height);
        int backSize = OfflineSettingsAppearance.dimension(activity, R.dimen.televim_back_size);
        FrameLayout header = new FrameLayout(activity);
        ImageView back = OfflineSettingsAppearance.back(activity, this::cancel);
        header.addView(back, new FrameLayout.LayoutParams(backSize, backSize, Gravity.START | Gravity.CENTER_VERTICAL));
        title = OfflineSettingsAppearance.text(activity, activity.getString(titleResource),
                R.dimen.televim_header_size, R.color.televim_primary_text);
        title.setTypeface(Typeface.create("sans-serif-medium", Typeface.NORMAL));
        title.setSingleLine(true);
        title.setGravity(Gravity.CENTER_VERTICAL);
        FrameLayout.LayoutParams titleParams = new FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT);
        titleParams.setMarginStart(OfflineSettingsAppearance.dimension(activity, R.dimen.televim_header_inset));
        titleParams.setMarginEnd(OfflineSettingsAppearance.dimension(activity, R.dimen.televim_sheet_inset));
        header.addView(title, titleParams);

        if (camera) {
            FrameLayout frame = new FrameLayout(activity);
            frame.setBackgroundColor(Color.BLACK);
            frame.setFitsSystemWindows(true);
            frame.addView(content, new FrameLayout.LayoutParams(
                    ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
            frame.addView(header, new FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, headerHeight));
            title.setTextColor(Color.WHITE);
            OfflineSettingsAppearance.setBackColor(back, Color.WHITE);
            header.addOnLayoutChangeListener((view, left, top, right, bottom, oldLeft, oldTop, oldRight, oldBottom) -> {
                if (content instanceof OfflineSettingsQrView) {
                    ((OfflineSettingsQrView) content).setHeaderBottom(header.getBottom() - content.getTop());
                }
            });
            setContentView(frame);
            window.setStatusBarColor(Color.BLACK);
            window.setNavigationBarColor(Color.BLACK);
            window.getDecorView().setSystemUiVisibility(0);
        } else {
            LinearLayout layout = new LinearLayout(activity);
            layout.setOrientation(LinearLayout.VERTICAL);
            layout.setFitsSystemWindows(true);
            Drawable background = activity.getDrawable(R.drawable.sheet_shadow_round).mutate();
            background.setTint(OfflineSettingsAppearance.color(activity, R.color.televim_background));
            layout.setBackground(background);
            layout.addView(header, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, headerHeight));
            ScrollView scroll = new ScrollView(activity);
            scroll.setImportantForAutofill(View.IMPORTANT_FOR_AUTOFILL_NO_EXCLUDE_DESCENDANTS);
            scroll.addView(content, new ScrollView.LayoutParams(
                    ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));
            layout.addView(scroll, new LinearLayout.LayoutParams(
                    ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));
            setContentView(layout);
            window.setNavigationBarColor(OfflineSettingsAppearance.color(activity, R.color.televim_background));
        }
    }

    void setHeading(int resource) {
        title.setText(resource);
    }

    void dismissSilently() {
        silent = true;
        dismiss();
    }

    @Override
    public void show() {
        super.show();
        getWindow().setLayout(ViewGroup.LayoutParams.MATCH_PARENT,
                camera ? ViewGroup.LayoutParams.MATCH_PARENT : ViewGroup.LayoutParams.WRAP_CONTENT);
    }
}
