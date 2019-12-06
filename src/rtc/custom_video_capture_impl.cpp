/*
 *  Copyright (c) 2012 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#include "custom_video_capture_impl.h"

#include <stdlib.h>
#include <string.h>

#include "api/video/i420_buffer.h"
#include "api/video/video_frame_buffer.h"
#include "common_video/libyuv/include/webrtc_libyuv.h"
#include "modules/video_capture/video_capture_config.h"
#include "rtc_base/logging.h"
#include "rtc_base/ref_counted_object.h"
#include "rtc_base/time_utils.h"
#include "rtc_base/trace_event.h"
#include "third_party/libyuv/include/libyuv.h"

namespace webrtc {

const char* CustomVideoCaptureImpl::CurrentDeviceName() const {
  return _deviceUniqueId;
}

// static
int32_t CustomVideoCaptureImpl::RotationFromDegrees(int degrees,
                                              VideoRotation* rotation) {
  switch (degrees) {
    case 0:
      *rotation = kVideoRotation_0;
      return 0;
    case 90:
      *rotation = kVideoRotation_90;
      return 0;
    case 180:
      *rotation = kVideoRotation_180;
      return 0;
    case 270:
      *rotation = kVideoRotation_270;
      return 0;
    default:
      return -1;
      ;
  }
}

// static
int32_t CustomVideoCaptureImpl::RotationInDegrees(VideoRotation rotation,
                                            int* degrees) {
  switch (rotation) {
    case kVideoRotation_0:
      *degrees = 0;
      return 0;
    case kVideoRotation_90:
      *degrees = 90;
      return 0;
    case kVideoRotation_180:
      *degrees = 180;
      return 0;
    case kVideoRotation_270:
      *degrees = 270;
      return 0;
  }
  return -1;
}

CustomVideoCaptureImpl::CustomVideoCaptureImpl()
    : _deviceUniqueId(NULL),
      _requestedCapability(),
      _lastProcessTimeNanos(rtc::TimeNanos()),
      _lastFrameRateCallbackTimeNanos(rtc::TimeNanos()),
      _dataCallBack(NULL),
      _lastProcessFrameTimeNanos(rtc::TimeNanos()),
      _rotateFrame(kVideoRotation_0),
      apply_rotation_(false) {
  _requestedCapability.width = videocapturemodule::kDefaultWidth;
  _requestedCapability.height = videocapturemodule::kDefaultHeight;
  _requestedCapability.maxFPS = 30;
  _requestedCapability.videoType = VideoType::kI420;
  memset(_incomingFrameTimesNanos, 0, sizeof(_incomingFrameTimesNanos));
}

CustomVideoCaptureImpl::~CustomVideoCaptureImpl() {
  DeRegisterCaptureDataCallback();
  if (_deviceUniqueId)
    delete[] _deviceUniqueId;
}

void CustomVideoCaptureImpl::RegisterCaptureDataCallback(
    rtc::VideoSinkInterface<VideoFrame>* dataCallBack) {
  rtc::CritScope cs(&_apiCs);
  _dataCallBack = dataCallBack;
}

void CustomVideoCaptureImpl::DeRegisterCaptureDataCallback() {
  rtc::CritScope cs(&_apiCs);
  _dataCallBack = NULL;
}
int32_t CustomVideoCaptureImpl::DeliverCapturedFrame(VideoFrame& captureFrame) {
  UpdateFrameCount();  // frame count used for local frame rate callback.

  if (_dataCallBack) {
    _dataCallBack->OnFrame(captureFrame);
  }

  return 0;
}

VideoFrame CustomVideoCaptureImpl::IncomingFrame(uint8_t* videoFrame,
                                        size_t videoFrameLength,
                                        const VideoCaptureCapability& frameInfo,
                                        int64_t captureTime /*=0*/) {
  rtc::CritScope cs(&_apiCs);

  const int32_t width = frameInfo.width;
  const int32_t height = frameInfo.height;

  TRACE_EVENT1("webrtc", "VC::IncomingFrame", "capture_time", captureTime);

  // Not encoded, convert to I420.
  if (frameInfo.videoType != VideoType::kMJPEG &&
      CalcBufferSize(frameInfo.videoType, width, abs(height)) !=
          videoFrameLength) {
    RTC_LOG(LS_ERROR) << "Wrong incoming frame length.";
  }

  int stride_y = width;
  int stride_uv = (width + 1) / 2;
  int target_width = width;
  int target_height = abs(height);

  // SetApplyRotation doesn't take any lock. Make a local copy here.
  bool apply_rotation = apply_rotation_;

  if (apply_rotation) {
    // Rotating resolution when for 90/270 degree rotations.
    if (_rotateFrame == kVideoRotation_90 ||
        _rotateFrame == kVideoRotation_270) {
      target_width = abs(height);
      target_height = width;
    }
  }

  // Setting absolute height (in case it was negative).
  // In Windows, the image starts bottom left, instead of top left.
  // Setting a negative source height, inverts the image (within LibYuv).

  // TODO(nisse): Use a pool?
  rtc::scoped_refptr<I420Buffer> buffer = I420Buffer::Create(
      target_width, target_height, stride_y, stride_uv, stride_uv);

  libyuv::RotationMode rotation_mode = libyuv::kRotate0;
  if (apply_rotation) {
    switch (_rotateFrame) {
      case kVideoRotation_0:
        rotation_mode = libyuv::kRotate0;
        break;
      case kVideoRotation_90:
        rotation_mode = libyuv::kRotate90;
        break;
      case kVideoRotation_180:
        rotation_mode = libyuv::kRotate180;
        break;
      case kVideoRotation_270:
        rotation_mode = libyuv::kRotate270;
        break;
    }
  }

  const int conversionResult = libyuv::ConvertToI420(
      videoFrame, videoFrameLength, buffer.get()->MutableDataY(),
      buffer.get()->StrideY(), buffer.get()->MutableDataU(),
      buffer.get()->StrideU(), buffer.get()->MutableDataV(),
      buffer.get()->StrideV(), 0, 0,  // No Cropping
      width, height, target_width, target_height, rotation_mode,
      ConvertVideoType(frameInfo.videoType));
  if (conversionResult < 0) {
    RTC_LOG(LS_ERROR) << "Failed to convert capture frame from type "
                      << static_cast<int>(frameInfo.videoType) << "to I420.";
  }

  VideoFrame captureFrame =
      VideoFrame::Builder()
          .set_video_frame_buffer(buffer)
          .set_timestamp_rtp(0)
          .set_timestamp_ms(rtc::TimeMillis())
          .set_rotation(!apply_rotation ? _rotateFrame : kVideoRotation_0)
          .build();
  captureFrame.set_ntp_time_ms(captureTime);

  return captureFrame;
}

