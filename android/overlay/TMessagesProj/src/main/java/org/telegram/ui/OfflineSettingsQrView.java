package org.telegram.ui;

import android.Manifest;
import android.app.Activity;
import android.content.pm.PackageManager;
import android.content.res.ColorStateList;
import android.graphics.Canvas;
import android.graphics.ImageFormat;
import android.graphics.Paint;
import android.graphics.Path;
import android.graphics.RectF;
import android.graphics.drawable.GradientDrawable;
import android.graphics.drawable.RippleDrawable;
import android.graphics.drawable.StateListDrawable;
import android.hardware.Camera;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.Surface;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.View;
import android.view.ViewGroup;
import android.widget.FrameLayout;
import android.widget.ImageButton;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

import com.google.zxing.BarcodeFormat;
import com.google.zxing.BinaryBitmap;
import com.google.zxing.DecodeHintType;
import com.google.zxing.MultiFormatReader;
import com.google.zxing.PlanarYUVLuminanceSource;
import com.google.zxing.common.HybridBinarizer;

import org.telegram.messenger.R;
import org.telegram.messenger.televim.OfflineTransfer;

import java.util.Arrays;
import java.util.Collections;
import java.util.EnumMap;
import java.util.List;
import java.util.Map;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.atomic.AtomicBoolean;

@SuppressWarnings("deprecation")
public final class OfflineSettingsQrView extends FrameLayout implements SurfaceHolder.Callback {
    private static final ExecutorService DECODER = Executors.newSingleThreadExecutor();
    private static final AtomicBoolean decoding = new AtomicBoolean();
    private final java.util.function.Consumer<String> accepted;
    private final Runnable unavailable;
    private final Activity activity;
    private final SurfaceView surface;
    private final View overlay;
    private final ScrollView instructions;
    private final TextView status;
    private final ImageButton torch;
    private final RectF frame = new RectF();
    private final int inset;
    private final int horizontalInset;
    private final int textGap;
    private final int torchGap;
    private final int torchSize;
    private Camera camera;
    private int previewWidth;
    private int previewHeight;
    private boolean resumed;
    private boolean finished;
    private boolean torchOn;
    private boolean compactPortrait;

