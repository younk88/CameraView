//
// Created by kun on 2018/10/19.
//
#include <utility>
#include <queue>
#include <unistd.h>
#include <cinttypes>
#include <cstring>
#include <math.h>
#include <media/NdkImage.h>
#include <android/native_window.h>
#include "camera_manager.h"
#include "native_debug.h"
#include "camera_utils.h"

#ifndef MAX
#define MAX(a, b)           \
  ({                        \
    __typeof__(a) _a = (a); \
    __typeof__(b) _b = (b); \
    _a > _b ? _a : _b;      \
  })
#define MIN(a, b)           \
  ({                        \
    __typeof__(a) _a = (a); \
    __typeof__(b) _b = (b); \
    _a < _b ? _a : _b;      \
  })
#endif

static const int kMaxChannelValue = 262143;

static inline uint32_t YUV2RGB(int nY, int nU, int nV) {
    nY -= 16;
    nU -= 128;
    nV -= 128;
    if (nY < 0) nY = 0;

    // This is the floating point equivalent. We do the conversion in integer
    // because some Android devices do not have floating point in hardware.
    // nR = (int)(1.164 * nY + 1.596 * nV);
    // nG = (int)(1.164 * nY - 0.813 * nV - 0.391 * nU);
    // nB = (int)(1.164 * nY + 2.018 * nU);

    int nR = (int)(1192 * nY + 1634 * nV);
    int nG = (int)(1192 * nY - 833 * nV - 400 * nU);
    int nB = (int)(1192 * nY + 2066 * nU);

    nR = MIN(kMaxChannelValue, MAX(0, nR));
    nG = MIN(kMaxChannelValue, MAX(0, nG));
    nB = MIN(kMaxChannelValue, MAX(0, nB));

    nR = (nR >> 10) & 0xff;
    nG = (nG >> 10) & 0xff;
    nB = (nB >> 10) & 0xff;

    return 0xff000000 | (nR << 16) | (nG << 8) | nB;
}
const double pi=acos(-1.0);

/**
 * adjust preview image by settings
 * */
YUVVector adjustPreview(int nY, int nU, int nV, ImageSetting setting) {
    float contrast = (float)setting.contrast / 100;
    float saturation = (float)setting.saturation / 100;
    double hue = setting.hue * pi / 180;
    int y = ((nY - 128) * contrast) + setting.brightness + 128;
    int u = (int)((((nU - 128) * cos(hue) + (nV - 128) * sin(hue)) * contrast * saturation) + 128);
    int v = (int)((((nV - 128) * cos(hue) - (nU - 128) * sin(hue)) * contrast * saturation) + 128);
    return YUVVector {
        MIN(MAX(0, y), 255),
        MIN(MAX(0, u), 255),
        MIN(MAX(0, v), 255)
    };
}

void OnImageCallback(void *ctx, AImageReader *reader) {
    reinterpret_cast<NDKCamera *>(ctx)->ImageCallback(reader);
}

/**
 * Range of Camera Exposure Time:
 *     Camera's capability range have a very long range which may be disturbing
 *     on camera. For this sample purpose, clamp to a range showing visible
 *     video on preview: 100000ns ~ 250000000ns
 */
static const uint64_t kMinExposureTime = static_cast<uint64_t>(1000000);
static const uint64_t kMaxExposureTime = static_cast<uint64_t>(250000000);

