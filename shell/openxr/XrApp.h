/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// @fb-only

#pragma once

#include <igl/Macros.h>

#include <array>
#include <string>
#include <unordered_set>
#include <vector>

#include <shell/openxr/XrPlatform.h>
#include <glm/glm.hpp>

#include <igl/IGL.h>
#include <shell/shared/platform/Platform.h>
#include <shell/shared/renderSession/RenderSession.h>

#ifndef ENABLE_META_OPENXR_FEATURES
#define ENABLE_META_OPENXR_FEATURES 1
#endif

#if ENABLE_META_OPENXR_FEATURES
#include <extx1_event_channel.h>
#include <fb_face_tracking2.h>
#include <fb_scene.h>

#include <meta_automatic_layer_filter.h>

#include <meta_body_tracking_calibration.h>
#include <meta_body_tracking_fidelity.h>
#include <meta_body_tracking_full_body.h>

#include <meta_detached_controllers.h>
#include <meta_environment_depth.h>

#include <meta_hand_tracking_wide_motion_mode.h>
#include <meta_recommended_layer_resolution.h>
#include <meta_simultaneous_hands_and_controllers.h>

#include <meta_spatial_entity_mesh.h>
#include <metax1_hand_tracking_microgestures.h>

#include <openxr_extension_helpers.h>
#endif

const int LEFT = 0;
const int RIGHT = 1;
const int NUM_SIDES = 2;

struct android_app;
struct AAssetManager;

namespace igl::shell{
    class OKCloudSession;
}

// forward declarations
namespace igl::shell::openxr {
class XrHands;
class XrSwapchainProvider;
class XrPassthrough;
namespace impl {
class XrAppImpl;
}
} // namespace igl::shell::openxr

namespace igl::shell::openxr {

typedef enum {
    UNKNOWN_,

    META_QUEST_1_,
    META_QUEST_2_,
    META_QUEST_3_,
    META_QUEST_PRO_,

    PICO_NEO_3_,
    PICO_NEO_3_EYE_,
    PICO_NEO_4_,
    PICO_NEO_4_EYE_,

    HTC_FOCUS_3_,
    HTC_VIVE_XR_ELITE_,

    HEADSET_TYPE_COUNT
} HeadsetType;


static HeadsetType compute_headset_type(const std::string& system_name, const uint64_t systemID, const uint vendorID)
{
    const bool is_meta_headset = (vendorID == 10291);
    const bool is_pico_headset = (vendorID == 42);
    const bool is_htc_headset = (vendorID == 2996);

    if (is_meta_headset) {
        if (system_name == "Oculus Quest") {
            return HeadsetType::META_QUEST_1_;
        } else if (system_name == "Oculus Quest2") {
            return HeadsetType::META_QUEST_2_;
        } else if (system_name == "Meta Quest 3") {
            return HeadsetType::META_QUEST_3_;
        } else if (system_name == "Meta Quest Pro") {
            return HeadsetType::META_QUEST_PRO_;
        }
    } else if (is_pico_headset) {
        if (system_name == "Pico Neo 3" || (system_name == "PICO HMD")) {
            return HeadsetType::PICO_NEO_3_;
        } else if (system_name == "Pico Neo 3 Pro Eye") {
            return HeadsetType::PICO_NEO_3_EYE_;
        } else if (system_name == "PICO 4") {
            return HeadsetType::PICO_NEO_4_;
        } else if (system_name == "PICO 4 Pro") {
            return HeadsetType::PICO_NEO_4_EYE_;
        }
    } else if (is_htc_headset) {
        if (system_name == "WAVE:EYA") {
            return HeadsetType::HTC_VIVE_XR_ELITE_;
        } else if (system_name == "WAVE:SUE") {
            return HeadsetType::HTC_FOCUS_3_;
        }
    }

    return HeadsetType::UNKNOWN_;
}

struct XrInputState
{
    std::array<float, NUM_SIDES> handScale = {{1.0f, 1.0f}};
    std::array<XrBool32, NUM_SIDES> handActive;

    std::array<XrPath, NUM_SIDES> handSubactionPath;

    std::array<XrSpace, NUM_SIDES> gripSpace;
    std::array<XrSpace, NUM_SIDES> aimSpace;

    XrActionSet actionSet{XR_NULL_HANDLE};
    XrAction grabAction{XR_NULL_HANDLE};
    XrAction vibrateAction{XR_NULL_HANDLE};

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
    XrAction thumbRestClickAction{ XR_NULL_HANDLE };
    XrAction thumbRestForceAction{ XR_NULL_HANDLE };
    XrAction thumbProximityAction{ XR_NULL_HANDLE };

