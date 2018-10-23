//
// Created by kun on 2018/10/19.
//

#ifndef CAMERAVIEW_CAMERA_ENGINE_H
#define CAMERAVIEW_CAMERA_ENGINE_H
#include <jni.h>
#include <android/native_activity.h>
#include <android/native_window_jni.h>
#include <android/native_window.h>
#include <functional>
#include <thread>

#include "camera_manager.h"

/**
 * CameraAppEngine
 */
class CameraAppEngine {
public:
    explicit CameraAppEngine(JNIEnv* env, jobject instance, jint w, jint h);
    ~CameraAppEngine();

    // Manage NDKCamera Object
    void CreateCameraSession(jobject surface);
    void StartPreview(bool start);
    const ImageFormat& GetCompatibleCameraRes() const;
    int32_t GetCameraSensorOrientation(int32_t facing);
    jobject GetSurfaceObject();
    void resizePreview(jint transform);
    void updatePreviewSetting(jint hue, jint saturation, jint brightness, jint contract);

private:
    JNIEnv* env_;
    jobject javaInstance_;
    int32_t requestWidth_;
    int32_t requestHeight_;
    jobject surface_;
    NDKCamera* camera_;
    ImageFormat compatibleCameraRes_;
};
#endif //CAMERAVIEW_CAMERA_ENGINE_H