NDKCamera::NDKCamera()
        : cameraMgr_(nullptr),
          activeCameraId_(""),
          outputContainer_(nullptr),
          captureSessionState_(CaptureSessionState::MAX_STATE),
          cameraFacing_(ACAMERA_LENS_FACING_BACK),
          cameraOrientation_(0),
          exposureTime_(static_cast<int64_t>(0)),
          previewReader(nullptr),
          previewWin(nullptr) {
    valid_ = false;
    requests_.resize(CAPTURE_REQUEST_COUNT);
    memset(requests_.data(), 0, requests_.size() * sizeof(requests_[0]));
    cameras_.clear();
    cameraMgr_ = ACameraManager_create();
    ASSERT(cameraMgr_, "Failed to create cameraManager");

    // Pick up a back-facing camera to preview
    EnumerateCamera();
    ASSERT(activeCameraId_.size(), "Unknown ActiveCameraIdx");

    // Create back facing camera device
    CALL_MGR(openCamera(cameraMgr_, activeCameraId_.c_str(), GetDeviceListener(),
                        &cameras_[activeCameraId_].device_));

    CALL_MGR(registerAvailabilityCallback(cameraMgr_, GetManagerListener()));

    // Initialize camera controls(exposure time and sensitivity), pick
    // up value of 2% * range + min as starting value (just a number, no magic)
    ACameraMetadata* metadataObj;
    CALL_MGR(getCameraCharacteristics(cameraMgr_, activeCameraId_.c_str(),
                                      &metadataObj));
    ACameraMetadata_const_entry val = {
            0,
    };
    camera_status_t status = ACameraMetadata_getConstEntry(
            metadataObj, ACAMERA_SENSOR_INFO_EXPOSURE_TIME_RANGE, &val);
    if (status == ACAMERA_OK) {
        exposureRange_.min_ = val.data.i64[0];
        if (exposureRange_.min_ < kMinExposureTime) {
            exposureRange_.min_ = kMinExposureTime;
        }
        exposureRange_.max_ = val.data.i64[1];
        if (exposureRange_.max_ > kMaxExposureTime) {
            exposureRange_.max_ = kMaxExposureTime;
        }
        exposureTime_ = exposureRange_.value(2);
    } else {
        LOGW("Unsupported ACAMERA_SENSOR_INFO_EXPOSURE_TIME_RANGE");
        exposureRange_.min_ = exposureRange_.max_ = 0l;
        exposureTime_ = 0l;
    }
    status = ACameraMetadata_getConstEntry(
            metadataObj, ACAMERA_SENSOR_INFO_SENSITIVITY_RANGE, &val);

    if (status == ACAMERA_OK){
        sensitivityRange_.min_ = val.data.i32[0];
        sensitivityRange_.max_ = val.data.i32[1];

        sensitivity_ = sensitivityRange_.value(2);
    } else {
        LOGW("failed for ACAMERA_SENSOR_INFO_SENSITIVITY_RANGE");
        sensitivityRange_.min_ = sensitivityRange_.max_ = 0;
        sensitivity_ = 0;
    }
    valid_ = true;
}

/**
 * A helper class to assist image size comparison, by comparing the absolute
 * size
 * regardless of the portrait or landscape mode.
 */
class DisplayDimension {
public:
    DisplayDimension(int32_t w, int32_t h) : w_(w), h_(h), portrait_(false) {
        if (h > w) {
            // make it landscape
            w_ = h;
            h_ = w;
            portrait_ = true;
        }
    }
    DisplayDimension(const DisplayDimension& other) {
        w_ = other.w_;
        h_ = other.h_;
        portrait_ = other.portrait_;
    }

    DisplayDimension(void) {
        w_ = 0;
        h_ = 0;
        portrait_ = false;
    }
    DisplayDimension& operator=(const DisplayDimension& other) {
        w_ = other.w_;
        h_ = other.h_;
        portrait_ = other.portrait_;

        return (*this);
    }

    bool IsSameRatio(DisplayDimension& other) {
        return (w_ * other.h_ == h_ * other.w_);
    }
    bool operator>(DisplayDimension& other) {
        return (w_ >= other.w_ & h_ >= other.h_);
    }
    bool operator==(DisplayDimension& other) {
        return (w_ == other.w_ && h_ == other.h_ && portrait_ == other.portrait_);
    }
    DisplayDimension operator-(DisplayDimension& other) {
        DisplayDimension delta(w_ - other.w_, h_ - other.h_);
        return delta;
    }
    void Flip(void) { portrait_ = !portrait_; }
    bool IsPortrait(void) { return portrait_; }
    int32_t width(void) { return w_; }
    int32_t height(void) { return h_; }
    int32_t org_width(void) { return (portrait_ ? h_ : w_); }
    int32_t org_height(void) { return (portrait_ ? w_ : h_); }

private:
    int32_t w_, h_;
    bool portrait_;
};

/**
 * Find a compatible camera modes:
 *    1) the same aspect ration as the native display window, which should be a
 *       rotated version of the physical device
 *    2) the smallest resolution in the camera mode list
 * This is to minimize the later color space conversion workload.
 */
bool NDKCamera::MatchCaptureSizeRequest(int32_t requestWidth,
                                        int32_t requestHeight,
                                        ImageFormat* view) {
    return MatchCaptureSizeRequest(requestWidth, requestHeight, view, nullptr);
}