    XrAction pinchValueAction{ XR_NULL_HANDLE };
    XrAction pinchForceAction{ XR_NULL_HANDLE };

    XrAction buttonAXClickAction{ XR_NULL_HANDLE };
    XrAction buttonAXTouchAction{ XR_NULL_HANDLE };

    XrAction buttonBYClickAction{ XR_NULL_HANDLE };
    XrAction buttonBYTouchAction{ XR_NULL_HANDLE };

    XrAction trackpadXAction{ XR_NULL_HANDLE };
    XrAction trackpadYAction{ XR_NULL_HANDLE };
};

class XrApp {

  friend class igl::shell::OKCloudSession;
  XrTime get_predicted_display_time_ns();
  //PFN_xrConvertTimespecTimeToTimeKHR xrConvertTimespecTimeToTimeKHR_ = nullptr;

 public:
  XrApp(std::unique_ptr<impl::XrAppImpl>&& impl, bool shouldPresent = true);
  ~XrApp();

  inline bool initialized() const {
    return initialized_;
  }

  struct InitParams {
    enum RefreshRateMode {
      UseDefault,
      UseMaxRefreshRate,
      UseSpecificRefreshRate,
    };
    RefreshRateMode refreshRateMode_ = RefreshRateMode::UseDefault;
    float desiredSpecificRefreshRate_ = 90.0f;
  };
  bool initialize(const struct android_app* app, const InitParams& params);

  XrInstance instance() const;

  void handleXrEvents();
  void handleActionView(const std::string& data);

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
  bool enumerateViewConfigurations();
  void enumerateReferenceSpaces();
  void enumerateBlendModes();
  void updateSwapchainProviders();
  void handleSessionStateChanges(XrSessionState state);
  void createShellSession(std::unique_ptr<igl::IDevice> device, AAssetManager* assetMgr);

  void createSpaces();
  void createActions();

  XrFrameState beginFrame();
  void render();
  void endFrame(XrFrameState frameState);

  float getCurrentRefreshRate();
  float getMaxRefreshRate();
  bool setRefreshRate(float refreshRate);
  void setMaxRefreshRate();
  bool isRefreshRateSupported(float refreshRate);
  const std::vector<float>& getSupportedRefreshRates();
  
  bool isSharpeningEnabled() const;
  void setSharpeningEnabled(const bool enabled);

  bool isBodyTrackingFBSupported() const
  {
    return bodyTrackingFBSupported_;
  }

  bool isMetaFullBodyTrackingSupported() const
  {
    return metaFullBodyTrackingSupported_;
  }

  bool isMetaBodyTrackingFidelitySupported() const
  {
    return metaBodyTrackingFidelitySupported_;
  }

  bool isEyeTrackingSocialFBSupported() const
  {
    return eyeTrackingSocialFBSupported_;
  }

  bool areSimultaneousHandsAndControllersSupported() const
  {
    return simultaneousHandsAndControllersSupported_;
  }

  bool areSimultaneousHandsAndControllersEnabled() const
  {
    return simultaneousHandsAndControllersEnabled_;
  }

  bool areHTCViveFocus3ControllersSupported() const
  {
    return htcViveFocus3ControllersSupported_;
  }

  bool areByteDanceControllersSupported() const
  {
    return byteDanceControllersSupported_;
  }

  HeadsetType getHeadsetType() const
  {
      return headsetType_;
  }

private:
  HeadsetType headsetType_ = HeadsetType::UNKNOWN_;

  static constexpr uint32_t kNumViews = 2; // 2 for stereo

  void queryCurrentRefreshRate();
  void querySupportedRefreshRates();
  void setupProjectionAndDepth(std::vector<XrCompositionLayerProjectionView>& projectionViews,
                               std::vector<XrCompositionLayerDepthInfoKHR>& depthInfos);
  void endFrameProjectionComposition(XrFrameState frameState);
  void endFrameQuadLayerComposition(XrFrameState frameState);

  [[nodiscard]] inline bool passthroughSupported() const noexcept;
  [[nodiscard]] inline bool passthroughEnabled() const noexcept;

  [[nodiscard]] inline bool handsTrackingSupported() const noexcept;
  [[nodiscard]] inline bool handsTrackingMeshSupported() const noexcept;
  [[nodiscard]] inline bool refreshRateExtensionSupported() const noexcept;
  [[nodiscard]] inline bool instanceCreateInfoAndroidSupported() const noexcept;
  [[nodiscard]] inline bool alphaBlendCompositionSupported() const noexcept;

