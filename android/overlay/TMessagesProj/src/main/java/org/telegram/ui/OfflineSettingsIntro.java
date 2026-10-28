package org.telegram.ui;

import android.content.Context;
import android.content.res.ColorStateList;
import android.graphics.drawable.ColorDrawable;
import android.graphics.drawable.Drawable;
import android.graphics.drawable.RippleDrawable;
import android.text.SpannableStringBuilder;
import android.text.Spanned;
import android.text.style.ImageSpan;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.TextView;

import org.telegram.messenger.R;
import org.telegram.ui.Components.RLottieDrawable;

final class OfflineSettingsIntro extends LinearLayout {
    private static final int MAX_ANIMATION_PIXELS = 1024;

    private final ImageView illustration;
    private RLottieDrawable animation;

    OfflineSettingsIntro(Context context, Runnable scan, Runnable link, Runnable fileHelp) {
        super(context);
        setOrientation(VERTICAL);
        setBackgroundColor(OfflineSettingsAppearance.color(context, R.color.televim_section_background));
        illustration = new ImageView(context);
        illustration.setScaleType(ImageView.ScaleType.FIT_CENTER);
        illustration.setImportantForAccessibility(IMPORTANT_FOR_ACCESSIBILITY_NO);
        addView(new ImportCard(context, scan), new LayoutParams(LayoutParams.MATCH_PARENT, LayoutParams.WRAP_CONTENT));
        addView(new View(context), new LayoutParams(LayoutParams.MATCH_PARENT,
                dimension(R.dimen.televim_intro_section_gap)));
        addRow(R.string.TelevimImportFromLink, R.drawable.msg_link, link);
        addRow(R.string.TelevimImportFromFile, R.drawable.msg_view_file, fileHelp);
    }

    private void addRow(int resource, int iconResource, Runnable action) {
        Context context = getContext();
        LinearLayout row = new LinearLayout(context);
        row.setOrientation(LinearLayout.HORIZONTAL);
        row.setGravity(Gravity.CENTER_VERTICAL);
        row.setMinimumHeight(dimension(R.dimen.televim_intro_row_height));
        int horizontalInset = dimension(R.dimen.televim_intro_row_inset);
        int verticalInset = dimension(R.dimen.televim_intro_row_vertical_inset);
        row.setPaddingRelative(horizontalInset, verticalInset, horizontalInset, verticalInset);
        row.setBackground(new RippleDrawable(ColorStateList.valueOf(OfflineSettingsAppearance.color(context,
                R.color.televim_touch_feedback)),
                new ColorDrawable(OfflineSettingsAppearance.color(context, R.color.televim_background)), null));
        row.setFocusable(true);
        row.setContentDescription(context.getString(resource));
        row.setImportantForAccessibility(IMPORTANT_FOR_ACCESSIBILITY_YES);
        row.setAccessibilityDelegate(new View.AccessibilityDelegate() {
            @Override
            public void onInitializeAccessibilityNodeInfo(View host, android.view.accessibility.AccessibilityNodeInfo info) {
                super.onInitializeAccessibilityNodeInfo(host, info);
                info.setClassName("android.widget.Button");
            }
        });
        row.setOnClickListener(view -> action.run());

        ImageView icon = new ImageView(context);
        icon.setImageResource(iconResource);
        icon.setImageTintList(ColorStateList.valueOf(OfflineSettingsAppearance.color(context, R.color.televim_secondary_text)));
        icon.setScaleType(ImageView.ScaleType.FIT_CENTER);
        icon.setImportantForAccessibility(IMPORTANT_FOR_ACCESSIBILITY_NO);
        int iconSize = dimension(R.dimen.televim_intro_row_icon_size);
        row.addView(icon, new LayoutParams(iconSize, iconSize));
        TextView text = OfflineSettingsAppearance.text(context, context.getString(resource),
                R.dimen.televim_text_size, R.color.televim_primary_text);
        text.setGravity(Gravity.START | Gravity.CENTER_VERTICAL);
        text.setImportantForAccessibility(IMPORTANT_FOR_ACCESSIBILITY_NO);
        LayoutParams textParams = new LayoutParams(0, LayoutParams.WRAP_CONTENT, 1);
        textParams.setMarginStart(dimension(R.dimen.televim_intro_row_icon_gap));
        row.addView(text, textParams);
        addView(row, new LayoutParams(LayoutParams.MATCH_PARENT, LayoutParams.WRAP_CONTENT));
    }

    private int dimension(int resource) {
        return OfflineSettingsAppearance.dimension(getContext(), resource);
    }

    private final class ImportCard extends ViewGroup {
        private final TextView description;
        private final TextView scanButton;

