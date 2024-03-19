/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <igl/Macros.h>

#include <array>
#include <string>
#include <vector>

#if defined(IGL_CMAKE_BUILD)

#if IGL_BACKEND_VULKAN
#include <igl/vulkan/Common.h>
#endif // IGL_BACKEND_VULKAN

#if IGL_BACKEND_OPENGL
#include <igl/opengl/GLIncludes.h>
#endif // IGL_BACKEND_OPENGL

#if IGL_PLATFORM_ANDROID
#include <jni.h>

#ifndef XR_USE_TIMESPEC
#define XR_USE_TIMESPEC
#endif

#if IGL_BACKEND_OPENGL
#include <EGL/egl.h>
#endif // IGL_BACKEND_OPENGL
#endif // IGL_PLATFORM_ANDROID

#include <openxr/openxr_platform.h>

#endif // IGL_CMAKE_BUILD

#include <openxr/openxr.h>

#include <glm/glm.hpp>

#include <igl/IGL.h>
#include <shell/shared/platform/Platform.h>
#include <shell/shared/renderSession/RenderSession.h>

const int LEFT = 0;
const int RIGHT = 1;
const int NUM_SIDES = 2;

struct android_app;
class AAssetManager;

namespace igl::shell{
    class OKCloudSession;
}

// forward declarations
namespace igl::shell::openxr {
class XrSwapchainProvider;
namespace impl {
class XrAppImpl;
}
} // namespace igl::shell::openxr

namespace igl::shell::openxr {

struct XrInputState
{
    XrActionSet actionSet{XR_NULL_HANDLE};
    XrAction grabAction{XR_NULL_HANDLE};
    XrAction vibrateAction{XR_NULL_HANDLE};
    XrAction quitAction{XR_NULL_HANDLE};

    std::array<float, NUM_SIDES> handScale = {{1.0f, 1.0f}};
    std::array<XrBool32, NUM_SIDES> handActive;

    std::array<XrPath, NUM_SIDES> handSubactionPath;

    std::array<XrSpace, NUM_SIDES> gripSpace;
    std::array<XrSpace, NUM_SIDES> aimSpace;

    XrAction gripPoseAction{ XR_NULL_HANDLE };
    XrAction aimPoseAction{ XR_NULL_HANDLE };
    XrAction menuClickAction{ XR_NULL_HANDLE };

    XrAction triggerClickAction{ XR_NULL_HANDLE };
    XrAction triggerTouchAction{ XR_NULL_HANDLE };
    XrAction triggerValueAction{ XR_NULL_HANDLE };

    XrAction squeezeClickAction{ XR_NULL_HANDLE };
    XrAction squeezeTouchAction{ XR_NULL_HANDLE };
    XrAction squeezeValueAction{ XR_NULL_HANDLE };
    // XrAction squeezeForceAction{ XR_NULL_HANDLE };

    XrAction thumbstickTouchAction{ XR_NULL_HANDLE };
    XrAction thumbstickClickAction{ XR_NULL_HANDLE };

    XrAction thumbstickXAction{ XR_NULL_HANDLE };
    XrAction thumbstickYAction{ XR_NULL_HANDLE };

    XrAction thumbRestTouchAction{ XR_NULL_HANDLE };

    XrAction buttonAXClickAction{ XR_NULL_HANDLE };
    XrAction buttonAXTouchAction{ XR_NULL_HANDLE };

    XrAction buttonBYClickAction{ XR_NULL_HANDLE };
    XrAction buttonBYTouchAction{ XR_NULL_HANDLE };

    std::array<XrPath, NUM_SIDES> handVibratePath;
    XrAction handVibrateAction;
};

class XrApp {

  friend class igl::shell::OKCloudSession;
  XrTime get_predicted_display_time();
  PFN_xrConvertTimespecTimeToTimeKHR xrConvertTimespecTimeToTimeKHR = nullptr;

public:
  XrApp(std::unique_ptr<impl::XrAppImpl>&& impl);
  ~XrApp();

  inline bool initialized() const {
    return initialized_;
  }
  bool initialize(const struct android_app* app);

  XrInstance instance() const;

  void handleXrEvents();

  void update();
  void pollActions(const bool mainThread);
  bool enableMainThreadPolling_ = true;
  bool enableAsyncPolling_ = false;

  void setNativeWindow(void* win) {
    nativeWindow_ = win;
  }
  void* nativeWindow() const {
    return nativeWindow_;
  }

  void setResumed(bool resumed) {
    resumed_ = resumed;
  }
  bool resumed() const {
    return resumed_;
  }

  bool sessionActive() const {
    return sessionActive_;
  }
  XrSession session() const;

 private:
  bool checkExtensions();
  bool createInstance();
  bool createSystem();
  bool createPassthrough();
  bool createHandsTracking();
  void updateHandMeshes();
  void updateHandTracking();
  bool enumerateViewConfigurations();
  void enumerateReferenceSpaces();
  void enumerateBlendModes();
  void createSwapchainProviders(const std::unique_ptr<igl::IDevice>& device);
  void handleSessionStateChanges(XrSessionState state);
  void createShellSession(std::unique_ptr<igl::IDevice> device, AAssetManager* assetMgr);

  void createSpaces();
  void createActions();