bool NDKCamera::MatchCaptureSizeRequest(int32_t requestWidth,
                                        int32_t requestHeight,
                                        ImageFormat* resView,
                                        ImageFormat* resCap) {
    DisplayDimension disp(requestWidth, requestHeight);
    if (cameraOrientation_ == 90 || cameraOrientation_ == 270) {
        disp.Flip();
    }
    ACameraMetadata* metadata;
    CALL_MGR(
            getCameraCharacteristics(cameraMgr_, activeCameraId_.c_str(), &metadata));
    ACameraMetadata_const_entry entry;
    CALL_METADATA(getConstEntry(
            metadata, ACAMERA_SCALER_AVAILABLE_STREAM_CONFIGURATIONS, &entry));
    // format of the data: format, width, height, input?, type int32
    bool foundIt = false;
    DisplayDimension foundRes(4000, 4000);
    DisplayDimension maxJPG(0, 0);

    for (int i = 0; i < entry.count; i += 4) {
        int32_t input = entry.data.i32[i + 3];
        int32_t format = entry.data.i32[i + 0];
        if (input) continue;

        if (format == AIMAGE_FORMAT_YUV_420_888 || format == AIMAGE_FORMAT_JPEG) {
            DisplayDimension res(entry.data.i32[i + 1],
                                 entry.data.i32[i + 2]);
            if (!disp.IsSameRatio(res)) continue;
            if (format == AIMAGE_FORMAT_YUV_420_888 && foundRes > res) {
                foundIt = true;
                foundRes = res;
            } else if (format == AIMAGE_FORMAT_JPEG && res > maxJPG) {
                maxJPG = res;
            }
        }
    }

    if (foundIt) {
        resView->width = foundRes.org_width();
        resView->height = foundRes.org_height();
        if (resCap) {
            resCap->width = maxJPG.org_width();
            resCap->height = maxJPG.org_height();
        }
    } else {
        LOGW("Did not find any compatible camera resolution, taking 640x480");
        if (disp.IsPortrait()) {
            resView->width = 480;
            resView->height = 640;
        } else {
            resView->width = 640;
            resView->height = 480;
        }
        if (resCap)
            *resCap = *resView;
    }
    resView->format = AIMAGE_FORMAT_YUV_420_888;
    if (resCap) resCap->format = AIMAGE_FORMAT_JPEG;
    return foundIt;
}

void NDKCamera::CreateSession(ANativeWindow* previewWindow,
                              ANativeWindow* jpgWindow, bool manualPreview,
                              int32_t imageRotation, ImageFormat* res) {

    previewWin = previewWindow;
    AImageReader_new(ANativeWindow_getWidth(previewWindow), ANativeWindow_getHeight(previewWindow), res->format, 4, &previewReader);
    AImageReader_ImageListener listener{
        .context = this, .onImageAvailable = OnImageCallback
    };
    AImageReader_setImageListener(previewReader, &listener);

    ANativeWindow* tmpPreviewWin;
    AImageReader_getWindow(previewReader, &tmpPreviewWin);
    // Create output from this app's ANativeWindow, and add into output container
    requests_[PREVIEW_REQUEST_IDX].outputNativeWindow_ = tmpPreviewWin;
    requests_[PREVIEW_REQUEST_IDX].template_ = TEMPLATE_PREVIEW;
    requests_[JPG_CAPTURE_REQUEST_IDX].outputNativeWindow_ = jpgWindow;
    requests_[JPG_CAPTURE_REQUEST_IDX].template_ = TEMPLATE_STILL_CAPTURE;

    CALL_CONTAINER(create(&outputContainer_));
    for (auto& req : requests_) {
        if (!req.outputNativeWindow_) continue;

        ANativeWindow_acquire(req.outputNativeWindow_);
        CALL_OUTPUT(create(req.outputNativeWindow_, &req.sessionOutput_));
        CALL_CONTAINER(add(outputContainer_, req.sessionOutput_));
        CALL_TARGET(create(req.outputNativeWindow_, &req.target_));
        CALL_DEV(createCaptureRequest(cameras_[activeCameraId_].device_,
                                      req.template_, &req.request_));
        CALL_REQUEST(addTarget(req.request_, req.target_));
    }

    // Create a capture session for the given preview request
    captureSessionState_ = CaptureSessionState::READY;
    CALL_DEV(createCaptureSession(cameras_[activeCameraId_].device_,
                                  outputContainer_, GetSessionListener(),
                                  &captureSession_));

    if (jpgWindow) {
        ACaptureRequest_setEntry_i32(requests_[JPG_CAPTURE_REQUEST_IDX].request_,
                                     ACAMERA_JPEG_ORIENTATION, 1, &imageRotation);
    }

    if (!manualPreview) {
        return;
    }
    /*
     * Only preview request is in manual mode, JPG is always in Auto mode
     * JPG capture mode could also be switch into manual mode and control
     * the capture parameters, this sample leaves JPG capture to be auto mode
     * (auto control has better effect than author's manual control)
     */
    uint8_t aeModeOff = ACAMERA_CONTROL_AE_MODE_OFF;
    CALL_REQUEST(setEntry_u8(requests_[PREVIEW_REQUEST_IDX].request_,
                             ACAMERA_CONTROL_AE_MODE, 1, &aeModeOff));
    CALL_REQUEST(setEntry_i32(requests_[PREVIEW_REQUEST_IDX].request_,
                              ACAMERA_SENSOR_SENSITIVITY, 1, &sensitivity_));
    CALL_REQUEST(setEntry_i64(requests_[PREVIEW_REQUEST_IDX].request_,
                              ACAMERA_SENSOR_EXPOSURE_TIME, 1, &exposureTime_));
}

