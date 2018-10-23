//
// Created by kun on 2018/10/19.
//
#include <cstdio>
#include <cstring>
#include "camera_engine.h"
#include "native_debug.h"

CameraAppEngine::CameraAppEngine(JNIEnv* env, jobject instance, jint w, jint h)
        : env_(env),
          javaInstance_(instance),
          requestWidth_(w),
          requestHeight_(h),
          surface_(nullptr),
          camera_(nullptr) {
    memset(&compatibleCameraRes_, 0, sizeof(compatibleCameraRes_));
    camera_ = new NDKCamera();
    ASSERT(camera_, "Failed to Create CameraObject");
    camera_->MatchCaptureSizeRequest(requestWidth_, requestHeight_,
                                     &compatibleCameraRes_);
}

CameraAppEngine::~CameraAppEngine() {
    if (camera_) {
        delete camera_;
        camera_ = nullptr;
    }

    if (surface_) {
        env_->DeleteGlobalRef(surface_);
        surface_ = nullptr;
    }
}

/**
 * Create a capture session with given Java Surface Object
 * @param surface a {@link Surface} object.
 */
void CameraAppEngine::CreateCameraSession(jobject surface) {
    surface_ = env_->NewGlobalRef(surface);
    ImageFormat res = GetCompatibleCameraRes();
    camera_->CreateSession(ANativeWindow_fromSurface(env_, surface), &res);
}

/**
 * @return cached {@link Surface} object
 */
jobject CameraAppEngine::GetSurfaceObject() { return surface_; }

/**
 *
 * @return saved camera preview resolution for this session
 */
const ImageFormat& CameraAppEngine::GetCompatibleCameraRes() const {
    return compatibleCameraRes_;
}

int CameraAppEngine::GetCameraSensorOrientation(int32_t requestFacing) {
    ASSERT(requestFacing == ACAMERA_LENS_FACING_BACK,
           "Only support rear facing camera");
    int32_t facing = 0, angle = 0;
    if (camera_->GetSensorOrientation(&facing, &angle) ||
        facing == requestFacing) {
        return angle;
    }
    ASSERT(false, "Failed for GetSensorOrientation()");
    return 0;
}

/**
 *
 * @param start is true to start preview, false to stop preview
 * @return  true if preview started, false when error happened
 */
void CameraAppEngine::StartPreview(bool start) { camera_->StartPreview(start); }

void CameraAppEngine::resizePreview(jint transform) {
    camera_->resizePreview(transform);
}

void CameraAppEngine::updatePreviewSetting(jint hue, jint saturation, jint brightness, jint contract) {
    camera_->updateSetting(hue, saturation, brightness, contract);
}