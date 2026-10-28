package org.telegram.ui;

import android.content.Context;
import android.content.res.Configuration;
import android.graphics.Typeface;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.TextView;

import org.telegram.messenger.R;
import org.telegram.ui.Components.RLottieDrawable;

final class OfflineSettingsIntro extends ViewGroup {
    private static final int MAX_ANIMATION_PIXELS = 1024;

    private final ImageView illustration;
    private final TextView title;
    private final LinearLayout steps;
    private final LinearLayout actions;
    private RLottieDrawable animation;
    private boolean landscape;
    private int contentLeft;
    private int contentWidth;

    OfflineSettingsIntro(Context context, Runnable scan, Runnable link, Runnable fileHelp) {
        super(context);
        setBackgroundColor(OfflineSettingsAppearance.color(context, R.color.televim_background));
        illustration = new ImageView(context);
        illustration.setScaleType(ImageView.ScaleType.FIT_CENTER);
        illustration.setImportantForAccessibility(IMPORTANT_FOR_ACCESSIBILITY_NO);
        addView(illustration);

        title = OfflineSettingsAppearance.text(context, context.getString(R.string.TelevimImportIntroTitle),
                R.dimen.televim_title_size, R.color.televim_primary_text);
        title.setGravity(Gravity.CENTER);
        addView(title);

        steps = new LinearLayout(context);
        steps.setOrientation(LinearLayout.VERTICAL);
        addStep(1, R.string.TelevimImportStepOne);
        addStep(2, R.string.TelevimImportStepTwo);
        addStep(3, R.string.TelevimImportStepThree);
        addView(steps);

        actions = new LinearLayout(context);
        actions.setOrientation(LinearLayout.VERTICAL);
        actions.setGravity(Gravity.CENTER_HORIZONTAL);
        addAction(R.string.TelevimImportScan, scan, true);
        addAction(R.string.TelevimImportFromLink, link, false);
        addAction(R.string.TelevimImportFromFile, fileHelp, false);
        addView(actions);
    }

    private void addStep(int index, int resource) {
        Context context = getContext();
        LinearLayout row = new LinearLayout(context);
        row.setOrientation(LinearLayout.HORIZONTAL);
        row.setGravity(Gravity.TOP);
        TextView number = OfflineSettingsAppearance.text(context, index + ".",
                R.dimen.televim_body_size, R.color.televim_primary_text);
        number.setTypeface(Typeface.create("sans-serif-medium", Typeface.NORMAL));
        TextView text = OfflineSettingsAppearance.text(context, context.getString(resource),
                R.dimen.televim_body_size, R.color.televim_primary_text);
        text.setGravity(Gravity.START);
        LinearLayout.LayoutParams numberParams = new LinearLayout.LayoutParams(
                LayoutParams.WRAP_CONTENT, LayoutParams.WRAP_CONTENT);
        numberParams.setMarginEnd(dimension(R.dimen.televim_number_gap));
        row.addView(number, numberParams);
        row.addView(text, new LinearLayout.LayoutParams(0, LayoutParams.WRAP_CONTENT, 1));
        LinearLayout.LayoutParams rowParams = new LinearLayout.LayoutParams(
                LayoutParams.MATCH_PARENT, LayoutParams.WRAP_CONTENT);
        if (index > 1) {
            rowParams.topMargin = dimension(R.dimen.televim_step_gap);
        }
        steps.addView(row, rowParams);
    }

    private void addAction(int resource, Runnable action, boolean primary) {
        TextView button = OfflineSettingsAppearance.button(getContext(), resource, action, primary);
        actions.addView(button, new LinearLayout.LayoutParams(
                LayoutParams.WRAP_CONTENT, LayoutParams.WRAP_CONTENT));
    }

    private int dimension(int resource) {
        return OfflineSettingsAppearance.dimension(getContext(), resource);
    }

    private void measureContent(int width) {
        int titleInset = Math.max(0, dimension(R.dimen.televim_title_inset)
                - dimension(R.dimen.televim_page_inset));
        int unspecified = MeasureSpec.makeMeasureSpec(0, MeasureSpec.UNSPECIFIED);
        title.measure(MeasureSpec.makeMeasureSpec(Math.max(1, width - titleInset * 2), MeasureSpec.EXACTLY),
                unspecified);
        steps.measure(MeasureSpec.makeMeasureSpec(width, MeasureSpec.EXACTLY), unspecified);
        actions.measure(MeasureSpec.makeMeasureSpec(width, MeasureSpec.EXACTLY), unspecified);
    }