void NDKCamera::CreateSession(ANativeWindow* previewWindow, ImageFormat* res) {
    CreateSession(previewWindow, nullptr, false, 0, res);
}

void NDKCamera::ImageCallback(AImageReader *reader) {
    AImage *image;
    //media_status_t status = AImageReader_acquireNextImage(reader, &image);
    /**
     * 移动很快时只渲染最后一帧
     * */
    media_status_t status = AImageReader_acquireLatestImage(reader, &image);
    if (status != AMEDIA_OK || !image) {
        return;
    }

    ANativeWindow_acquire(previewWin);
    ANativeWindow_Buffer buf;
    if (ANativeWindow_lock(previewWin, &buf, nullptr) < 0) {
        AImage_delete(image);
        return;
    }

    PresentImage(&buf, image);
    AImage_delete(image);
    ANativeWindow_unlockAndPost(previewWin);
    ANativeWindow_release(previewWin);
}

void NDKCamera::PresentImage(ANativeWindow_Buffer *buf, AImage *image) {
    AImageCropRect srcRect;
    AImage_getCropRect(image, &srcRect);

    int32_t yStride, uvStride;
    uint8_t *yPixel, *uPixel, *vPixel;
    int32_t yLen, uLen, vLen;
    AImage_getPlaneRowStride(image, 0, &yStride);
    AImage_getPlaneRowStride(image, 1, &uvStride);
    AImage_getPlaneData(image, 0, &yPixel, &yLen);
    AImage_getPlaneData(image, 1, &vPixel, &vLen);
    AImage_getPlaneData(image, 2, &uPixel, &uLen);
    int32_t uvPixelStride;
    AImage_getPlanePixelStride(image, 1, &uvPixelStride);

    int32_t height = MIN(buf->height, (srcRect.bottom - srcRect.top));
    int32_t width = MIN(buf->width, (srcRect.right - srcRect.left));

    uint32_t *out = static_cast<uint32_t *>(buf->bits);
    for (int32_t y = 0; y < height; y++) {
        const uint8_t *pY = yPixel + yStride * (y + srcRect.top) + srcRect.left;

        int32_t uv_row_start = uvStride * ((y + srcRect.top) >> 1);
        const uint8_t *pU = uPixel + uv_row_start + (srcRect.left >> 1);
        const uint8_t *pV = vPixel + uv_row_start + (srcRect.left >> 1);

        for (int32_t x = 0; x < width; x++) {
            const int32_t uv_offset = (x >> 1) * uvPixelStride;
            YUVVector vec = adjustPreview(pY[x], pU[uv_offset], pV[uv_offset], setting_);
            out[x] = YUV2RGB(vec.Y, vec.U, vec.V);
        }
        out += buf->stride;
    }
    LOGI("brightness %d", setting_.brightness);
}

