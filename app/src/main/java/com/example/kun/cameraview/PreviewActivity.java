package com.example.kun.cameraview;

import android.Manifest;
import android.content.pm.PackageManager;
import android.content.res.Configuration;
import android.graphics.SurfaceTexture;
import android.os.Bundle;
import android.support.annotation.NonNull;
import android.support.v4.app.ActivityCompat;
import android.util.Log;
import android.util.Size;
import android.view.Display;
import android.view.Gravity;
import android.view.Surface;
import android.view.TextureView;
import android.support.v7.app.AppCompatActivity;
import android.view.View;
import android.widget.FrameLayout;
import android.widget.SeekBar;

public class PreviewActivity extends AppCompatActivity
        implements TextureView.SurfaceTextureListener,
        ActivityCompat.OnRequestPermissionsResultCallback,
        SeekBar.OnSeekBarChangeListener {
    long ndkCamera_;
    private TextureView textureView_;
    Surface surface_ = null;
    private Size cameraPreviewSize_;
    SeekBar hueSb;
    SeekBar satSb;
    SeekBar brtSb;
    SeekBar crtSb;

    // Used to load the 'native-lib' library on application startup.
    static {
        System.loadLibrary("native-lib");
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        onWindowFocusChanged(true);
        setContentView(R.layout.activity_preview);
        if (Feature.isCamera2Device(this)) {
            RequestCamera();
        } else {
            Log.e("cameraView", "未找到camera2設備");
        }
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus) {
            getWindow().getDecorView().setSystemUiVisibility(
                    View.SYSTEM_UI_FLAG_LAYOUT_STABLE
                            | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION
                            | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN
                            | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
                            | View.SYSTEM_UI_FLAG_FULLSCREEN
                            | View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY);
        }
    }

    private static final int PERMISSION_REQUEST_CODE_CAMERA = 1;

    public void RequestCamera() {
        if (ActivityCompat.checkSelfPermission(this, Manifest.permission.CAMERA) !=
                PackageManager.PERMISSION_GRANTED) {
            ActivityCompat.requestPermissions(
                    this,
                    new String[]{Manifest.permission.CAMERA},
                    PERMISSION_REQUEST_CODE_CAMERA);
            return;
        }
        createTextureView();
        initUI();
    }

    private void initUI() {
        hueSb = (SeekBar)findViewById(R.id.hueSeekBar);
        satSb = (SeekBar)findViewById(R.id.saturationSeekBar);
        brtSb = (SeekBar)findViewById(R.id.brightnessSeekBar);
        crtSb = (SeekBar)findViewById(R.id.contrastSeekBar);

        hueSb.setOnSeekBarChangeListener(this);
        satSb.setOnSeekBarChangeListener(this);
        brtSb.setOnSeekBarChangeListener(this);
        crtSb.setOnSeekBarChangeListener(this);
    }

    @Override
    public void onProgressChanged(SeekBar seekBar, int progress, boolean isFromUser) {
        updatePreviewSetting(
                hueSb.getProgress() - 180,
                satSb.getProgress(),
                brtSb.getProgress() -127,
                crtSb.getProgress()
        );
    }

    @Override
    public void onStartTrackingTouch(SeekBar seekBar) {

    }

    @Override
    public void onStopTrackingTouch(SeekBar seekBar) {

    }

    @Override
    public void onRequestPermissionsResult(int requestCode,
                                           @NonNull String[] permissions,
                                           @NonNull int[] grantResults) {
        if (PERMISSION_REQUEST_CODE_CAMERA != requestCode) {
            super.onRequestPermissionsResult(requestCode,
                    permissions,
                    grantResults);
            return;
        }

        if (grantResults[0] == PackageManager.PERMISSION_GRANTED) {
            Thread initCamera = new Thread(new Runnable() {
                public void run() {
                    runOnUiThread(new Runnable() {
                        @Override
                        public void run() {
                            createTextureView();
                        }
                    });
                }
            });
            initCamera.start();
        }
    }

    private void createTextureView() {
        textureView_ = (TextureView) findViewById(R.id.preview);
        textureView_.setSurfaceTextureListener(this);
        if (textureView_.isAvailable()) {
            onSurfaceTextureAvailable(textureView_.getSurfaceTexture(),
                    textureView_.getWidth(), textureView_.getHeight());
        }
    }

    @Override
    public void onSurfaceTextureAvailable(SurfaceTexture surface, int width, int height) {
        createNativeCamera();

        updatePreviewSetting(0, 100, 0, 100);

        resizeTextureView(width, height);
        surface.setDefaultBufferSize(cameraPreviewSize_.getWidth(),
                cameraPreviewSize_.getHeight());
        surface_ = new Surface(surface);
        onPreviewSurfaceCreated(ndkCamera_, surface_);
    }

    private void createNativeCamera() {
        Display display = getWindowManager().getDefaultDisplay();
        int height = display.getMode().getPhysicalHeight();
        int width = display.getMode().getPhysicalWidth();

        ndkCamera_ = createCamera(width, height);
        cameraPreviewSize_ = getMinimumCompatiblePreviewSize(ndkCamera_);
    }

    private void resizeTextureView(int textureWidth, int textureHeight) {
        int rotation = getWindowManager().getDefaultDisplay().getRotation();
        int newWidth = textureWidth;
        int newHeight = textureWidth * cameraPreviewSize_.getWidth() / cameraPreviewSize_.getHeight();

        if (Surface.ROTATION_90 == rotation || Surface.ROTATION_270 == rotation) {
            newHeight = (textureWidth * cameraPreviewSize_.getHeight()) / cameraPreviewSize_.getWidth();
        }
        textureView_.setLayoutParams(
                new FrameLayout.LayoutParams(newWidth, newHeight, Gravity.CENTER));

        textureView_.setTransform(Feature.getTransform(this, newWidth, newHeight));
        resizePreview(rotation);
    }

    @Override
    public void onSurfaceTextureSizeChanged(SurfaceTexture surface, int width, int height) {

    }

    @Override
    public boolean onSurfaceTextureDestroyed(SurfaceTexture surface) {
        onPreviewSurfaceDestroyed(ndkCamera_, surface_);
        deleteCamera(ndkCamera_);
        ndkCamera_ = 0;
        surface_ = null;
        return true;
    }

    @Override
    public void onSurfaceTextureUpdated(SurfaceTexture surface) {
Log.i("View_DE", "frame update");
    }

    private native long createCamera(int width, int height);

    private native Size getMinimumCompatiblePreviewSize(long ndkCamera);

    private native void onPreviewSurfaceCreated(long ndkCamera, Surface surface);

    private native void onPreviewSurfaceDestroyed(long ndkCamera, Surface surface);

    private native void deleteCamera(long ndkCamera);

    private native void resizePreview(int transform);

    private native void updatePreviewSetting(int hue, int saturation, int brightness, int contract);
}