  void* nativeWindow_ = nullptr;
  bool resumed_ = false;
  bool sessionActive_ = false;


  std::vector<XrExtensionProperties> extensions_;
  std::vector<const char*> enabledExtensions_;

  XrInstanceProperties instanceProps_ = {
      .type = XR_TYPE_INSTANCE_PROPERTIES,
      .next = nullptr,
  };

  XrSystemProperties systemProps_ = {
      .type = XR_TYPE_SYSTEM_PROPERTIES,
      .next = nullptr,
  };

#if IGL_PLATFORM_ANDROID
  XrInstanceCreateInfoAndroidKHR instanceCreateInfoAndroid_ = {
      .type = XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR,
  };
#endif // IGL_PLATFORM_ANDROID

  std::unordered_set<std::string> supportedOptionalXrExtensions_;

  XrInstance instance_ = XR_NULL_HANDLE;
  XrSystemId systemId_ = XR_NULL_SYSTEM_ID;
  XrSession session_ = XR_NULL_HANDLE;

  XrViewConfigurationProperties viewConfigProps_ = {.type = XR_TYPE_VIEW_CONFIGURATION_PROPERTIES};
  std::array<XrViewConfigurationView, kNumViews> viewports_;
  std::array<XrView, kNumViews> views_;
  std::array<XrPosef, kNumViews> viewStagePoses_;
  std::array<glm::mat4, kNumViews> viewTransforms_;
  std::array<glm::vec3, kNumViews> cameraPositions_;

  XrPosef headPose_;
  XrTime headPoseTime_;
  XrInputState xr_inputs_;

#if ENABLE_CLOUDXR
  bool should_override_eye_poses_ = false;
  XrPosef override_eye_poses_[NUM_SIDES] = {};
#endif

  bool useSinglePassStereo_ = false;
  bool useQuadLayerComposition_ = false;
  uint32_t numQuadLayersPerView_ = 1;
  igl::shell::QuadLayerParams quadLayersParams_;

  // If useSinglePassStereo_ is true, only one XrSwapchainProvider will be created.
  std::vector<std::unique_ptr<XrSwapchainProvider>> swapchainProviders_;

  XrSpace headSpace_ = XR_NULL_HANDLE;
  XrSpace currentSpace_ = XR_NULL_HANDLE;
  bool stageSpaceSupported_ = false;

  bool additiveBlendingSupported_ = false;

  std::unique_ptr<XrPassthrough> passthrough_;
  std::unique_ptr<XrHands> hands_;

  std::vector<float> supportedRefreshRates_;
  float currentRefreshRate_ = 0.0f;

  PFN_xrGetDisplayRefreshRateFB xrGetDisplayRefreshRateFB_ = nullptr;
  PFN_xrEnumerateDisplayRefreshRatesFB xrEnumerateDisplayRefreshRatesFB_ = nullptr;
  PFN_xrRequestDisplayRefreshRateFB xrRequestDisplayRefreshRateFB_ = nullptr;

  bool compositionLayerSettingsSupported_ = false;
  XrCompositionLayerSettingsFB compositionLayerSettings_ = 
  { XR_TYPE_COMPOSITION_LAYER_SETTINGS_FB, nullptr, 0 };

  bool simpleControllersSupported_ = false;
  bool touchControllersSupported_ = true;
  bool touchProControllersSupported_ = false;
  bool touchControllerProximitySupported_ = false;

  bool bodyTrackingFBSupported_ = false;
  bool metaFullBodyTrackingSupported_ = false;
  bool metaBodyTrackingFidelitySupported_ = false;

  bool simultaneousHandsAndControllersSupported_ = false;
  bool simultaneousHandsAndControllersEnabled_ = false;
  PFN_xrResumeSimultaneousHandsAndControllersTrackingMETA xrResumeSimultaneousHandsAndControllersTrackingMETA_ = nullptr;
  PFN_xrPauseSimultaneousHandsAndControllersTrackingMETA xrPauseSimultaneousHandsAndControllersTrackingMETA_ = nullptr;
  bool setSimultaneousHandsAndControllersEnabled(const bool enabled);

  bool eyeTrackingSocialFBSupported_ = false;

  bool htcViveFocus3ControllersSupported_ = false;
  bool byteDanceControllersSupported_ = false;

  std::unique_ptr<impl::XrAppImpl> impl_;

  bool initialized_ = false;

  std::shared_ptr<igl::shell::Platform> platform_;
  std::unique_ptr<igl::shell::RenderSession> renderSession_;

  std::unique_ptr<igl::shell::ShellParams> shellParams_;
};
} // namespace igl::shell::openxr
