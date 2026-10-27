package org.telegram.ui;

import android.Manifest;
import android.content.Context;
import android.content.pm.PackageManager;
import android.graphics.ImageFormat;
import android.hardware.Camera;
import android.view.Surface;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.View;
import android.view.ViewGroup;
import android.widget.LinearLayout;
import android.widget.TextView;

import com.google.zxing.BarcodeFormat;
import com.google.zxing.BinaryBitmap;
import com.google.zxing.DecodeHintType;
import com.google.zxing.MultiFormatReader;
import com.google.zxing.PlanarYUVLuminanceSource;
import com.google.zxing.common.HybridBinarizer;

import org.telegram.messenger.AndroidUtilities;
import org.telegram.messenger.LocaleController;
import org.telegram.messenger.R;
import org.telegram.messenger.televim.OfflineTransfer;
import org.telegram.ui.ActionBar.ActionBar;
import org.telegram.ui.ActionBar.BaseFragment;
import org.telegram.ui.ActionBar.Theme;

import java.util.Arrays;
import java.util.Collections;
import java.util.EnumMap;
import java.util.Map;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.atomic.AtomicBoolean;

@SuppressWarnings("deprecation")
public final class OfflineSettingsQrActivity extends BaseFragment implements SurfaceHolder.Callback {
    private static final int CAMERA_PERMISSION = 65174;
    private static final ExecutorService DECODER = Executors.newSingleThreadExecutor();
    private final java.util.function.Consumer<String> accepted;
    private final AtomicBoolean decoding = new AtomicBoolean();
    private Camera camera;
    private SurfaceView surface;
    private TextView status;
    private boolean resumed;
    private boolean finished;

    public OfflineSettingsQrActivity(java.util.function.Consumer<String> accepted) {
        this.accepted = accepted;
    }

    @Override
    protected boolean shouldCancelAccountRequestsOnDestroy() {
        return false;
    }

    @Override
    public View createView(Context context) {
        actionBar.setTitle(LocaleController.getString(R.string.TelevimImportScan));
        actionBar.setBackButtonImage(R.drawable.ic_ab_back);
        actionBar.setActionBarMenuOnItemClick(new ActionBar.ActionBarMenuOnItemClick() {
            @Override
            public void onItemClick(int id) {
                if (id == -1) {
                    finishFragment();
                }
            }
        });
        LinearLayout content = new LinearLayout(context);
        content.setOrientation(LinearLayout.VERTICAL);
        content.setBackgroundColor(Theme.getColor(Theme.key_windowBackgroundWhite));
        status = new TextView(context);
        status.setText(LocaleController.getString(R.string.TelevimImportScanAbout));
        status.setTextColor(Theme.getColor(Theme.key_windowBackgroundWhiteBlackText));
        int padding = context.getResources().getDimensionPixelSize(R.dimen.televim_spacing);
        status.setPadding(padding, padding, padding, padding);
        content.addView(status);
        surface = new SurfaceView(context);
        surface.getHolder().addCallback(this);
        content.addView(surface, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, 0, 1));
        fragmentView = content;
        return content;
    }

    @Override
    public void onResume() {
        super.onResume();
        resumed = true;
        if (getParentActivity().checkSelfPermission(Manifest.permission.CAMERA) != PackageManager.PERMISSION_GRANTED) {
            getParentActivity().requestPermissions(new String[]{Manifest.permission.CAMERA}, CAMERA_PERMISSION);
        } else {
            openCamera();
        }
    }

    @Override
    public void onPause() {
        resumed = false;
        closeCamera();
        super.onPause();
    }

    @Override
    public void onFragmentDestroy() {
        finished = true;
        closeCamera();
        super.onFragmentDestroy();
    }

    @Override
    public void onRequestPermissionsResultFragment(int requestCode, String[] permissions, int[] results) {
        if (requestCode == CAMERA_PERMISSION && results.length > 0 && results[0] == PackageManager.PERMISSION_GRANTED) {
            openCamera();
        } else if (requestCode == CAMERA_PERMISSION) {
            status.setText(LocaleController.getString(R.string.TelevimImportCameraUnavailable));
        }
    }

    private void openCamera() {
        if (!resumed || finished || camera != null || surface == null || !surface.getHolder().getSurface().isValid()
                || getParentActivity().checkSelfPermission(Manifest.permission.CAMERA) != PackageManager.PERMISSION_GRANTED) {
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
                if (size.width * size.height <= 1280 * 720
                        && (selectedSize == null || size.width * size.height > selectedSize.width * selectedSize.height)) {
                    selectedSize = size;
                }
            }
            if (selectedSize == null) {
                throw new IllegalStateException("Bounded camera preview unavailable");
            }
            parameters.setPreviewSize(selectedSize.width, selectedSize.height);
            parameters.setPreviewFormat(ImageFormat.NV21);
            if (parameters.getSupportedFocusModes().contains(Camera.Parameters.FOCUS_MODE_CONTINUOUS_PICTURE)) {
                parameters.setFocusMode(Camera.Parameters.FOCUS_MODE_CONTINUOUS_PICTURE);
            }
            camera.setParameters(parameters);
            int rotation = getParentActivity().getWindowManager().getDefaultDisplay().getRotation();
            int degrees = rotation == Surface.ROTATION_90 ? 90 : rotation == Surface.ROTATION_180 ? 180 : rotation == Surface.ROTATION_270 ? 270 : 0;
            camera.setDisplayOrientation((info.orientation - degrees + 360) % 360);
            camera.setPreviewDisplay(surface.getHolder());
            int width = selectedSize.width;
            int height = selectedSize.height;
            Camera owner = camera;
            camera.setPreviewCallback((bytes, ignored) -> decode(bytes, width, height, owner));
            camera.startPreview();
        } catch (Exception error) {
            closeCamera();
            status.setText(LocaleController.getString(R.string.TelevimImportCameraUnavailable));
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
                AndroidUtilities.runOnUIThread(() -> {
                    if (!resumed || finished || camera != owner) {
                        return;
                    }
                    try {
                        byte[] checked = OfflineTransfer.decodeLink(decoded, true);
                        Arrays.fill(checked, (byte) 0);
                        finished = true;
                        closeCamera();
                        accepted.accept(decoded);
                        finishFragment();
                    } catch (Exception error) {
                        status.setText(LocaleController.getString(R.string.TelevimImportInvalidQr));
                    }
                });
            }
        });
    }

    private void closeCamera() {
        Camera previous = camera;
        camera = null;
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
        closeCamera();
        openCamera();
    }

    @Override
    public void surfaceDestroyed(SurfaceHolder holder) {
        closeCamera();
    }
}