NDKCamera::~NDKCamera() {
    valid_ = false;
    // stop session if it is on:
    if (captureSessionState_ == CaptureSessionState::ACTIVE) {
        ACameraCaptureSession_stopRepeating(captureSession_);
    }
    ACameraCaptureSession_close(captureSession_);

    for (auto& req : requests_) {
        if (!req.outputNativeWindow_) continue;

        CALL_REQUEST(removeTarget(req.request_, req.target_));
        ACaptureRequest_free(req.request_);
        ACameraOutputTarget_free(req.target_);

        CALL_CONTAINER(remove(outputContainer_, req.sessionOutput_));
        ACaptureSessionOutput_free(req.sessionOutput_);

        ANativeWindow_release(req.outputNativeWindow_);
    }
    requests_.resize(0);
    ACaptureSessionOutputContainer_free(outputContainer_);

    for (auto& cam : cameras_) {
        if (cam.second.device_) {
            CALL_DEV(close(cam.second.device_));
        }
    }
    cameras_.clear();
    if (cameraMgr_) {
        CALL_MGR(unregisterAvailabilityCallback(cameraMgr_, GetManagerListener()));
        ACameraManager_delete(cameraMgr_);
        cameraMgr_ = nullptr;
    }
}

/**
 * EnumerateCamera()
 *     Loop through cameras on the system, pick up
 *     1) back facing one if available
 *     2) otherwise pick the first one reported to us
 */
void NDKCamera::EnumerateCamera() {
    ACameraIdList* cameraIds = nullptr;
    CALL_MGR(getCameraIdList(cameraMgr_, &cameraIds));

    for (int i = 0; i < cameraIds->numCameras; ++i) {
        const char* id = cameraIds->cameraIds[i];

        ACameraMetadata* metadataObj;
        CALL_MGR(getCameraCharacteristics(cameraMgr_, id, &metadataObj));

        int32_t count = 0;
        const uint32_t* tags = nullptr;
        ACameraMetadata_getAllTags(metadataObj, &count, &tags);
        for (int tagIdx = 0; tagIdx < count; ++tagIdx) {
            if (ACAMERA_LENS_FACING == tags[tagIdx]) {
                ACameraMetadata_const_entry lensInfo = {
                        0,
                };
                CALL_METADATA(getConstEntry(metadataObj, tags[tagIdx], &lensInfo));
                CameraId cam(id);
                cam.facing_ = static_cast<acamera_metadata_enum_android_lens_facing_t>(
                        lensInfo.data.u8[0]);
                cam.owner_ = false;
                cam.device_ = nullptr;
                cameras_[cam.id_] = cam;
                if (cam.facing_ == ACAMERA_LENS_FACING_BACK) {
                    activeCameraId_ = cam.id_;
                }
                break;
            }
        }
        ACameraMetadata_free(metadataObj);
    }

    ASSERT(cameras_.size(), "No Camera Available on the device");
    if (activeCameraId_.length() == 0) {
        // if no back facing camera found, pick up the first one to use...
        activeCameraId_ = cameras_.begin()->second.id_;
    }
    ACameraManager_deleteCameraIdList(cameraIds);
}

/**
 * GetSensorOrientation()
 *     Retrieve current sensor orientation regarding to the phone device
 * orientation
 *     SensorOrientation is NOT settable.
 */
bool NDKCamera::GetSensorOrientation(int32_t* facing, int32_t* angle) {
    if (!cameraMgr_) {
        return false;
    }

    ACameraMetadata* metadataObj;
    ACameraMetadata_const_entry face, orientation;
    CALL_MGR(getCameraCharacteristics(cameraMgr_, activeCameraId_.c_str(),
                                      &metadataObj));
    CALL_METADATA(getConstEntry(metadataObj, ACAMERA_LENS_FACING, &face));
    cameraFacing_ = static_cast<int32_t>(face.data.u8[0]);

    CALL_METADATA(
            getConstEntry(metadataObj, ACAMERA_SENSOR_ORIENTATION, &orientation));

    LOGI("====Current SENSOR_ORIENTATION: %8d", orientation.data.i32[0]);

    ACameraMetadata_free(metadataObj);
    cameraOrientation_ = orientation.data.i32[0];

    if (facing) *facing = cameraFacing_;
    if (angle) *angle = cameraOrientation_;
    return true;
}

/**
 * StartPreview()
 *   Toggle preview start/stop
 */