        ImportCard(Context context, Runnable scan) {
            super(context);
            setBackgroundColor(OfflineSettingsAppearance.color(context, R.color.televim_background));
            addView(illustration);
            description = OfflineSettingsAppearance.text(context, context.getString(R.string.TelevimImportIntroAbout),
                    R.dimen.televim_body_size, R.color.televim_primary_text);
            description.setGravity(Gravity.CENTER);
            description.setLineSpacing(0, 1);
            addView(description);
            scanButton = OfflineSettingsAppearance.button(context, R.string.TelevimImportScan, scan, true);
            Drawable icon = context.getDrawable(R.drawable.msg_mini_qr).mutate();
            icon.setTint(OfflineSettingsAppearance.color(context, R.color.televim_button_text));
            int iconSize = dimension(R.dimen.televim_intro_scan_icon_size);
            icon.setBounds(0, 0, iconSize, iconSize);
            SpannableStringBuilder label = new SpannableStringBuilder("\uFFFC  ");
            label.append(context.getString(R.string.TelevimImportScan));
            label.setSpan(new ImageSpan(icon, ImageSpan.ALIGN_BASELINE), 0, 1, Spanned.SPAN_EXCLUSIVE_EXCLUSIVE);
            scanButton.setText(label);
            scanButton.setContentDescription(context.getString(R.string.TelevimImportScan));
            addView(scanButton);
        }

        @Override
        protected void onMeasure(int widthSpec, int heightSpec) {
            int width = MeasureSpec.getSize(widthSpec);
            int imageSize = Math.min(width, dimension(R.dimen.televim_intro_art_size));
            illustration.measure(MeasureSpec.makeMeasureSpec(imageSize, MeasureSpec.EXACTLY),
                    MeasureSpec.makeMeasureSpec(imageSize, MeasureSpec.EXACTLY));
            int unspecified = MeasureSpec.makeMeasureSpec(0, MeasureSpec.UNSPECIFIED);
            int descriptionWidth = Math.max(0, width - dimension(R.dimen.televim_intro_description_inset) * 2);
            description.measure(MeasureSpec.makeMeasureSpec(descriptionWidth, MeasureSpec.EXACTLY), unspecified);
            int buttonWidth = Math.max(0, width - dimension(R.dimen.televim_intro_card_inset) * 2);
            scanButton.measure(MeasureSpec.makeMeasureSpec(buttonWidth, MeasureSpec.EXACTLY), unspecified);
            int contentHeight = dimension(R.dimen.televim_intro_description_top)
                    + description.getMeasuredHeight() + dimension(R.dimen.televim_intro_button_gap)
                    + scanButton.getMeasuredHeight() + dimension(R.dimen.televim_intro_card_inset);
            setMeasuredDimension(width, resolveSize(Math.max(dimension(R.dimen.televim_intro_card_height),
                    contentHeight), heightSpec));
        }

        @Override
        protected void onLayout(boolean changed, int left, int top, int right, int bottom) {
            int width = right - left;
            int imageSize = illustration.getMeasuredWidth();
            int imageLeft = (width - imageSize) / 2;
            int imageTop = dimension(R.dimen.televim_intro_card_inset);
            illustration.layout(imageLeft, imageTop, imageLeft + imageSize, imageTop + imageSize);
            int descriptionLeft = (width - description.getMeasuredWidth()) / 2;
            int descriptionTop = dimension(R.dimen.televim_intro_description_top);
            description.layout(descriptionLeft, descriptionTop, descriptionLeft + description.getMeasuredWidth(),
                    descriptionTop + description.getMeasuredHeight());
            int buttonLeft = (width - scanButton.getMeasuredWidth()) / 2;
            int buttonBottom = bottom - top - dimension(R.dimen.televim_intro_card_inset);
            scanButton.layout(buttonLeft, buttonBottom - scanButton.getMeasuredHeight(),
                    buttonLeft + scanButton.getMeasuredWidth(), buttonBottom);
        }
    }

    private void updateAnimation() {
        if (illustration == null || !isAttachedToWindow()) {
            return;
        }
        if (getWindowVisibility() != VISIBLE || !isShown()) {
            if (animation != null) {
                animation.stop();
            }
            return;
        }
        if (animation == null) {
            Context context = getContext();
            int pixels = Math.max(1, Math.min(MAX_ANIMATION_PIXELS, dimension(R.dimen.televim_intro_art_size)));
            int background = OfflineSettingsAppearance.color(context, R.color.televim_background);
            int[] colors = {
                    0x333333, OfflineSettingsAppearance.color(context, R.color.televim_primary_text),
                    0xffffff, background,
                    0x50a7ea, OfflineSettingsAppearance.color(context, R.color.televim_accent),
                    0x212020, background
            };
            animation = new RLottieDrawable(R.raw.qr_login, pixels, pixels, false, colors);
            animation.setAllowVibration(false);
            animation.setAllowDecodeSingleFrame(true);
            illustration.setImageDrawable(animation);
        }
        animation.start();
    }

    @Override
    protected void onAttachedToWindow() {
        super.onAttachedToWindow();
        updateAnimation();
    }

    @Override
    protected void onVisibilityChanged(View changedView, int visibility) {
        super.onVisibilityChanged(changedView, visibility);
        updateAnimation();
    }

    @Override
    protected void onWindowVisibilityChanged(int visibility) {
        super.onWindowVisibilityChanged(visibility);
        updateAnimation();
    }

    @Override
    protected void onDetachedFromWindow() {
        if (animation != null) {
            RLottieDrawable previous = animation;
            animation = null;
            previous.stop();
            illustration.setImageDrawable(null);
            previous.setCallback(null);
            previous.recycle(true);
        }
        super.onDetachedFromWindow();
    }
}
