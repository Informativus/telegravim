package org.telegram.ui;

import android.content.Context;
import android.content.res.ColorStateList;
import android.graphics.Color;
import android.graphics.Typeface;
import android.graphics.drawable.ColorDrawable;
import android.graphics.drawable.GradientDrawable;
import android.graphics.drawable.LayerDrawable;
import android.graphics.drawable.RippleDrawable;
import android.graphics.drawable.StateListDrawable;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.View;
import android.widget.EditText;
import android.widget.ImageView;
import android.widget.TextView;

import org.telegram.messenger.R;
import org.telegram.ui.ActionBar.BackDrawable;

final class OfflineSettingsAppearance {
    private OfflineSettingsAppearance() {
    }

    static int dimension(Context context, int resource) {
        return context.getResources().getDimensionPixelSize(resource);
    }

    static int color(Context context, int resource) {
        return context.getColor(resource);
    }

    static TextView text(Context context, String value, int size, int color) {
        TextView result = new TextView(context);
        result.setText(value);
        result.setTextSize(TypedValue.COMPLEX_UNIT_PX, context.getResources().getDimension(size));
        result.setTextColor(color(context, color));
        result.setLineSpacing(dimension(context, R.dimen.televim_line_gap), 1);
        return result;
    }

    static TextView button(Context context, int title, Runnable action, boolean primary) {
        return button(context, title, action, primary, R.dimen.televim_button_radius);
    }

    static TextView sheetButton(Context context, int title, Runnable action) {
        return button(context, title, action, true, R.dimen.televim_sheet_button_radius);
    }

    private static TextView button(Context context, int title, Runnable action, boolean primary, int radius) {
        TextView result = new TextView(context) {
            @Override
            public void setEnabled(boolean enabled) {
                super.setEnabled(enabled);
                setAlpha(enabled ? 1 : 0.5f);
            }
        };
        result.setText(title);
        result.setTextSize(TypedValue.COMPLEX_UNIT_PX, context.getResources().getDimension(R.dimen.televim_button_size));
        result.setTypeface(Typeface.create("sans-serif-medium", Typeface.NORMAL));
        result.setGravity(Gravity.CENTER);
        result.setMinimumHeight(dimension(context, R.dimen.televim_button_height));
        result.setTextColor(color(context, primary ? R.color.televim_button_text : R.color.televim_accent));
        int inset = dimension(context, R.dimen.televim_button_inset);
        result.setPadding(inset, 0, inset, 0);
        GradientDrawable shape = new GradientDrawable();
        shape.setCornerRadius(dimension(context, radius));
        shape.setColor(primary ? color(context, R.color.televim_accent) : Color.TRANSPARENT);
        result.setBackground(new RippleDrawable(ColorStateList.valueOf(color(context,
                primary ? R.color.televim_accent_pressed : R.color.televim_touch_feedback)), shape, null));
        result.setFocusable(true);
        result.setAccessibilityDelegate(new View.AccessibilityDelegate() {
            @Override
            public void onInitializeAccessibilityNodeInfo(View host, android.view.accessibility.AccessibilityNodeInfo info) {
                super.onInitializeAccessibilityNodeInfo(host, info);
                info.setClassName("android.widget.Button");
            }
        });
        result.setOnClickListener(view -> action.run());
        return result;
    }

    static void styleField(EditText field) {
        Context context = field.getContext();
        field.setTextColor(color(context, R.color.televim_primary_text));
        field.setHintTextColor(color(context, R.color.televim_secondary_text));
        field.setTextSize(TypedValue.COMPLEX_UNIT_PX, context.getResources().getDimension(R.dimen.televim_text_size));
        field.setPadding(0, 0, 0, 0);
        field.setMinimumHeight(dimension(context, R.dimen.televim_field_height));
        StateListDrawable underline = new StateListDrawable();
        underline.addState(new int[]{android.R.attr.state_focused}, fieldLine(context, R.color.televim_accent));
        underline.addState(new int[]{}, fieldLine(context, R.color.televim_field_line));
        field.setBackground(underline);
        field.setBackgroundTintList(null);
    }

    private static LayerDrawable fieldLine(Context context, int color) {
        LayerDrawable result = new LayerDrawable(new android.graphics.drawable.Drawable[]{new ColorDrawable(color(context, color))});
        result.setLayerHeight(0, dimension(context, R.dimen.televim_field_line_width));
        result.setLayerGravity(0, Gravity.BOTTOM);
        return result;
    }

    static ImageView back(Context context, Runnable action) {
        ImageView view = new ImageView(context);
        int inset = dimension(context, R.dimen.televim_back_inset);
        view.setPadding(inset, inset, inset, inset);
        view.setScaleType(ImageView.ScaleType.CENTER);
        view.setContentDescription(context.getString(R.string.TelevimImportBack));
        view.setFocusable(true);
        view.setBackground(new RippleDrawable(ColorStateList.valueOf(color(context, R.color.televim_touch_feedback)), null,
                new ColorDrawable(Color.WHITE)));
        view.setOnClickListener(ignored -> action.run());
        setBackColor(view, color(context, R.color.televim_primary_text));
        return view;
    }

    static void setBackColor(ImageView view, int color) {
        BackDrawable drawable = new BackDrawable(false);
        drawable.setColor(color);
        view.setImageDrawable(drawable);
    }
}