  XrFrameState beginFrame();
  void render();
  void endFrame(XrFrameState frameState);

  float getCurrentRefreshRate();
  float getMaxRefreshRate();
  bool setRefreshRate(const float refreshRate);
  void setMaxRefreshRate();
  bool isRefreshRateSupported(const float refreshRate);
  const std::vector<float>& getSupportedRefreshRates();

  bool isSharpeningEnabled() const;
  void setSharpeningEnabled(const bool enabled);

 private:
  void* nativeWindow_ = nullptr;
  bool resumed_ = false;
  bool sessionActive_ = false;

  std::vector<XrExtensionProperties> extensions_;
  std::vector<const char*> requiredExtensions_;

  XrInstanceProperties instanceProps_ = {
      .type = XR_TYPE_INSTANCE_PROPERTIES,
      .next = nullptr,
  };

  XrSystemProperties systemProps_ = {
      .type = XR_TYPE_SYSTEM_PROPERTIES,
      .next = nullptr,
  };

  XrInstance instance_ = XR_NULL_HANDLE;
  XrSystemId systemId_ = XR_NULL_SYSTEM_ID;
  XrSession session_ = XR_NULL_HANDLE;

  XrViewConfigurationProperties viewConfigProps_ = {.type = XR_TYPE_VIEW_CONFIGURATION_PROPERTIES};
  static constexpr uint32_t kNumViews = 2; // 2 for stereo
  std::array<XrViewConfigurationView, kNumViews> viewports_;
  std::array<XrView, kNumViews> views_;
  std::array<XrPosef, kNumViews> viewStagePoses_;
  std::array<glm::mat4, kNumViews> viewTransforms_;
  std::array<glm::vec3, kNumViews> cameraPositions_;

  XrPosef headPose_;
  XrTime headPoseTime_;
  XrInputState xr_inputs_;

#if 1//ENABLE_CLOUDXR
    bool cloudxr_connected_ = false;
    XrPosef override_eye_poses_[NUM_SIDES] = {};
#endif

  bool useSinglePassStereo_ = false;
  bool useQuadLayerComposition_ = false;

  // If useSinglePassStereo_ is true, only one XrSwapchainProvider will be created.
  std::vector<std::unique_ptr<XrSwapchainProvider>> swapchainProviders_;

  XrSpace headSpace_ = XR_NULL_HANDLE;
  XrSpace currentSpace_ = XR_NULL_HANDLE;
  bool stageSpaceSupported_ = false;

  bool additiveBlendingSupported_ = false;

  XrPassthroughFB passthrough_;
  XrPassthroughLayerFB passthrougLayer_;

  bool passthroughSupported_ = false;
  PFN_xrCreatePassthroughFB xrCreatePassthroughFB_ = nullptr;
  PFN_xrDestroyPassthroughFB xrDestroyPassthroughFB_ = nullptr;
  PFN_xrPassthroughStartFB xrPassthroughStartFB_ = nullptr;
  PFN_xrCreatePassthroughLayerFB xrCreatePassthroughLayerFB_ = nullptr;
  PFN_xrDestroyPassthroughLayerFB xrDestroyPassthroughLayerFB_ = nullptr;
  PFN_xrPassthroughLayerSetStyleFB xrPassthroughLayerSetStyleFB_ = nullptr;

  bool handsTrackingSupported_ = false;
  bool handsTrackingMeshSupported_ = false;
  PFN_xrCreateHandTrackerEXT xrCreateHandTrackerEXT_ = nullptr;
  PFN_xrDestroyHandTrackerEXT xrDestroyHandTrackerEXT_ = nullptr;
  PFN_xrLocateHandJointsEXT xrLocateHandJointsEXT_ = nullptr;
  PFN_xrGetHandMeshFB xrGetHandMeshFB_ = nullptr;

  XrHandTrackerEXT leftHandTracker_ = XR_NULL_HANDLE;
  XrHandTrackerEXT rightHandTracker_ = XR_NULL_HANDLE;

  bool refreshRateExtensionSupported_ = false;
  bool useMaxRefreshRate_ = false;
  float desiredSpecificRefreshRate_ = 90.0f;
  std::vector<float> supportedRefreshRates_;
  float currentRefreshRate_ = 0.0f;
  void querySupportedRefreshRates();
  PFN_xrGetDisplayRefreshRateFB xrGetDisplayRefreshRateFB_ = nullptr;
  PFN_xrEnumerateDisplayRefreshRatesFB xrEnumerateDisplayRefreshRatesFB_ = nullptr;
  PFN_xrRequestDisplayRefreshRateFB xrRequestDisplayRefreshRateFB_ = nullptr;

  bool compositionLayerSettingsSupported_ = false;
  XrCompositionLayerSettingsFB compositionLayerSettings_ = { XR_TYPE_COMPOSITION_LAYER_SETTINGS_FB, nullptr, 0 };
  bool enableSharpeningAtStartup_ = false;

  std::unique_ptr<impl::XrAppImpl> impl_;

  bool initialized_ = false;

  std::shared_ptr<igl::shell::Platform> platform_;
  std::unique_ptr<igl::shell::RenderSession> renderSession_;

  std::unique_ptr<igl::shell::ShellParams> shellParams_;
};
} // namespace igl::shell::openxr