    public OfflineSettingsQrView(Activity context, java.util.function.Consumer<String> accepted, Runnable unavailable) {
        super(context);
        activity = context;
        this.accepted = accepted;
        this.unavailable = unavailable;
        inset = getResources().getDimensionPixelSize(R.dimen.televim_qr_inset);
        horizontalInset = getResources().getDimensionPixelSize(R.dimen.televim_qr_text_inset);
        textGap = getResources().getDimensionPixelSize(R.dimen.televim_qr_text_gap);
        torchGap = getResources().getDimensionPixelSize(R.dimen.televim_qr_torch_gap);
        torchSize = getResources().getDimensionPixelSize(R.dimen.televim_qr_torch_size);
        setBackgroundColor(context.getColor(R.color.televim_qr_background));
        setClipChildren(true);
        setSaveFromParentEnabled(false);
        surface = new SurfaceView(context);
        surface.getHolder().addCallback(this);
        addView(surface);
        overlay = new FrameOverlay(context);
        overlay.setImportantForAccessibility(View.IMPORTANT_FOR_ACCESSIBILITY_NO);
        addView(overlay);
        LinearLayout copy = new LinearLayout(context);
        copy.setOrientation(LinearLayout.VERTICAL);
        TextView title = text(R.string.TelevimImportScan, R.dimen.televim_qr_title_size);
        copy.addView(title, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));
        status = text(R.string.TelevimImportScanAbout, R.dimen.televim_qr_text_size);
        status.setAccessibilityLiveRegion(View.ACCESSIBILITY_LIVE_REGION_POLITE);
        LinearLayout.LayoutParams description = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        description.topMargin = getResources().getDimensionPixelSize(R.dimen.televim_qr_description_gap);
        copy.addView(status, description);
        instructions = new ScrollView(context);
        instructions.setVerticalScrollBarEnabled(false);
        instructions.addView(copy);
        addView(instructions);
        torch = new ImageButton(context);
        torch.setPadding(0, 0, 0, 0);
        torch.setScaleType(ImageView.ScaleType.CENTER);
        torch.setImageResource(R.drawable.qr_flashlight);
        torch.setImageTintList(ColorStateList.valueOf(context.getColor(R.color.televim_qr_foreground)));
        StateListDrawable states = new StateListDrawable();
        states.addState(new int[]{android.R.attr.state_selected}, circle(R.color.televim_qr_torch_active));
        states.addState(new int[0], circle(R.color.televim_qr_torch_idle));
        torch.setBackground(new RippleDrawable(ColorStateList.valueOf(context.getColor(R.color.televim_qr_torch_active)), states, circle(R.color.televim_qr_foreground)));
        torch.setVisibility(View.INVISIBLE);
        torch.setOnClickListener(view -> toggleTorch());
        updateTorch();
        addView(torch);
    }

    private TextView text(int string, int size) {
        TextView result = new TextView(getContext());
        result.setText(string);
        result.setGravity(Gravity.CENTER);
        result.setTextColor(getContext().getColor(R.color.televim_qr_foreground));
        result.setTextSize(TypedValue.COMPLEX_UNIT_PX, getResources().getDimension(size));
        result.setLineSpacing(getResources().getDimension(R.dimen.televim_qr_line_spacing), 1);
        return result;
    }

    private GradientDrawable circle(int color) {
        GradientDrawable result = new GradientDrawable();
        result.setShape(GradientDrawable.OVAL);
        result.setColor(getContext().getColor(color));
        return result;
    }

    @Override
    protected void onMeasure(int widthMeasureSpec, int heightMeasureSpec) {
        int width = MeasureSpec.getSize(widthMeasureSpec);
        int height = MeasureSpec.getSize(heightMeasureSpec);
        setMeasuredDimension(width, height);
        float side = Math.min(width, height) * getResources().getFraction(R.fraction.televim_qr_frame_fraction, 1, 1);
        frame.set((width - side) / 2, (height - side) / 2, (width + side) / 2, (height + side) / 2);
        int surfaceWidth = width;
        int surfaceHeight = height;
        if (previewWidth > 0 && previewHeight > 0) {
            double scale = Math.max((double) width / previewWidth, (double) height / previewHeight);
            surfaceWidth = (int) Math.ceil(previewWidth * scale);
            surfaceHeight = (int) Math.ceil(previewHeight * scale);
        }
        surface.measure(MeasureSpec.makeMeasureSpec(surfaceWidth, MeasureSpec.EXACTLY), MeasureSpec.makeMeasureSpec(surfaceHeight, MeasureSpec.EXACTLY));
        overlay.measure(MeasureSpec.makeMeasureSpec(width, MeasureSpec.EXACTLY), MeasureSpec.makeMeasureSpec(height, MeasureSpec.EXACTLY));
        int textWidth = width > height ? (int) frame.left - inset * 2 : width - horizontalInset * 2;
        int textHeight = width > height ? height - inset * 2 : (int) frame.top - textGap - inset;
        int textWidthSpec = MeasureSpec.makeMeasureSpec(Math.max(0, textWidth), MeasureSpec.EXACTLY);
        compactPortrait = false;
        if (width <= height) {
            int compactTextHeight = Math.max(0, Math.min(height / 3, height - torchSize - inset * 4));
            instructions.measure(textWidthSpec, MeasureSpec.makeMeasureSpec(compactTextHeight, MeasureSpec.AT_MOST));
            compactPortrait = textHeight < instructions.getMeasuredHeight()
                    || frame.bottom + torchGap + torchSize + inset > height;
            if (compactPortrait) {
                float frameTop = inset * 2 + instructions.getMeasuredHeight();
                float frameBottom = height - inset * 2 - torchSize;
                side = Math.max(0, Math.min(side, Math.min(width - inset * 2, frameBottom - frameTop)));
                float centerY = frameTop + Math.max(0, frameBottom - frameTop) / 2;
                frame.set((width - side) / 2, centerY - side / 2, (width + side) / 2, centerY + side / 2);
            }
        }
        if (!compactPortrait) {
            instructions.measure(textWidthSpec, MeasureSpec.makeMeasureSpec(Math.max(0, textHeight), MeasureSpec.AT_MOST));
        }
        torch.measure(MeasureSpec.makeMeasureSpec(torchSize, MeasureSpec.EXACTLY), MeasureSpec.makeMeasureSpec(torchSize, MeasureSpec.EXACTLY));
        overlay.invalidate();
    }

    @Override
    protected void onLayout(boolean changed, int left, int top, int right, int bottom) {
        int width = right - left;
        int height = bottom - top;
        int surfaceLeft = (width - surface.getMeasuredWidth()) / 2;
        int surfaceTop = (height - surface.getMeasuredHeight()) / 2;
        surface.layout(surfaceLeft, surfaceTop, surfaceLeft + surface.getMeasuredWidth(), surfaceTop + surface.getMeasuredHeight());
        overlay.layout(0, 0, width, height);
        int textLeft = width > height ? inset : horizontalInset;
        int textTop = width > height
                ? (height - instructions.getMeasuredHeight()) / 2
                : compactPortrait ? inset
                : Math.max(inset, (int) frame.top - textGap - instructions.getMeasuredHeight());
        instructions.layout(textLeft, textTop, textLeft + instructions.getMeasuredWidth(), textTop + instructions.getMeasuredHeight());
        int torchLeft = width > height
                ? (int) (frame.right + (width - frame.right - torchSize) / 2)
                : (width - torchSize) / 2;
        int torchTop = width > height
                ? (height - torchSize) / 2
                : Math.min((int) frame.bottom + (compactPortrait ? inset : torchGap), height - inset - torchSize);
        torch.layout(torchLeft, torchTop, torchLeft + torchSize, torchTop + torchSize);
    }

    private void toggleTorch() {
        if (!resumed || finished || camera == null) {
            return;
        }
        try {
            Camera.Parameters parameters = camera.getParameters();
            parameters.setFlashMode(torchOn ? Camera.Parameters.FLASH_MODE_OFF : Camera.Parameters.FLASH_MODE_TORCH);
            camera.setParameters(parameters);
            torchOn = Camera.Parameters.FLASH_MODE_TORCH.equals(camera.getParameters().getFlashMode());
            updateTorch();
        } catch (RuntimeException error) {
            torch.setEnabled(false);
        }
    }

    private void updateTorch() {
        torch.setSelected(torchOn);
        torch.setContentDescription(getContext().getString(torchOn ? R.string.TelevimImportTorchOff : R.string.TelevimImportTorchOn));
    }

    private final class FrameOverlay extends View {
        private final Paint shade = new Paint();
        private final Paint corners = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final Path path = new Path();
        private final float length;
        private final float radius;

        FrameOverlay(Activity context) {
            super(context);
            shade.setColor(context.getColor(R.color.televim_qr_shade));
            corners.setColor(context.getColor(R.color.televim_qr_foreground));
            corners.setStyle(Paint.Style.STROKE);
            corners.setStrokeWidth(getResources().getDimension(R.dimen.televim_qr_corner_width));
            corners.setStrokeCap(Paint.Cap.ROUND);
            corners.setStrokeJoin(Paint.Join.ROUND);
            length = getResources().getDimension(R.dimen.televim_qr_corner_length);
            radius = getResources().getDimension(R.dimen.televim_qr_corner_radius);
        }

        @Override
        protected void onDraw(Canvas canvas) {
            canvas.drawRect(0, 0, getWidth(), frame.top, shade);
            canvas.drawRect(0, frame.bottom, getWidth(), getHeight(), shade);
            canvas.drawRect(0, frame.top, frame.left, frame.bottom, shade);
            canvas.drawRect(frame.right, frame.top, getWidth(), frame.bottom, shade);
            path.rewind();
            corner(frame.left, frame.top, 1, 1);
            corner(frame.right, frame.top, -1, 1);
            corner(frame.left, frame.bottom, 1, -1);
            corner(frame.right, frame.bottom, -1, -1);
            canvas.drawPath(path, corners);
        }

        private void corner(float x, float y, int horizontal, int vertical) {
            path.moveTo(x, y + length * vertical);
            path.lineTo(x, y + radius * vertical);
            path.quadTo(x, y, x + radius * horizontal, y);
            path.lineTo(x + length * horizontal, y);
        }
    }

    public void resume() {
        resumed = true;
        if (activity.checkSelfPermission(Manifest.permission.CAMERA) != PackageManager.PERMISSION_GRANTED) {
            unavailable.run();
        } else {
            openCamera();
        }
    }

    public void close() {
        resumed = false;
        finished = true;
        closeCamera();
    }

    private void openCamera() {
        if (!resumed || finished || camera != null || surface == null || !surface.getHolder().getSurface().isValid()
                || activity.checkSelfPermission(Manifest.permission.CAMERA) != PackageManager.PERMISSION_GRANTED) {
            return;
        }
        try {
            Camera.CameraInfo info = new Camera.CameraInfo();
            int selected = -1;
            for (int i = 0; i < Camera.getNumberOfCameras(); ++i) {
                Camera.getCameraInfo(i, info);
                if (info.facing == Camera.CameraInfo.CAMERA_FACING_BACK) {
                    selected = i;
                    break;
                }
            }
            if (selected < 0) {
                throw new IllegalStateException("Back camera unavailable");
            }
            camera = Camera.open(selected);
            Camera.Parameters parameters = camera.getParameters();
            Camera.Size selectedSize = null;
            for (Camera.Size size : parameters.getSupportedPreviewSizes()) {
                if (size.width * size.height <= 1920 * 1080
                        && (selectedSize == null || size.width * size.height > selectedSize.width * selectedSize.height)) {
                    selectedSize = size;
                }
            }
            if (selectedSize == null) {
                throw new IllegalStateException("Bounded camera preview unavailable");
            }
            parameters.setPreviewSize(selectedSize.width, selectedSize.height);
            parameters.setPreviewFormat(ImageFormat.NV21);
            List<String> focusModes = parameters.getSupportedFocusModes();
            if (focusModes != null && focusModes.contains(Camera.Parameters.FOCUS_MODE_CONTINUOUS_PICTURE)) {
                parameters.setFocusMode(Camera.Parameters.FOCUS_MODE_CONTINUOUS_PICTURE);
            }
            List<String> flashModes = parameters.getSupportedFlashModes();
            boolean hasTorch = flashModes != null && flashModes.contains(Camera.Parameters.FLASH_MODE_TORCH)
                    && flashModes.contains(Camera.Parameters.FLASH_MODE_OFF);
            if (hasTorch) {
                parameters.setFlashMode(Camera.Parameters.FLASH_MODE_OFF);
            }
            camera.setParameters(parameters);
            int rotation = activity.getWindowManager().getDefaultDisplay().getRotation();
            int degrees = rotation == Surface.ROTATION_90 ? 90 : rotation == Surface.ROTATION_180 ? 180 : rotation == Surface.ROTATION_270 ? 270 : 0;
            int orientation = (info.orientation - degrees + 360) % 360;
            camera.setDisplayOrientation(orientation);
            previewWidth = orientation % 180 == 0 ? selectedSize.width : selectedSize.height;
            previewHeight = orientation % 180 == 0 ? selectedSize.height : selectedSize.width;
            requestLayout();
            camera.setPreviewDisplay(surface.getHolder());
            int width = selectedSize.width;
            int height = selectedSize.height;
            Camera owner = camera;
            camera.setPreviewCallback((bytes, ignored) -> decode(bytes, width, height, owner));
            camera.startPreview();
            torchOn = false;
            torch.setEnabled(true);
            torch.setVisibility(hasTorch ? View.VISIBLE : View.INVISIBLE);
            updateTorch();
        } catch (Exception error) {
            closeCamera();
            unavailable.run();
        }
    }

    private void decode(byte[] bytes, int width, int height, Camera owner) {
        if (!resumed || finished || bytes == null || !decoding.compareAndSet(false, true)) {
            return;
        }
        byte[] frame = bytes.clone();
        DECODER.execute(() -> {
            String value = null;
            try {
                Map<DecodeHintType, Object> hints = new EnumMap<>(DecodeHintType.class);
                hints.put(DecodeHintType.POSSIBLE_FORMATS, Collections.singletonList(BarcodeFormat.QR_CODE));
                hints.put(DecodeHintType.TRY_HARDER, true);
                MultiFormatReader reader = new MultiFormatReader();
                PlanarYUVLuminanceSource source = new PlanarYUVLuminanceSource(frame, width, height, 0, 0, width, height, false);
                value = reader.decode(new BinaryBitmap(new HybridBinarizer(source)), hints).getText();
            } catch (Exception ignored) {
            } finally {
                Arrays.fill(frame, (byte) 0);
                decoding.set(false);
            }
            String decoded = value;
            if (decoded != null) {
                activity.runOnUiThread(() -> {
                    if (!resumed || finished || camera != owner) {
                        return;
                    }
                    try {
                        byte[] checked = OfflineTransfer.decodeLink(decoded, true);
                        Arrays.fill(checked, (byte) 0);
                        finished = true;
                        closeCamera();
                        accepted.accept(decoded);
                    } catch (Exception error) {
                        status.setText(activity.getString(R.string.TelevimImportInvalidQr));
                    }
                });
            }
        });
    }

    private void closeCamera() {
        Camera previous = camera;
        camera = null;
        torchOn = false;
        torch.setVisibility(View.INVISIBLE);
        updateTorch();
        if (previous != null) {
            previous.setPreviewCallback(null);
            previous.release();
        }
    }

    @Override
    public void surfaceCreated(SurfaceHolder holder) {
        openCamera();
    }

    @Override
    public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) {
        if (camera == null) {
            openCamera();
        } else {
            try {
                camera.stopPreview();
                camera.setPreviewDisplay(holder);
                camera.startPreview();
            } catch (Exception error) {
                closeCamera();
                unavailable.run();
            }
        }
    }

    @Override
    public void surfaceDestroyed(SurfaceHolder holder) {
        closeCamera();
    }
}