int32_t CustomVideoCaptureImpl::StartCapture(
    const VideoCaptureCapability& capability) {
  _requestedCapability = capability;
  return -1;
}

int32_t CustomVideoCaptureImpl::StopCapture() {
  return -1;
}

bool CustomVideoCaptureImpl::CaptureStarted() {
  return false;
}

int32_t CustomVideoCaptureImpl::CaptureSettings(
    VideoCaptureCapability& /*settings*/) {
  return -1;
}

int32_t CustomVideoCaptureImpl::SetCaptureRotation(VideoRotation rotation) {
  rtc::CritScope cs(&_apiCs);
  _rotateFrame = rotation;
  return 0;
}

bool CustomVideoCaptureImpl::SetApplyRotation(bool enable) {
  // We can't take any lock here as it'll cause deadlock with IncomingFrame.

  // The effect of this is the last caller wins.
  apply_rotation_ = enable;
  return true;
}

bool CustomVideoCaptureImpl::GetApplyRotation() {
  return apply_rotation_;
}

void CustomVideoCaptureImpl::UpdateFrameCount() {
  if (_incomingFrameTimesNanos[0] / rtc::kNumNanosecsPerMicrosec == 0) {
    // first no shift
  } else {
    // shift
    for (int i = (videocapturemodule::kFrameRateCountHistorySize - 2); i >= 0; --i) {
      _incomingFrameTimesNanos[i + 1] = _incomingFrameTimesNanos[i];
    }
  }
  _incomingFrameTimesNanos[0] = rtc::TimeNanos();
}

uint32_t CustomVideoCaptureImpl::CalculateFrameRate(int64_t now_ns) {
  int32_t num = 0;
  int32_t nrOfFrames = 0;
  for (num = 1; num < (videocapturemodule::kFrameRateCountHistorySize - 1); ++num) {
    if (_incomingFrameTimesNanos[num] <= 0 ||
        (now_ns - _incomingFrameTimesNanos[num]) /
                rtc::kNumNanosecsPerMillisec >
            videocapturemodule::kFrameRateHistoryWindowMs) {  // don't use data older than 2sec
      break;
    } else {
      nrOfFrames++;
    }
  }
  if (num > 1) {
    int64_t diff = (now_ns - _incomingFrameTimesNanos[num - 1]) /
                   rtc::kNumNanosecsPerMillisec;
    if (diff > 0) {
      return uint32_t((nrOfFrames * 1000.0f / diff) + 0.5f);
    }
  }

  return nrOfFrames;
}
}  // namespace webrtc