void NDKCamera::StartPreview(bool start) {
    if (start) {
        CALL_SESSION(setRepeatingRequest(captureSession_, nullptr, 1,
                                         &requests_[PREVIEW_REQUEST_IDX].request_,
                                         nullptr));
    } else if (!start && captureSessionState_ == CaptureSessionState::ACTIVE) {
        ACameraCaptureSession_stopRepeating(captureSession_);
    }
}

/**
 * Capture one jpg photo into
 *     /sdcard/DCIM/Camera
 * refer to WriteFile() for details
 */
bool NDKCamera::TakePhoto(void) {
    if (captureSessionState_ == CaptureSessionState::ACTIVE) {
        ACameraCaptureSession_stopRepeating(captureSession_);
    }

    CALL_SESSION(capture(captureSession_, GetCaptureCallback(), 1,
                         &requests_[JPG_CAPTURE_REQUEST_IDX].request_,
                         &requests_[JPG_CAPTURE_REQUEST_IDX].sessionSequenceId_));
    return true;
}

void NDKCamera::UpdateCameraRequestParameter(int32_t code, int64_t val) {
    ACaptureRequest* request = requests_[PREVIEW_REQUEST_IDX].request_;
    switch (code) {
        case ACAMERA_SENSOR_EXPOSURE_TIME:
            exposureTime_ = val;
            CALL_REQUEST(setEntry_i64(request, ACAMERA_SENSOR_EXPOSURE_TIME, 1,
                                      &exposureTime_));
            break;

        case ACAMERA_SENSOR_SENSITIVITY:
            sensitivity_ = val;
            CALL_REQUEST(
                    setEntry_i32(request, ACAMERA_SENSOR_SENSITIVITY, 1, &sensitivity_));
            break;
        default:
            ASSERT(false, "==ERROR==: error code for CameraParameterChange: %d",
                   code);
            return;
    }

    uint8_t aeModeOff = ACAMERA_CONTROL_AE_MODE_OFF;
    CALL_REQUEST(setEntry_u8(request, ACAMERA_CONTROL_AE_MODE, 1, &aeModeOff));
    CALL_SESSION(
            setRepeatingRequest(captureSession_, nullptr, 1, &request,
                                &requests_[PREVIEW_REQUEST_IDX].sessionSequenceId_));
}

/**
 * Retrieve Camera Exposure adjustable range.
 *
 * @param min Camera minimium exposure time in nanoseconds
 * @param max Camera maximum exposure tiem in nanoseconds
 *
 * @return true  min and max are loaded with the camera's exposure values
 *         false camera has not initialized, no value available
 */
bool NDKCamera::GetExposureRange(int64_t* min, int64_t* max, int64_t* curVal) {
    if (!exposureTime_ || !min || !max || !curVal) {
        return false;
    }
    *min = exposureRange_.min_;
    *max = exposureRange_.max_;
    *curVal = exposureTime_;

    return true;
}

/**
 * Retrieve Camera sensitivity range.
 *
 * @param min Camera minimium sensitivity
 * @param max Camera maximum sensitivity
 *
 * @return true  min and max are loaded with the camera's sensitivity values
 *         false camera has not initialized, no value available
 */
bool NDKCamera::GetSensitivityRange(int64_t* min, int64_t* max,
                                    int64_t* curVal) {
    if (!sensitivity_ || !min || !max || !curVal) {
        return false;
    }
    *min = static_cast<int64_t>(sensitivityRange_.min_);
    *max = static_cast<int64_t>(sensitivityRange_.max_);
    *curVal = sensitivity_;
    return true;
}

void NDKCamera::resizePreview(int32_t transform) {
    if (!previewReader) {
        return;
    }
    ANativeWindow* tmpPreviewWin;
    AImageReader_getWindow(previewReader, &tmpPreviewWin);

    ANativeWindow_setBuffersGeometry(
            tmpPreviewWin,
            ANativeWindow_getWidth(previewWin),
            ANativeWindow_getHeight(previewWin),
            ANativeWindow_getFormat(previewWin)
    );
    ANativeWindow_setBuffersTransform(tmpPreviewWin, transform);
}

void NDKCamera::updateSetting(int8_t hue, int8_t saturation, int8_t brightness, int8_t contract) {
    ImageSetting s = {
            hue: hue,
            saturation: saturation,
            brightness: brightness,
            contrast: contract
    };
    setting_ = s;
    LOGI("brt %d", brightness);
}