    private int contentHeight() {
        return title.getMeasuredHeight() + steps.getMeasuredHeight() + actions.getMeasuredHeight()
                + dimension(R.dimen.televim_page_inset) * 2;
    }

    @Override
    protected void onMeasure(int widthSpec, int heightSpec) {
        int inset = dimension(R.dimen.televim_page_inset);
        int animationSize = dimension(R.dimen.televim_animation_size);
        int width = resolveSize(animationSize + inset * 2, widthSpec);
        int heightMode = MeasureSpec.getMode(heightSpec);
        int availableHeight = MeasureSpec.getSize(heightSpec);
        int landscapeHeight = dimension(R.dimen.televim_intro_landscape_height);
        if (heightMode == MeasureSpec.UNSPECIFIED) {
            availableHeight = landscapeHeight;
        }
        landscape = getResources().getConfiguration().orientation == Configuration.ORIENTATION_LANDSCAPE
                && width > availableHeight;
        int imageSize;
        int desiredHeight;
        if (landscape) {
            contentLeft = (int) (width * 0.4f) + inset;
            contentWidth = Math.max(1, width - contentLeft - inset);
            measureContent(contentWidth);
            landscape = contentHeight() + inset * 2 <= Math.max(landscapeHeight, availableHeight);
        }
        if (landscape) {
            imageSize = Math.max(1, Math.min(animationSize, contentLeft - inset * 2));
            desiredHeight = Math.max(landscapeHeight, Math.max(imageSize, contentHeight()) + inset * 2);
        } else {
            contentLeft = inset;
            contentWidth = Math.max(1, width - inset * 2);
            measureContent(contentWidth);
            imageSize = Math.min(animationSize, contentWidth);
            desiredHeight = Math.max(dimension(R.dimen.televim_intro_min_height),
                    imageSize + contentHeight() + inset * 3);
        }
        illustration.measure(MeasureSpec.makeMeasureSpec(imageSize, MeasureSpec.EXACTLY),
                MeasureSpec.makeMeasureSpec(imageSize, MeasureSpec.EXACTLY));
        setMeasuredDimension(width, resolveSize(desiredHeight, heightSpec));
    }

    @Override
    protected void onLayout(boolean changed, int left, int top, int right, int bottom) {
        int width = right - left;
        int height = bottom - top;
        int inset = dimension(R.dimen.televim_page_inset);
        int imageSize = illustration.getMeasuredHeight();
        if (landscape) {
            int imageLeft = (contentLeft - inset - imageSize) / 2;
            int imageTop = Math.max(inset, (height - imageSize) / 2);
            illustration.layout(imageLeft, imageTop, imageLeft + imageSize, imageTop + imageSize);
            int contentTop = Math.max(inset, (height - contentHeight()) / 2);
            layoutCentered(title, contentTop);
            contentTop += title.getMeasuredHeight() + inset;
            layoutCentered(steps, contentTop);
            layoutCentered(actions, contentTop + steps.getMeasuredHeight() + inset);
            return;
        }
        int imageTop = Math.max(inset, (int) (height * 0.06f));
        int titleTop = (int) (height * 0.463f);
        int stepsTop = (int) (height * 0.543f);
        int actionsTop = height - inset - actions.getMeasuredHeight();
        if (imageTop + imageSize > titleTop
                || titleTop + title.getMeasuredHeight() + inset > stepsTop
                || stepsTop + steps.getMeasuredHeight() + inset > actionsTop) {
            int extra = Math.max(0, height - imageSize - contentHeight() - inset * 3);
            imageTop = inset + extra / 4;
            titleTop = imageTop + imageSize + inset + extra / 4;
            stepsTop = titleTop + title.getMeasuredHeight() + inset;
            actionsTop = Math.max(actionsTop, stepsTop + steps.getMeasuredHeight() + inset);
        }
        int imageLeft = (width - imageSize) / 2;
        illustration.layout(imageLeft, imageTop, imageLeft + imageSize, imageTop + imageSize);
        layoutCentered(title, titleTop);
        layoutCentered(steps, stepsTop);
        layoutCentered(actions, actionsTop);
    }

    private void layoutCentered(View child, int top) {
        int left = contentLeft + (contentWidth - child.getMeasuredWidth()) / 2;
        child.layout(left, top, left + child.getMeasuredWidth(), top + child.getMeasuredHeight());
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
            int pixels = Math.max(1, Math.min(MAX_ANIMATION_PIXELS, dimension(R.dimen.televim_animation_size)));
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
