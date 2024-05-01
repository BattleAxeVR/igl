/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// @fb-only

#include <shell/openxr/XrApp.h>

#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <string>

#include <android/asset_manager.h>
#include <android/log.h>
#include <android_native_app_glue.h>

#include <openxr/openxr.h>

#include <igl/Common.h>
#if USE_VULKAN_BACKEND
#include <igl/vulkan/Common.h>

#ifndef XR_USE_GRAPHICS_API_VULKAN
#define XR_USE_GRAPHICS_API_VULKAN
#endif
#endif // USE_VULKAN_BACKEND
#if USE_OPENGL_BACKEND
#if !defined(XR_USE_GRAPHICS_API_OPENGL_ES) && defined(IGL_CMAKE_BUILD)
#define XR_USE_GRAPHICS_API_OPENGL_ES
#endif
#endif // USE_OPENGL_BACKEND
#include <openxr/openxr_platform.h>

#include <glm/gtc/type_ptr.hpp>

#ifndef EXTERNAL_XR_BUILD
#include <xr_linear.h>
#endif

#include <shell/shared/fileLoader/android/FileLoaderAndroid.h>
#include <shell/shared/imageLoader/android/ImageLoaderAndroid.h>
#include <shell/shared/platform/android/PlatformAndroid.h>
#include <shell/shared/renderSession/AppParams.h>
#include <shell/shared/renderSession/DefaultSession.h>
#include <shell/shared/renderSession/ShellParams.h>

#include <shell/openxr/XrLog.h>
#include <shell/openxr/XrSwapchainProvider.h>
#include <shell/openxr/impl/XrAppImpl.h>
#include <shell/openxr/impl/XrSwapchainProviderImpl.h>

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

#if ENABLE_CLOUDXR
#include "../src/cpp/ok_defines.h"
#include "../src/cpp/OKConfig.h"
BVR::OKConfig ok_config_s;
#endif

namespace igl::shell::openxr {

constexpr auto kAppName = "IGL Shell OpenXR";
constexpr auto kEngineName = "IGL";
constexpr auto kSupportedViewConfigType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;

namespace {
inline glm::quat glmQuatFromXrQuat(const XrQuaternionf& quat) noexcept {
  return glm::quat(quat.w, quat.x, quat.y, quat.z);
}

inline glm::vec4 glmVecFromXrVec(const XrVector4f& vec) noexcept {
  return glm::vec4(vec.x, vec.y, vec.z, vec.w);
}

inline glm::vec4 glmVecFromXrVec(const XrVector4sFB& vec) noexcept {
  return glm::vec4(vec.x, vec.y, vec.z, vec.w);
}

inline glm::vec3 glmVecFromXrVec(const XrVector3f& vec) noexcept {
  return glm::vec3(vec.x, vec.y, vec.z);
}

inline glm::vec2 glmVecFromXrVec(const XrVector2f& vec) noexcept {
  return glm::vec2(vec.x, vec.y);
}

inline Pose poseFromXrPose(const XrPosef& pose) noexcept {
  return Pose{
      /*.orientation = */ glmQuatFromXrQuat(pose.orientation),
      /*.position = */ glmVecFromXrVec(pose.position),
  };
}

inline int64_t currentTimeInNs() {
  const auto now = std::chrono::steady_clock::now();
  return std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count();
}
} // namespace

XrApp::XrApp(std::unique_ptr<impl::XrAppImpl>&& impl) :
  requiredExtensions_({
#if USE_VULKAN_BACKEND
      XR_KHR_VULKAN_ENABLE_EXTENSION_NAME,
#endif // USE_VULKAN_BACKEND
#if !defined(XR_USE_PLATFORM_MACOS) && !defined(IGL_CMAKE_BUILD)
      XR_FB_SWAPCHAIN_UPDATE_STATE_EXTENSION_NAME,
#endif
    //XR_KHR_CONVERT_TIMESPEC_TIME_EXTENSION_NAME,
  }),
  impl_(std::move(impl)),
  shellParams_(std::make_unique<ShellParams>()) {
  viewports_.fill({XR_TYPE_VIEW_CONFIGURATION_VIEW});
  views_.fill({XR_TYPE_VIEW});
#ifdef USE_COMPOSITION_LAYER_QUAD
  useQuadLayerComposition_ = true;
#endif
}

XrApp::~XrApp() {
  if (!initialized_)
    return;

  swapchainProviders_.clear();

  if (leftHandTracker_ != XR_NULL_HANDLE) {
    xrDestroyHandTrackerEXT_(leftHandTracker_);
  }
  if (rightHandTracker_ != XR_NULL_HANDLE) {
    xrDestroyHandTrackerEXT_(rightHandTracker_);
  }
  if (passthrough_ != XR_NULL_HANDLE) {
    xrDestroyPassthroughFB_(passthrough_);
  }
  xrDestroySpace(currentSpace_);
  xrDestroySpace(headSpace_);
  xrDestroySession(session_);
  xrDestroyInstance(instance_);
}

XrInstance XrApp::instance() const {
  return instance_;
}

XrSession XrApp::session() const {
  return session_;
}

bool XrApp::checkExtensions() {
  // Check that the extensions required are present.
  XrResult result;
  PFN_xrEnumerateInstanceExtensionProperties xrEnumerateInstanceExtensionProperties;
  XR_CHECK(result =
               xrGetInstanceProcAddr(XR_NULL_HANDLE,
                                     "xrEnumerateInstanceExtensionProperties",
                                     (PFN_xrVoidFunction*)&xrEnumerateInstanceExtensionProperties));
  if (result != XR_SUCCESS) {
    IGL_LOG_ERROR("Failed to get xrEnumerateInstanceExtensionProperties function pointer.");
    return false;
  }

  uint32_t numExtensions = 0;
  XR_CHECK(xrEnumerateInstanceExtensionProperties(nullptr, 0, &numExtensions, nullptr));
  IGL_LOG_INFO("xrEnumerateInstanceExtensionProperties found %u extension(s).", numExtensions);

  extensions_.resize(numExtensions, {XR_TYPE_EXTENSION_PROPERTIES});

  XR_CHECK(xrEnumerateInstanceExtensionProperties(
      NULL, numExtensions, &numExtensions, extensions_.data()));
  for (uint32_t i = 0; i < numExtensions; i++) {
    IGL_LOG_INFO("Extension #%d = '%s'.", i, extensions_[i].extensionName);
  }

  auto requiredExtensionsImpl_ = impl_->getXrRequiredExtensions();
  requiredExtensions_.insert(std::end(requiredExtensions_),
                             std::begin(requiredExtensionsImpl_),
                             std::end(requiredExtensionsImpl_));

  for (auto& requiredExtension : requiredExtensions_) {
    auto it = std::find_if(std::begin(extensions_),
                           std::end(extensions_),
                           [&requiredExtension](const XrExtensionProperties& extension) {
                             return strcmp(extension.extensionName, requiredExtension) == 0;
                           });
    if (it == std::end(extensions_)) {
      IGL_LOG_ERROR("Extension %s is required.", requiredExtension);
      return false;
    }
  }

  auto checkExtensionSupported = [this](const char* name) {
    return std::any_of(std::begin(extensions_),
                       std::end(extensions_),
                       [&](const XrExtensionProperties& extension) {
                         return strcmp(extension.extensionName, name) == 0;
                       });
  };

  passthroughSupported_ = checkExtensionSupported(XR_FB_PASSTHROUGH_EXTENSION_NAME);
  IGL_LOG_INFO("Passthrough is %s", passthroughSupported_ ? "supported" : "not supported");

  auto checkNeedRequiredExtension = [this](const char* name) {
    return std::find_if(std::begin(requiredExtensions_),
                        std::end(requiredExtensions_),
                        [&](const char* extensionName) {
                          return strcmp(extensionName, name) == 0;
                        }) == std::end(requiredExtensions_);
  };

  // Add passthough extension if supported.
  if (passthroughSupported_ && checkNeedRequiredExtension(XR_FB_PASSTHROUGH_EXTENSION_NAME)) {
    requiredExtensions_.push_back(XR_FB_PASSTHROUGH_EXTENSION_NAME);
  }

  handsTrackingSupported_ = false;//checkExtensionSupported(XR_EXT_HAND_TRACKING_EXTENSION_NAME);
  IGL_LOG_INFO("Hands tracking is %s", handsTrackingSupported_ ? "supported" : "not supported");

  handsTrackingMeshSupported_ = checkExtensionSupported(XR_FB_HAND_TRACKING_MESH_EXTENSION_NAME);
  IGL_LOG_INFO("Hands tracking mesh is %s",
               handsTrackingMeshSupported_ ? "supported" : "not supported");

  // Add hands tracking extension if supported.
  if (handsTrackingSupported_ && checkNeedRequiredExtension(XR_EXT_HAND_TRACKING_EXTENSION_NAME)) {
    requiredExtensions_.push_back(XR_EXT_HAND_TRACKING_EXTENSION_NAME);

    if (handsTrackingMeshSupported_ &&
        checkNeedRequiredExtension(XR_FB_HAND_TRACKING_MESH_EXTENSION_NAME)) {
      requiredExtensions_.push_back(XR_FB_HAND_TRACKING_MESH_EXTENSION_NAME);
    }
  }

  refreshRateExtensionSupported_ =
      checkExtensionSupported(XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME);
  IGL_LOG_INFO("RefreshRate is %s", refreshRateExtensionSupported_ ? "supported" : "not supported");

  if (refreshRateExtensionSupported_ &&
      checkNeedRequiredExtension(XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME)) {
    requiredExtensions_.push_back(XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME);
  }
  
  compositionLayerSettingsSupported_ = 
      checkExtensionSupported(XR_FB_COMPOSITION_LAYER_SETTINGS_EXTENSION_NAME);
  IGL_LOG_INFO("Composition Layer Settings are %s", 
	compositionLayerSettingsSupported_ ? "supported" : "not supported");

  if (compositionLayerSettingsSupported_ && 
	checkNeedRequiredExtension(XR_FB_COMPOSITION_LAYER_SETTINGS_EXTENSION_NAME)) {
    requiredExtensions_.push_back(XR_FB_COMPOSITION_LAYER_SETTINGS_EXTENSION_NAME);
  }

  touchProControllersSupported_ = 
      checkExtensionSupported(XR_FB_TOUCH_CONTROLLER_PRO_EXTENSION_NAME);
  IGL_LOG_INFO("Touch Pro controllers are %s", touchProControllersSupported_ ? "supported" : "not supported");

  if (touchProControllersSupported_ && 
      checkNeedRequiredExtension(XR_FB_TOUCH_CONTROLLER_PRO_EXTENSION_NAME)) {
    requiredExtensions_.push_back(XR_FB_TOUCH_CONTROLLER_PRO_EXTENSION_NAME);
  }

  touchControllerProximitySupported_ = checkExtensionSupported(XR_FB_TOUCH_CONTROLLER_PROXIMITY_EXTENSION_NAME);
  IGL_LOG_INFO("Touch controller proximity is %s", touchControllerProximitySupported_ ? "supported" : "not supported");

  if (touchControllerProximitySupported_ && 
      checkNeedRequiredExtension(XR_FB_TOUCH_CONTROLLER_PROXIMITY_EXTENSION_NAME)) {
    requiredExtensions_.push_back(XR_FB_TOUCH_CONTROLLER_PROXIMITY_EXTENSION_NAME);
  }

  bodyTrackingFBSupported_ = checkExtensionSupported(XR_FB_BODY_TRACKING_EXTENSION_NAME);
  IGL_LOG_INFO("FB Body Tracking is %s", bodyTrackingFBSupported_ ? "supported" : "not supported");

  if (bodyTrackingFBSupported_ &&
    checkNeedRequiredExtension(XR_FB_BODY_TRACKING_EXTENSION_NAME)) {
    requiredExtensions_.push_back(XR_FB_BODY_TRACKING_EXTENSION_NAME);
  }

#if ENABLE_META_OPENXR_FEATURES
  metaFullBodyTrackingSupported_ = checkExtensionSupported(XR_META_BODY_TRACKING_FULL_BODY_EXTENSION_NAME);
  IGL_LOG_INFO("Meta Full Body Tracking is %s", metaFullBodyTrackingSupported_ ? "supported" : "not supported");

  if (metaFullBodyTrackingSupported_ &&
    checkNeedRequiredExtension(XR_META_BODY_TRACKING_FULL_BODY_EXTENSION_NAME)) {
    requiredExtensions_.push_back(XR_META_BODY_TRACKING_FULL_BODY_EXTENSION_NAME);
  }

  metaBodyTrackingFidelitySupported_ = checkExtensionSupported(XR_META_BODY_TRACKING_FIDELITY_EXTENSION_NAME);
  IGL_LOG_INFO("Meta Body Tracking Fidelity is %s", metaBodyTrackingFidelitySupported_ ? "supported" : "not supported");

  if (metaBodyTrackingFidelitySupported_ &&
    checkNeedRequiredExtension(XR_META_BODY_TRACKING_FIDELITY_EXTENSION_NAME)) {
    requiredExtensions_.push_back(XR_META_BODY_TRACKING_FIDELITY_EXTENSION_NAME);
  }

  simultaneousHandsAndControllersSupported_ = checkExtensionSupported(XR_META_SIMULTANEOUS_HANDS_AND_CONTROLLERS_EXTENSION_NAME);
  IGL_LOG_INFO("Simultaneous Hands and Controllers are %s", simultaneousHandsAndControllersSupported_ ? "supported" : "not supported");

  if (simultaneousHandsAndControllersSupported_ &&
    checkNeedRequiredExtension(XR_META_SIMULTANEOUS_HANDS_AND_CONTROLLERS_EXTENSION_NAME)) {
    requiredExtensions_.push_back(XR_META_SIMULTANEOUS_HANDS_AND_CONTROLLERS_EXTENSION_NAME);
  }
#endif

  eyeTrackingSocialFBSupported_ = checkExtensionSupported(XR_FB_EYE_TRACKING_SOCIAL_EXTENSION_NAME);
  IGL_LOG_INFO("FB Eye Tracking Social is %s", eyeTrackingSocialFBSupported_ ? "supported" : "not supported");

  if (eyeTrackingSocialFBSupported_ &&
    checkNeedRequiredExtension(XR_FB_EYE_TRACKING_SOCIAL_EXTENSION_NAME)) {
    requiredExtensions_.push_back(XR_FB_EYE_TRACKING_SOCIAL_EXTENSION_NAME);
  }

  htcViveFocus3ControllersSupported_ = checkExtensionSupported(XR_HTC_VIVE_FOCUS3_CONTROLLER_INTERACTION_EXTENSION_NAME);
  IGL_LOG_INFO("HTC Vive Focus 3 Controllers are %s", htcViveFocus3ControllersSupported_ ? "supported" : "not supported");

  if (htcViveFocus3ControllersSupported_ &&
    checkNeedRequiredExtension(XR_HTC_VIVE_FOCUS3_CONTROLLER_INTERACTION_EXTENSION_NAME)) {
    requiredExtensions_.push_back(XR_HTC_VIVE_FOCUS3_CONTROLLER_INTERACTION_EXTENSION_NAME);
  }

  byteDanceControllersSupported_ = checkExtensionSupported(XR_BD_CONTROLLER_INTERACTION_EXTENSION_NAME);
  IGL_LOG_INFO("ByteDance (Pico 3/4) Controllers are %s", byteDanceControllersSupported_ ? "supported" : "not supported");

  if (byteDanceControllersSupported_ &&
    checkNeedRequiredExtension(XR_BD_CONTROLLER_INTERACTION_EXTENSION_NAME)) {
    requiredExtensions_.push_back(XR_BD_CONTROLLER_INTERACTION_EXTENSION_NAME);
  }

  return true;
}

bool XrApp::createInstance() {
  XrApplicationInfo appInfo = {};
  strcpy(appInfo.applicationName, kAppName);
  appInfo.applicationVersion = 0;
  strcpy(appInfo.engineName, kEngineName);
  appInfo.engineVersion = 0;
  appInfo.apiVersion = XR_MAKE_VERSION(1, 0, 34);

  XrInstanceCreateInfo instanceCreateInfo = {
      .type = XR_TYPE_INSTANCE_CREATE_INFO,
      .next = impl_->getInstanceCreateExtension(),
      .createFlags = 0,
      .applicationInfo = appInfo,
      .enabledApiLayerCount = 0,
      .enabledApiLayerNames = nullptr,
      .enabledExtensionCount = static_cast<uint32_t>(requiredExtensions_.size()),
      .enabledExtensionNames = requiredExtensions_.data()
  };

  XrResult initResult;
  XR_CHECK(initResult = xrCreateInstance(&instanceCreateInfo, &instance_));
  if (initResult != XR_SUCCESS) {
    IGL_LOG_ERROR("Failed to create XR instance: %d.", initResult);
    return false;
  }

  XR_CHECK(xrGetInstanceProperties(instance_, &instanceProps_));
  IGL_LOG_INFO("Runtime %s: Version : %u.%u.%u",
               instanceProps_.runtimeName,
               XR_VERSION_MAJOR(instanceProps_.runtimeVersion),
               XR_VERSION_MINOR(instanceProps_.runtimeVersion),
               XR_VERSION_PATCH(instanceProps_.runtimeVersion));

  if (passthroughSupported_) {
    XR_CHECK(xrGetInstanceProcAddr(
        instance_, "xrCreatePassthroughFB", (PFN_xrVoidFunction*)(&xrCreatePassthroughFB_)));
    XR_CHECK(xrGetInstanceProcAddr(
        instance_, "xrDestroyPassthroughFB", (PFN_xrVoidFunction*)(&xrDestroyPassthroughFB_)));
    XR_CHECK(xrGetInstanceProcAddr(
        instance_, "xrPassthroughStartFB", (PFN_xrVoidFunction*)(&xrPassthroughStartFB_)));
    XR_CHECK(xrGetInstanceProcAddr(instance_,
                                   "xrCreatePassthroughLayerFB",
                                   (PFN_xrVoidFunction*)(&xrCreatePassthroughLayerFB_)));
    XR_CHECK(xrGetInstanceProcAddr(instance_,
                                   "xrDestroyPassthroughLayerFB",
                                   (PFN_xrVoidFunction*)(&xrDestroyPassthroughLayerFB_)));
    XR_CHECK(xrGetInstanceProcAddr(instance_,
                                   "xrPassthroughLayerSetStyleFB",
                                   (PFN_xrVoidFunction*)(&xrPassthroughLayerSetStyleFB_)));
  }

  if (handsTrackingSupported_) {
    XR_CHECK(xrGetInstanceProcAddr(
        instance_, "xrCreateHandTrackerEXT", (PFN_xrVoidFunction*)(&xrCreateHandTrackerEXT_)));
    IGL_ASSERT(xrCreateHandTrackerEXT_ != nullptr);
    XR_CHECK(xrGetInstanceProcAddr(
        instance_, "xrDestroyHandTrackerEXT", (PFN_xrVoidFunction*)(&xrDestroyHandTrackerEXT_)));
    IGL_ASSERT(xrDestroyHandTrackerEXT_ != nullptr);
    XR_CHECK(xrGetInstanceProcAddr(
        instance_, "xrLocateHandJointsEXT", (PFN_xrVoidFunction*)(&xrLocateHandJointsEXT_)));
    IGL_ASSERT(xrLocateHandJointsEXT_ != nullptr);
    if (handsTrackingMeshSupported_) {
      XR_CHECK(xrGetInstanceProcAddr(
          instance_, "xrGetHandMeshFB", (PFN_xrVoidFunction*)(&xrGetHandMeshFB_)));
      IGL_ASSERT(xrGetHandMeshFB_ != nullptr);
    }
  }

  if (refreshRateExtensionSupported_) {
    XR_CHECK(xrGetInstanceProcAddr(instance_,
                                   "xrGetDisplayRefreshRateFB",
                                   (PFN_xrVoidFunction*)(&xrGetDisplayRefreshRateFB_)));
    IGL_ASSERT(xrGetDisplayRefreshRateFB_ != nullptr);
    XR_CHECK(xrGetInstanceProcAddr(instance_,
                                   "xrEnumerateDisplayRefreshRatesFB",
                                   (PFN_xrVoidFunction*)(&xrEnumerateDisplayRefreshRatesFB_)));
    IGL_ASSERT(xrEnumerateDisplayRefreshRatesFB_ != nullptr);
    XR_CHECK(xrGetInstanceProcAddr(instance_,
                                   "xrRequestDisplayRefreshRateFB",
                                   (PFN_xrVoidFunction*)(&xrRequestDisplayRefreshRateFB_)));
    IGL_ASSERT(xrRequestDisplayRefreshRateFB_ != nullptr);
  }

  return true;
} // namespace igl::shell::openxr

bool XrApp::createSystem() {
  XrSystemGetInfo systemGetInfo = {
      .type = XR_TYPE_SYSTEM_GET_INFO,
      .formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY,
  };

  XrResult result;
  XR_CHECK(result = xrGetSystem(instance_, &systemGetInfo, &systemId_));
  if (result != XR_SUCCESS) {
    IGL_LOG_ERROR("Failed to get system.");
    return false;
  }

#if ENABLE_META_OPENXR_FEATURES
  XrSystemPropertiesBodyTrackingFullBodyMETA meta_full_body_tracking_properties{ XR_TYPE_SYSTEM_PROPERTIES_BODY_TRACKING_FULL_BODY_META };

  if (metaFullBodyTrackingSupported_)
  {
      meta_full_body_tracking_properties.next = systemProps_.next;
      systemProps_.next = &meta_full_body_tracking_properties;
  }

  XrSystemSimultaneousHandsAndControllersPropertiesMETA simultaneous_properties = { XR_TYPE_SYSTEM_SIMULTANEOUS_HANDS_AND_CONTROLLERS_PROPERTIES_META };

  if (simultaneousHandsAndControllersSupported_)
  {
      simultaneous_properties.next = systemProps_.next;
      systemProps_.next = &simultaneous_properties;
  }
#endif

  XR_CHECK(xrGetSystemProperties(instance_, systemId_, &systemProps_));

#if ENABLE_META_OPENXR_FEATURES
  metaFullBodyTrackingSupported_ = meta_full_body_tracking_properties.supportsFullBodyTracking;
  simultaneousHandsAndControllersSupported_ = simultaneous_properties.supportsSimultaneousHandsAndControllers;
#endif

  IGL_LOG_INFO(
      "System Properties: Name=%s VendorId=%x", systemProps_.systemName, systemProps_.vendorId);
  IGL_LOG_INFO("System Graphics Properties: MaxWidth=%d MaxHeight=%d MaxLayers=%d",
               systemProps_.graphicsProperties.maxSwapchainImageWidth,
               systemProps_.graphicsProperties.maxSwapchainImageHeight,
               systemProps_.graphicsProperties.maxLayerCount);
  IGL_LOG_INFO("System Tracking Properties: OrientationTracking=%s PositionTracking=%s",
               systemProps_.trackingProperties.orientationTracking ? "True" : "False",
               systemProps_.trackingProperties.positionTracking ? "True" : "False");

  return true;
}

bool XrApp::createPassthrough() {
  if (!passthroughSupported_) {
    return false;
  }
  XrPassthroughCreateInfoFB passthroughInfo{XR_TYPE_PASSTHROUGH_CREATE_INFO_FB};
  passthroughInfo.next = nullptr;
  passthroughInfo.flags = 0u;

  XrResult result;
  XR_CHECK(result = xrCreatePassthroughFB_(session_, &passthroughInfo, &passthrough_));
  if (result != XR_SUCCESS) {
    IGL_LOG_ERROR("xrCreatePassthroughFB failed.");
    return false;
  }

  XrPassthroughLayerCreateInfoFB layerInfo{XR_TYPE_PASSTHROUGH_LAYER_CREATE_INFO_FB};
  layerInfo.next = nullptr;
  layerInfo.passthrough = passthrough_;
  layerInfo.purpose = XR_PASSTHROUGH_LAYER_PURPOSE_RECONSTRUCTION_FB;
  layerInfo.flags = XR_PASSTHROUGH_IS_RUNNING_AT_CREATION_BIT_FB;
  XR_CHECK(result = xrCreatePassthroughLayerFB_(session_, &layerInfo, &passthrougLayer_));
  if (result != XR_SUCCESS) {
    IGL_LOG_ERROR("xrCreatePassthroughLayerFB failed.");
    return false;
  }

  XrPassthroughStyleFB style{XR_TYPE_PASSTHROUGH_STYLE_FB};
  style.next = nullptr;
  style.textureOpacityFactor = 1.0f;
  style.edgeColor = {0.0f, 0.0f, 0.0f, 0.0f};
  XR_CHECK(result = xrPassthroughLayerSetStyleFB_(passthrougLayer_, &style));
  if (result != XR_SUCCESS) {
    IGL_LOG_ERROR("xrPassthroughLayerSetStyleFB failed.");
    return false;
  }

  XR_CHECK(result = xrPassthroughStartFB_(passthrough_));
  if (result != XR_SUCCESS) {
    IGL_LOG_ERROR("xrPassthroughStartFB failed.");
    return false;
  }
  return true;
}

bool XrApp::createHandsTracking() {
  if (!handsTrackingSupported_) {
    return false;
  }
  XrHandTrackerCreateInfoEXT createInfo{XR_TYPE_HAND_TRACKER_CREATE_INFO_EXT};
  createInfo.handJointSet = XR_HAND_JOINT_SET_DEFAULT_EXT;
  createInfo.hand = XR_HAND_LEFT_EXT;

  std::array<XrHandTrackingDataSourceEXT, 2> dataSources = {
      XR_HAND_TRACKING_DATA_SOURCE_UNOBSTRUCTED_EXT,
      XR_HAND_TRACKING_DATA_SOURCE_CONTROLLER_EXT,
  };

  XrHandTrackingDataSourceInfoEXT dataSourceInfo{XR_TYPE_HAND_TRACKING_DATA_SOURCE_INFO_EXT};
  dataSourceInfo.requestedDataSourceCount = static_cast<uint32_t>(dataSources.size());
  dataSourceInfo.requestedDataSources = dataSources.data();

  createInfo.next = &dataSourceInfo;

  XrResult result;
  XR_CHECK(result = xrCreateHandTrackerEXT_(session_, &createInfo, &leftHandTracker_));
  if (result != XR_SUCCESS) {
    IGL_LOG_ERROR("xrCreateHandTrackerEXT (left hand) failed.");
    return false;
  }

  createInfo.hand = XR_HAND_RIGHT_EXT;
  XR_CHECK(result = xrCreateHandTrackerEXT_(session_, &createInfo, &rightHandTracker_));
  if (result != XR_SUCCESS) {
    IGL_LOG_ERROR("xrCreateHandTrackerEXT (right hand) failed.");
    return false;
  }

  return true;
}

void XrApp::updateHandMeshes() {
  if (!handsTrackingMeshSupported_ || !xrGetHandMeshFB_) {
    return;
  }
  auto& handMeshes = shellParams_->handMeshes;

  XrResult result;
  XrHandTrackerEXT trackers[] = {leftHandTracker_, rightHandTracker_};
  for (uint8_t i = 0; i < 2; ++i) {
    XrHandTrackingMeshFB mesh{XR_TYPE_HAND_TRACKING_MESH_FB};
    XR_CHECK(result = xrGetHandMeshFB_(trackers[i], &mesh));
    if (result != XR_SUCCESS) {
      continue;
    }

    IGL_ASSERT(mesh.jointCountOutput <= XR_HAND_JOINT_COUNT_EXT);
    XrPosef jointBindPoses[XR_HAND_JOINT_COUNT_EXT]{};
    XrHandJointEXT jointParents[XR_HAND_JOINT_COUNT_EXT]{};
    float jointRadii[XR_HAND_JOINT_COUNT_EXT]{};

    mesh.jointCapacityInput = mesh.jointCountOutput;
    mesh.vertexCapacityInput = mesh.vertexCountOutput;
    mesh.indexCapacityInput = mesh.indexCountOutput;

    std::vector<XrVector3f> vertexPositions(mesh.vertexCapacityInput);
    std::vector<XrVector3f> vertexNormals(mesh.vertexCapacityInput);
    std::vector<XrVector2f> vertexUVs(mesh.vertexCapacityInput);
    std::vector<XrVector4sFB> vertexBlendIndices(mesh.vertexCapacityInput);
    std::vector<XrVector4f> vertexBlendWeights(mesh.vertexCapacityInput);

    handMeshes[i].indices.resize(mesh.indexCapacityInput);

    mesh.jointBindPoses = jointBindPoses;
    mesh.jointParents = jointParents;
    mesh.jointRadii = jointRadii;
    mesh.vertexPositions = vertexPositions.data();
    mesh.vertexNormals = vertexNormals.data();
    mesh.vertexUVs = vertexUVs.data();
    mesh.vertexBlendIndices = vertexBlendIndices.data();
    mesh.vertexBlendWeights = vertexBlendWeights.data();
    mesh.indices = handMeshes[i].indices.data();

    XR_CHECK(result = xrGetHandMeshFB_(trackers[i], &mesh));
    if (result != XR_SUCCESS) {
      continue;
    }

    handMeshes[i].vertexCountOutput = mesh.vertexCountOutput;
    handMeshes[i].indexCountOutput = mesh.indexCountOutput;
    handMeshes[i].jointCountOutput = mesh.jointCountOutput;
    handMeshes[i].vertexPositions.reserve(mesh.vertexCountOutput);
    handMeshes[i].vertexNormals.reserve(mesh.vertexCountOutput);
    handMeshes[i].vertexUVs.reserve(mesh.vertexCountOutput);
    handMeshes[i].vertexBlendIndices.reserve(mesh.vertexCountOutput);
    handMeshes[i].vertexBlendWeights.reserve(mesh.vertexCountOutput);
    handMeshes[i].jointBindPoses.reserve(mesh.jointCountOutput);

    for (uint32_t j = 0; j < mesh.vertexCountOutput; ++j) {
      handMeshes[i].vertexPositions.emplace_back(glmVecFromXrVec(mesh.vertexPositions[j]));
      handMeshes[i].vertexUVs.emplace_back(glmVecFromXrVec(mesh.vertexUVs[j]));
      handMeshes[i].vertexNormals.emplace_back(glmVecFromXrVec(mesh.vertexNormals[j]));
      handMeshes[i].vertexBlendIndices.emplace_back(glmVecFromXrVec(mesh.vertexBlendIndices[j]));
      handMeshes[i].vertexBlendWeights.emplace_back(glmVecFromXrVec(mesh.vertexBlendWeights[j]));
    }

    for (uint32_t j = 0; j < mesh.jointCountOutput; ++j) {
      handMeshes[i].jointBindPoses.emplace_back(poseFromXrPose(mesh.jointBindPoses[j]));
    }
  }
}

void XrApp::updateHandTracking() {
  if (!handsTrackingSupported_) {
    return;
  }
  auto& handTracking = shellParams_->handTracking;

  XrResult result;
  XrHandTrackerEXT trackers[] = {leftHandTracker_, rightHandTracker_};
  for (uint8_t i = 0; i < 2; ++i) {
    XrHandJointLocationEXT jointLocations[XR_HAND_JOINT_COUNT_EXT];
    XrHandJointVelocityEXT jointVelocities[XR_HAND_JOINT_COUNT_EXT];

    XrHandJointVelocitiesEXT velocities{.type = XR_TYPE_HAND_JOINT_VELOCITIES_EXT,
                                        .next = nullptr,
                                        .jointCount = XR_HAND_JOINT_COUNT_EXT,
                                        .jointVelocities = jointVelocities};

    XrHandJointLocationsEXT locations{.type = XR_TYPE_HAND_JOINT_LOCATIONS_EXT,
                                      .next = &velocities,
                                      .jointCount = XR_HAND_JOINT_COUNT_EXT,
                                      .jointLocations = jointLocations};

    XrHandJointsMotionRangeInfoEXT motionRangeInfo{XR_TYPE_HAND_JOINTS_MOTION_RANGE_INFO_EXT};
    motionRangeInfo.handJointsMotionRange =
        XR_HAND_JOINTS_MOTION_RANGE_CONFORMING_TO_CONTROLLER_EXT;

    const XrHandJointsLocateInfoEXT locateInfo{.type = XR_TYPE_HAND_JOINTS_LOCATE_INFO_EXT,
                                               .next = &motionRangeInfo,
                                               .baseSpace = currentSpace_,
                                               .time = currentTimeInNs()};

    handTracking[i].jointPose.resize(XR_HAND_JOINT_COUNT_EXT);
    handTracking[i].jointVelocity.resize(XR_HAND_JOINT_COUNT_EXT);
    handTracking[i].isJointTracked.resize(XR_HAND_JOINT_COUNT_EXT);

    XR_CHECK(result = xrLocateHandJointsEXT_(trackers[i], &locateInfo, &locations));
    if (result != XR_SUCCESS) {
      for (size_t jointIndex = 0; jointIndex < XR_HAND_JOINT_COUNT_EXT; ++jointIndex) {
        handTracking[i].isJointTracked[jointIndex] = false;
      }
      continue;
    }

    if (!locations.isActive) {
      for (size_t jointIndex = 0; jointIndex < XR_HAND_JOINT_COUNT_EXT; ++jointIndex) {
        handTracking[i].isJointTracked[jointIndex] = false;
      }
      continue;
    }

    constexpr XrSpaceLocationFlags isValid =
        XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT;
    for (size_t jointIndex = 0; jointIndex < XR_HAND_JOINT_COUNT_EXT; ++jointIndex) {
      if ((jointLocations[jointIndex].locationFlags & isValid) != 0) {
        handTracking[i].jointPose[jointIndex] = poseFromXrPose(jointLocations[jointIndex].pose);
        handTracking[i].jointVelocity[jointIndex].linear =
            glmVecFromXrVec(jointVelocities[jointIndex].linearVelocity);
        handTracking[i].jointVelocity[jointIndex].angular =
            glmVecFromXrVec(jointVelocities[jointIndex].angularVelocity);
        handTracking[i].isJointTracked[jointIndex] = true;
      } else {
        handTracking[i].isJointTracked[jointIndex] = false;
      }
    }
  }
}

bool XrApp::enumerateViewConfigurations() {
  uint32_t numViewConfigs = 0;
  XR_CHECK(xrEnumerateViewConfigurations(instance_, systemId_, 0, &numViewConfigs, nullptr));

  std::vector<XrViewConfigurationType> viewConfigTypes(numViewConfigs);
  XR_CHECK(xrEnumerateViewConfigurations(
      instance_, systemId_, numViewConfigs, &numViewConfigs, viewConfigTypes.data()));

  IGL_LOG_INFO("Available Viewport Configuration Types: %d", numViewConfigs);
  auto foundViewConfig = false;
  for (auto& viewConfigType : viewConfigTypes) {
    IGL_LOG_INFO("View configuration type %d : %s",
                 viewConfigType,
                 viewConfigType == kSupportedViewConfigType ? "Selected" : "");

    if (viewConfigType != kSupportedViewConfigType) {
      continue;
    }

    // Check properties
    XrViewConfigurationProperties viewConfigProps = {XR_TYPE_VIEW_CONFIGURATION_PROPERTIES};
    XR_CHECK(
        xrGetViewConfigurationProperties(instance_, systemId_, viewConfigType, &viewConfigProps));
    IGL_LOG_INFO("FovMutable=%s ConfigurationType %d",
                 viewConfigProps.fovMutable ? "true" : "false",
                 viewConfigProps.viewConfigurationType);

    // Check views
    uint32_t numViewports = 0;
    XR_CHECK(xrEnumerateViewConfigurationViews(
        instance_, systemId_, viewConfigType, 0, &numViewports, nullptr));

    if (!IGL_VERIFY(numViewports == kNumViews)) {
      IGL_LOG_ERROR(
          "numViewports must be %d. Make sure XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO is used.",
          kNumViews);
      return false;
    }

#if ENABLE_CLOUDXR
      ok_config_s.load();
#endif

    XR_CHECK(xrEnumerateViewConfigurationViews(
        instance_, systemId_, viewConfigType, numViewports, &numViewports, viewports_.data()));

    for (auto& view : viewports_) {
      (void)view; // doesn't compile in release for unused variable

#if ENABLE_CLOUDXR
        view.recommendedImageRectWidth = ok_config_s.per_eye_width_;
        view.recommendedImageRectHeight = ok_config_s.per_eye_height_;
#endif

        IGL_LOG_INFO("Viewport [%d]: Recommended Width=%d Height=%d SampleCount=%d",
                   view,
                   view.recommendedImageRectWidth,
                   view.recommendedImageRectHeight,
                   view.recommendedSwapchainSampleCount);

      IGL_LOG_INFO("Viewport [%d]: Max Width=%d Height=%d SampleCount=%d",
                   view,
                   view.maxImageRectWidth,
                   view.maxImageRectHeight,
                   view.maxSwapchainSampleCount);
    }

    viewConfigProps_ = viewConfigProps;

    foundViewConfig = true;

    break;
  }

  IGL_ASSERT_MSG(
      foundViewConfig, "XrViewConfigurationType %d not found.", kSupportedViewConfigType);

  return true;
}

void XrApp::enumerateReferenceSpaces() {
  uint32_t numRefSpaceTypes = 0;
  XR_CHECK(xrEnumerateReferenceSpaces(session_, 0, &numRefSpaceTypes, nullptr));

  std::vector<XrReferenceSpaceType> refSpaceTypes(numRefSpaceTypes);

  XR_CHECK(xrEnumerateReferenceSpaces(
      session_, numRefSpaceTypes, &numRefSpaceTypes, refSpaceTypes.data()));

  stageSpaceSupported_ =
      std::any_of(std::begin(refSpaceTypes), std::end(refSpaceTypes), [](const auto& type) {
        return type == XR_REFERENCE_SPACE_TYPE_STAGE;
      });
  IGL_LOG_INFO("OpenXR stage reference space is %s",
               stageSpaceSupported_ ? "supported" : "not supported");
}

void XrApp::enumerateBlendModes() {
  uint32_t numBlendModes = 0;
  XR_CHECK(xrEnumerateEnvironmentBlendModes(
      instance_, systemId_, kSupportedViewConfigType, 0, &numBlendModes, nullptr));

  std::vector<XrEnvironmentBlendMode> blendModes(numBlendModes);
  XR_CHECK(xrEnumerateEnvironmentBlendModes(instance_,
                                            systemId_,
                                            kSupportedViewConfigType,
                                            numBlendModes,
                                            &numBlendModes,
                                            blendModes.data()));

  additiveBlendingSupported_ =
      std::any_of(std::begin(blendModes), std::end(blendModes), [](const auto& type) {
        return type == XR_ENVIRONMENT_BLEND_MODE_ADDITIVE;
      });
  IGL_LOG_INFO("OpenXR additive blending %s",
               additiveBlendingSupported_ ? "supported" : "not supported");
}

void XrApp::updateSwapchainProviders() {
  const size_t numSwapchainProviders = useSinglePassStereo_ ? numQuadLayersPerView_
                                                            : kNumViews * numQuadLayersPerView_;
  const size_t numViewsPerSwapchain = useSinglePassStereo_ ? kNumViews : 1;
  if (numSwapchainProviders != swapchainProviders_.size()) {
    swapchainProviders_.clear();
    swapchainProviders_.reserve(numSwapchainProviders);
    const size_t viewCnt = useSinglePassStereo_ ? 1 : kNumViews;
    for (size_t quadLayer = 0; quadLayer < numQuadLayersPerView_; quadLayer++) {
      for (size_t view = 0; view < viewCnt; view++) {
        swapchainProviders_.emplace_back(
            std::make_unique<XrSwapchainProvider>(impl_->createSwapchainProviderImpl(),
                                                  platform_,
                                                  session_,
                                                  viewports_[view],
                                                  numViewsPerSwapchain));
        swapchainProviders_.back()->initialize();
      }
    }
    IGL_ASSERT(numSwapchainProviders == swapchainProviders_.size());
  }
}

//#ifndef EXTERNAL_XR_BUILD
#if 1
bool XrApp::initialize(const struct android_app* app, const InitParams& params) {
  if (initialized_) {
    return false;
  }

  PFN_xrInitializeLoaderKHR xrInitializeLoaderKHR;
  XR_CHECK(xrGetInstanceProcAddr(
      XR_NULL_HANDLE, "xrInitializeLoaderKHR", (PFN_xrVoidFunction*)&xrInitializeLoaderKHR));
  if (xrInitializeLoaderKHR) {
    XrLoaderInitInfoAndroidKHR loaderInitializeInfoAndroid = {
        XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR,
        nullptr,
        app->activity->vm,
        app->activity->clazz,
    };

    XR_CHECK(xrInitializeLoaderKHR((XrLoaderInitInfoBaseHeaderKHR*)&loaderInitializeInfoAndroid));
  }

  if (!checkExtensions()) {
    return false;
  }

  XrInstanceCreateInfoAndroidKHR* instanceCreateInfoAndroid_ptr =
      (XrInstanceCreateInfoAndroidKHR*)impl_->getInstanceCreateExtension();

  if (instanceCreateInfoAndroid_ptr) {
    instanceCreateInfoAndroid_ptr->applicationVM = app->activity->vm;
    instanceCreateInfoAndroid_ptr->applicationActivity = app->activity->clazz;
  }

  if (!createInstance()) {
    return false;
  }

  if (!createSystem()) {
    return false;
  }

  if (!enumerateViewConfigurations()) {
    return false;
  }

  std::unique_ptr<igl::IDevice> device;
  device = impl_->initIGL(instance_, systemId_);
  if (!device) {
    IGL_LOG_ERROR("Failed to initialize IGL");
    return false;
  }

  useSinglePassStereo_ = useSinglePassStereo_ && device->hasFeature(igl::DeviceFeatures::Multiview);

  createShellSession(std::move(device), app->activity->assetManager);

  session_ = impl_->initXrSession(instance_, systemId_, platform_->getDevice());
  if (session_ == XR_NULL_HANDLE) {
    IGL_LOG_ERROR("Failed to initialize graphics system");
    return false;
  }

  // The following are initialization steps that happen after XrSession is created.
  enumerateReferenceSpaces();
  enumerateBlendModes();
  updateSwapchainProviders();
  createSpaces();
  createActions();

#if !ENABLE_CLOUDXR
  if (passthroughSupported_ && !createPassthrough()) {
    return false;
  }
#endif
  if (handsTrackingSupported_ && !createHandsTracking()) {
    return false;
  }
  if (refreshRateExtensionSupported_) {
    queryCurrentRefreshRate();
    if (params.refreshRateMode_ == InitParams::UseMaxRefreshRate) {
      setMaxRefreshRate();
    } else if (params.refreshRateMode_ == InitParams::UseSpecificRefreshRate) {
      setRefreshRate(params.desiredSpecificRefreshRate_);
    } else {
      // Do nothing. Use default refresh rate.
    }
  }
  updateHandMeshes();

  IGL_ASSERT(renderSession_ != nullptr);
  renderSession_->initialize();
  initialized_ = true;

  return initialized_;
}
#endif

void XrApp::createShellSession(std::unique_ptr<igl::IDevice> device, AAssetManager* assetMgr) {
  platform_ = std::make_shared<igl::shell::PlatformAndroid>(std::move(device));
  IGL_ASSERT(platform_ != nullptr);
  static_cast<igl::shell::ImageLoaderAndroid&>(platform_->getImageLoader())
      .setAssetManager(assetMgr);
  static_cast<igl::shell::FileLoaderAndroid&>(platform_->getFileLoader()).setAssetManager(assetMgr);
  renderSession_ = igl::shell::createDefaultRenderSession(platform_);
  shellParams_->shellControlsViewParams = true;
  shellParams_->rightHandedCoordinateSystem = true;
  shellParams_->renderMode = useSinglePassStereo_ ? RenderMode::SinglePassStereo
                                                  : RenderMode::DualPassStereo;
  shellParams_->viewParams.resize(useSinglePassStereo_ ? 2 : 1);
  renderSession_->setShellParams(*shellParams_);
}

void XrApp::createSpaces() {
  XrReferenceSpaceCreateInfo spaceCreateInfo = {
      XR_TYPE_REFERENCE_SPACE_CREATE_INFO,
      nullptr,
      XR_REFERENCE_SPACE_TYPE_VIEW,
      {{0.0f, 0.0f, 0.0f, 1.0f}},
  };
  XR_CHECK(xrCreateReferenceSpace(session_, &spaceCreateInfo, &headSpace_));

#if USE_LOCAL_AR_SPACE
  spaceCreateInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
#else
  spaceCreateInfo.referenceSpaceType = stageSpaceSupported_ ? XR_REFERENCE_SPACE_TYPE_STAGE
                                                            : XR_REFERENCE_SPACE_TYPE_LOCAL;
#endif
  XR_CHECK(xrCreateReferenceSpace(session_, &spaceCreateInfo, &currentSpace_));
}

void XrApp::createActions() {

    headsetType_ = compute_headset_type(systemProps_.systemName, systemProps_.systemId, systemProps_.vendorId);

    XrActionSetCreateInfo actionSetInfo{XR_TYPE_ACTION_SET_CREATE_INFO};
    strcpy(actionSetInfo.actionSetName, "gameplay");
    strcpy(actionSetInfo.localizedActionSetName, "Gameplay");
    actionSetInfo.priority = 0;
    XR_CHECK(xrCreateActionSet(instance_, &actionSetInfo, &xr_inputs_.actionSet));

    // Get the XrPath for the left and right hands - we will use them as subaction paths.
    xrStringToPath(instance_, "/user/hand/left", &xr_inputs_.handSubactionPath[LEFT]);
    xrStringToPath(instance_, "/user/hand/right", &xr_inputs_.handSubactionPath[RIGHT]);

    // Create actions.
    {
        // Create an input action for grabbing objects with the left and right hands.
        XrActionCreateInfo actionInfo{XR_TYPE_ACTION_CREATE_INFO};
        actionInfo.actionType = XR_ACTION_TYPE_FLOAT_INPUT;
        strcpy(actionInfo.actionName, "grab_object");
        strcpy(actionInfo.localizedActionName, "Grab Object");
        actionInfo.countSubactionPaths = uint32_t(xr_inputs_.handSubactionPath.size());
        actionInfo.subactionPaths = xr_inputs_.handSubactionPath.data();
        XR_CHECK(xrCreateAction(xr_inputs_.actionSet, &actionInfo, &xr_inputs_.grabAction));

        // Create an input action getting the left and right hand poses.
        actionInfo.actionType = XR_ACTION_TYPE_POSE_INPUT;
        strcpy(actionInfo.actionName, "grip_pose");
        strcpy(actionInfo.localizedActionName, "Grip Pose");
        actionInfo.countSubactionPaths = uint32_t(xr_inputs_.handSubactionPath.size());
        actionInfo.subactionPaths = xr_inputs_.handSubactionPath.data();
        XR_CHECK(xrCreateAction(xr_inputs_.actionSet, &actionInfo, &xr_inputs_.gripPoseAction));

        actionInfo.actionType = XR_ACTION_TYPE_POSE_INPUT;
        strcpy(actionInfo.actionName, "aim_pose");
        strcpy(actionInfo.localizedActionName, "Aim Pose");
        actionInfo.countSubactionPaths = uint32_t(xr_inputs_.handSubactionPath.size());
        actionInfo.subactionPaths = xr_inputs_.handSubactionPath.data();
        XR_CHECK(xrCreateAction(xr_inputs_.actionSet, &actionInfo, &xr_inputs_.aimPoseAction));

        // Menu / System
        actionInfo.actionType = XR_ACTION_TYPE_BOOLEAN_INPUT;
        strcpy(actionInfo.actionName, "menu_click");
        strcpy(actionInfo.localizedActionName, "Menu Click");
        XR_CHECK(xrCreateAction(xr_inputs_.actionSet, &actionInfo, &xr_inputs_.menuClickAction));

        // Trigger
        actionInfo.actionType = XR_ACTION_TYPE_BOOLEAN_INPUT;
        strcpy(actionInfo.actionName, "trigger_click");
        strcpy(actionInfo.localizedActionName, "Trigger Click");
        XR_CHECK(xrCreateAction(xr_inputs_.actionSet, &actionInfo, &xr_inputs_.triggerClickAction));

        actionInfo.actionType = XR_ACTION_TYPE_BOOLEAN_INPUT;
        strcpy(actionInfo.actionName, "trigger_touch");
        strcpy(actionInfo.localizedActionName, "Trigger Touch");
        XR_CHECK(xrCreateAction(xr_inputs_.actionSet, &actionInfo, &xr_inputs_.triggerTouchAction));

        actionInfo.actionType = XR_ACTION_TYPE_FLOAT_INPUT;
        strcpy(actionInfo.actionName, "trigger_value");
        strcpy(actionInfo.localizedActionName, "Trigger Value");
        XR_CHECK(xrCreateAction(xr_inputs_.actionSet, &actionInfo, &xr_inputs_.triggerValueAction));

        // Squeeze
        actionInfo.actionType = XR_ACTION_TYPE_BOOLEAN_INPUT;
        strcpy(actionInfo.actionName, "squeeze_click");
        strcpy(actionInfo.localizedActionName, "Squeeze Click");
        XR_CHECK(xrCreateAction(xr_inputs_.actionSet, &actionInfo, &xr_inputs_.squeezeClickAction));

        actionInfo.actionType = XR_ACTION_TYPE_BOOLEAN_INPUT;
        strcpy(actionInfo.actionName, "squeeze_touch");
        strcpy(actionInfo.localizedActionName, "Squeeze Touch");
        XR_CHECK(xrCreateAction(xr_inputs_.actionSet, &actionInfo, &xr_inputs_.squeezeTouchAction));

        actionInfo.actionType = XR_ACTION_TYPE_FLOAT_INPUT;
        strcpy(actionInfo.actionName, "squeeze_value");
        strcpy(actionInfo.localizedActionName, "Squeeze Value");
        XR_CHECK(xrCreateAction(xr_inputs_.actionSet, &actionInfo, &xr_inputs_.squeezeValueAction));

        // Thumbsticks
        actionInfo.actionType = XR_ACTION_TYPE_BOOLEAN_INPUT;
        strcpy(actionInfo.actionName, "thumbstick_click");
        strcpy(actionInfo.localizedActionName, "Thumbstick Click");
        XR_CHECK(xrCreateAction(xr_inputs_.actionSet, &actionInfo, &xr_inputs_.thumbstickClickAction));

        actionInfo.actionType = XR_ACTION_TYPE_BOOLEAN_INPUT;
        strcpy(actionInfo.actionName, "thumbstick_touch");
        strcpy(actionInfo.localizedActionName, "Thumbstick Touch");
        XR_CHECK(xrCreateAction(xr_inputs_.actionSet, &actionInfo, &xr_inputs_.thumbstickTouchAction));

        actionInfo.actionType = XR_ACTION_TYPE_FLOAT_INPUT;
        strcpy(actionInfo.actionName, "thumbstick_x");
        strcpy(actionInfo.localizedActionName, "Thumbstick X");
        XR_CHECK(xrCreateAction(xr_inputs_.actionSet, &actionInfo, &xr_inputs_.thumbstickXAction));

        actionInfo.actionType = XR_ACTION_TYPE_FLOAT_INPUT;
        strcpy(actionInfo.actionName, "thumbstick_y");
        strcpy(actionInfo.localizedActionName, "Thumbstick Y");
        XR_CHECK(xrCreateAction(xr_inputs_.actionSet, &actionInfo, &xr_inputs_.thumbstickYAction));

        actionInfo.actionType = XR_ACTION_TYPE_BOOLEAN_INPUT;
        strcpy(actionInfo.actionName, "thumbrest_touch");
        strcpy(actionInfo.localizedActionName, "Thumb Rest Touch");
        XR_CHECK(xrCreateAction(xr_inputs_.actionSet, &actionInfo, &xr_inputs_.thumbRestTouchAction));

        actionInfo.actionType = XR_ACTION_TYPE_BOOLEAN_INPUT;
        strcpy(actionInfo.actionName, "thumbrest_click");
        strcpy(actionInfo.localizedActionName, "Thumb Rest Click");
        XR_CHECK(xrCreateAction(xr_inputs_.actionSet, &actionInfo, &xr_inputs_.thumbRestClickAction));

        actionInfo.actionType = XR_ACTION_TYPE_FLOAT_INPUT;
        strcpy(actionInfo.actionName, "thumbrest_force");
        strcpy(actionInfo.localizedActionName, "Thumb Rest Force");
        XR_CHECK(xrCreateAction(xr_inputs_.actionSet, &actionInfo, &xr_inputs_.thumbRestForceAction));

        actionInfo.actionType = XR_ACTION_TYPE_FLOAT_INPUT;
        strcpy(actionInfo.actionName, "thumb_proximity");
        strcpy(actionInfo.localizedActionName, "Thumb Proximity");
        XR_CHECK(xrCreateAction(xr_inputs_.actionSet, &actionInfo, &xr_inputs_.thumbProximityAction));

        actionInfo.actionType = XR_ACTION_TYPE_FLOAT_INPUT;
        strcpy(actionInfo.actionName, "pinch_value");
        strcpy(actionInfo.localizedActionName, "Pinch Value");
        XR_CHECK(xrCreateAction(xr_inputs_.actionSet, &actionInfo, &xr_inputs_.pinchValueAction));

        actionInfo.actionType = XR_ACTION_TYPE_FLOAT_INPUT;
        strcpy(actionInfo.actionName, "pinch_force");
        strcpy(actionInfo.localizedActionName, "Pinch Force");
        XR_CHECK(xrCreateAction(xr_inputs_.actionSet, &actionInfo, &xr_inputs_.pinchForceAction));

        // A/X Button
        actionInfo.actionType = XR_ACTION_TYPE_BOOLEAN_INPUT;
        strcpy(actionInfo.actionName, "button_a_click");
        strcpy(actionInfo.localizedActionName, "Button A Click");
        XR_CHECK(xrCreateAction(xr_inputs_.actionSet, &actionInfo, &xr_inputs_.buttonAXClickAction));

        actionInfo.actionType = XR_ACTION_TYPE_BOOLEAN_INPUT;
        strcpy(actionInfo.actionName, "button_a_touch");
        strcpy(actionInfo.localizedActionName, "Button A Touch");
        XR_CHECK(xrCreateAction(xr_inputs_.actionSet, &actionInfo, &xr_inputs_.buttonAXTouchAction));

        // B/Y Button
        actionInfo.actionType = XR_ACTION_TYPE_BOOLEAN_INPUT;
        strcpy(actionInfo.actionName, "button_b_click");
        strcpy(actionInfo.localizedActionName, "Button B Click");
        XR_CHECK(xrCreateAction(xr_inputs_.actionSet, &actionInfo, &xr_inputs_.buttonBYClickAction));

        actionInfo.actionType = XR_ACTION_TYPE_BOOLEAN_INPUT;
        strcpy(actionInfo.actionName, "button_b_touch");
        strcpy(actionInfo.localizedActionName, "Button B Touch");
        XR_CHECK(xrCreateAction(xr_inputs_.actionSet, &actionInfo, &xr_inputs_.buttonBYTouchAction));

        actionInfo.actionType = XR_ACTION_TYPE_FLOAT_INPUT;
        strcpy(actionInfo.actionName, "trackpad_x");
        strcpy(actionInfo.localizedActionName, "trackpad X");
        XR_CHECK(xrCreateAction(xr_inputs_.actionSet, &actionInfo, &xr_inputs_.trackpadXAction));

        actionInfo.actionType = XR_ACTION_TYPE_FLOAT_INPUT;
        strcpy(actionInfo.actionName, "trackpad_y");
        strcpy(actionInfo.localizedActionName, "trackpad Y");
        XR_CHECK(xrCreateAction(xr_inputs_.actionSet, &actionInfo, &xr_inputs_.trackpadXAction));

        actionInfo.actionType = XR_ACTION_TYPE_VIBRATION_OUTPUT;
        strcpy(actionInfo.actionName, "vibrate_hand");
        strcpy(actionInfo.localizedActionName, "Vibrate Hand");
        actionInfo.countSubactionPaths = uint32_t(xr_inputs_.handSubactionPath.size());
        actionInfo.subactionPaths = xr_inputs_.handSubactionPath.data();
        XR_CHECK(xrCreateAction(xr_inputs_.actionSet, &actionInfo, &xr_inputs_.vibrateAction));
    }

    std::array<XrPath, NUM_SIDES> selectPath;

    xrStringToPath(instance_, "/user/hand/left/input/select/click", &selectPath[LEFT]);
    xrStringToPath(instance_, "/user/hand/right/input/select/click", &selectPath[RIGHT]);

    std::array<XrPath, NUM_SIDES> squeezeClickPath;

    xrStringToPath(instance_, "/user/hand/left/input/squeeze/click", &squeezeClickPath[LEFT]);
    xrStringToPath(instance_, "/user/hand/right/input/squeeze/click", &squeezeClickPath[RIGHT]);

    std::array<XrPath, NUM_SIDES> squeezeTouchPath;

    xrStringToPath(instance_, "/user/hand/left/input/squeeze/touch", &squeezeTouchPath[LEFT]);
    xrStringToPath(instance_, "/user/hand/right/input/squeeze/touch", &squeezeTouchPath[RIGHT]);

    std::array<XrPath, NUM_SIDES> squeezeValuePath;

    xrStringToPath(instance_, "/user/hand/left/input/squeeze/value", &squeezeValuePath[LEFT]);
    xrStringToPath(instance_, "/user/hand/right/input/squeeze/value", &squeezeValuePath[RIGHT]);

    std::array<XrPath, NUM_SIDES> squeezeForcePath;

    xrStringToPath(instance_, "/user/hand/left/input/squeeze/force", &squeezeForcePath[LEFT]);
    xrStringToPath(instance_, "/user/hand/right/input/squeeze/force", &squeezeForcePath[RIGHT]);

    std::array<XrPath, NUM_SIDES> triggerClickPath;

    xrStringToPath(instance_, "/user/hand/left/input/trigger/click", &triggerClickPath[LEFT]);
    xrStringToPath(instance_, "/user/hand/right/input/trigger/click", &triggerClickPath[RIGHT]);

    std::array<XrPath, NUM_SIDES> triggerTouchPath;

    xrStringToPath(instance_, "/user/hand/left/input/trigger/touch", &triggerTouchPath[LEFT]);
    xrStringToPath(instance_, "/user/hand/right/input/trigger/touch", &triggerTouchPath[RIGHT]);

    std::array<XrPath, NUM_SIDES> triggerValuePath;

    xrStringToPath(instance_, "/user/hand/left/input/trigger/value", &triggerValuePath[LEFT]);
    xrStringToPath(instance_, "/user/hand/right/input/trigger/value", &triggerValuePath[RIGHT]);

    std::array<XrPath, NUM_SIDES> menuClickPath;

    xrStringToPath(instance_, "/user/hand/left/input/menu/click", &menuClickPath[LEFT]);
    xrStringToPath(instance_, "/user/hand/right/input/menu/click", &menuClickPath[RIGHT]);

    std::array<XrPath, NUM_SIDES> gripPosePath;

    xrStringToPath(instance_, "/user/hand/left/input/grip/pose", &gripPosePath[LEFT]);
    xrStringToPath(instance_, "/user/hand/right/input/grip/pose", &gripPosePath[RIGHT]);

    std::array<XrPath, NUM_SIDES> aimPosePath;

    xrStringToPath(instance_, "/user/hand/left/input/aim/pose", &aimPosePath[LEFT]);
    xrStringToPath(instance_, "/user/hand/right/input/aim/pose", &aimPosePath[RIGHT]);

    std::array<XrPath, NUM_SIDES> stickClickPath;

    xrStringToPath(instance_, "/user/hand/left/input/thumbstick/click", &stickClickPath[LEFT]);
    xrStringToPath(instance_, "/user/hand/right/input/thumbstick/click", &stickClickPath[RIGHT]);

    std::array<XrPath, NUM_SIDES> stickTouchPath;

    xrStringToPath(instance_, "/user/hand/left/input/thumbstick/touch", &stickTouchPath[LEFT]);
    xrStringToPath(instance_, "/user/hand/right/input/thumbstick/touch", &stickTouchPath[RIGHT]);

    std::array<XrPath, NUM_SIDES> stickXPath;

    xrStringToPath(instance_, "/user/hand/left/input/thumbstick/x", &stickXPath[LEFT]);
    xrStringToPath(instance_, "/user/hand/right/input/thumbstick/x", &stickXPath[RIGHT]);

    std::array<XrPath, NUM_SIDES> stickYPath;

    xrStringToPath(instance_, "/user/hand/left/input/thumbstick/y", &stickYPath[LEFT]);
    xrStringToPath(instance_, "/user/hand/right/input/thumbstick/y", &stickYPath[RIGHT]);

    std::array<XrPath, NUM_SIDES> thumbRestTouchPath;

    xrStringToPath(instance_, "/user/hand/left/input/thumbrest/touch", &thumbRestTouchPath[LEFT]);
    xrStringToPath(instance_, "/user/hand/right/input/thumbrest/touch", &thumbRestTouchPath[RIGHT]);

    std::array<XrPath, NUM_SIDES> thumbRestClickPath;

    xrStringToPath(instance_, "/user/hand/left/input/thumbrest/click", &thumbRestClickPath[LEFT]);
    xrStringToPath(instance_, "/user/hand/right/input/thumbrest/click", &thumbRestClickPath[RIGHT]);

    std::array<XrPath, NUM_SIDES> thumbRestForcePath;

    xrStringToPath(instance_, "/user/hand/left/input/thumbrest/force", &thumbRestForcePath[LEFT]);
    xrStringToPath(instance_, "/user/hand/right/input/thumbrest/force", &thumbRestForcePath[RIGHT]);

    std::array<XrPath, NUM_SIDES> thumbProximityPath;

    xrStringToPath(instance_, "/user/hand/left/input/thumb_fb/proximity_fb", &thumbProximityPath[LEFT]);
    xrStringToPath(instance_, "/user/hand/right/input/thumb_fb/proximity_fb", &thumbProximityPath[RIGHT]);

    std::array<XrPath, NUM_SIDES> pinchValuePath;

    xrStringToPath(instance_, "/user/hand/left/input/pinch_fb/value", &pinchValuePath[LEFT]);
    xrStringToPath(instance_, "/user/hand/right/input/pinch_fb/value", &pinchValuePath[RIGHT]);

    std::array<XrPath, NUM_SIDES> pinchForcePath;

    xrStringToPath(instance_, "/user/hand/left/input/pinch_fb/force", &pinchForcePath[LEFT]);
    xrStringToPath(instance_, "/user/hand/right/input/pinch_fb/force", &pinchForcePath[RIGHT]);

    std::array<XrPath, NUM_SIDES> XA_ClickPath;

    xrStringToPath(instance_, "/user/hand/left/input/x/click", &XA_ClickPath[LEFT]);
    xrStringToPath(instance_, "/user/hand/right/input/a/click", &XA_ClickPath[RIGHT]);

    std::array<XrPath, NUM_SIDES> XA_TouchPath;

    xrStringToPath(instance_, "/user/hand/left/input/x/touch", &XA_TouchPath[LEFT]);
    xrStringToPath(instance_, "/user/hand/right/input/a/touch", &XA_TouchPath[RIGHT]);

    std::array<XrPath, NUM_SIDES> YB_ClickPath;

    xrStringToPath(instance_, "/user/hand/left/input/y/click", &YB_ClickPath[LEFT]);
    xrStringToPath(instance_, "/user/hand/right/input/b/click", &YB_ClickPath[RIGHT]);

    std::array<XrPath, NUM_SIDES> YB_TouchPath;

    xrStringToPath(instance_, "/user/hand/left/input/y/touch", &YB_TouchPath[LEFT]);
    xrStringToPath(instance_, "/user/hand/right/input/b/touch", &YB_TouchPath[RIGHT]);

    std::array<XrPath, NUM_SIDES> trackPad_X_Path;
    std::array<XrPath, NUM_SIDES> trackPad_Y_Path;

    xrStringToPath(instance_, "/user/hand/left/input/trackpad/x", &trackPad_X_Path[LEFT]);
    xrStringToPath(instance_, "/user/hand/right/input/trackpad/x", &trackPad_X_Path[RIGHT]);

    xrStringToPath(instance_, "/user/hand/left/input/trackpad/y", &trackPad_Y_Path[LEFT]);
    xrStringToPath(instance_, "/user/hand/right/input/trackpad/y", &trackPad_Y_Path[RIGHT]);

    std::array<XrPath, NUM_SIDES> hapticPath;

    xrStringToPath(instance_, "/user/hand/left/output/haptic", &hapticPath[LEFT]);
    xrStringToPath(instance_, "/user/hand/right/output/haptic", &hapticPath[RIGHT]);

    // Suggest bindings for KHR Simple.
    if (simpleControllersSupported_)
    {
        XrPath khrSimpleInteractionProfilePath;
        xrStringToPath(instance_, "/interaction_profiles/khr/simple_controller", &khrSimpleInteractionProfilePath);

        std::vector<XrActionSuggestedBinding> bindings{{
                                                               {xr_inputs_.grabAction, selectPath[LEFT]},
                                                               {xr_inputs_.grabAction, selectPath[RIGHT]},
                                                               {xr_inputs_.gripPoseAction, gripPosePath[LEFT]},
                                                               {xr_inputs_.gripPoseAction, gripPosePath[RIGHT]},
                                                               {xr_inputs_.aimPoseAction, aimPosePath[LEFT]},
                                                               {xr_inputs_.aimPoseAction, aimPosePath[RIGHT]},
                                                               {xr_inputs_.menuClickAction, menuClickPath[LEFT]},
                                                               {xr_inputs_.vibrateAction, hapticPath[LEFT]},
                                                               {xr_inputs_.vibrateAction, hapticPath[RIGHT]}}};

        XrInteractionProfileSuggestedBinding suggestedBindings{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
        suggestedBindings.interactionProfile = khrSimpleInteractionProfilePath;
        suggestedBindings.suggestedBindings = bindings.data();
        suggestedBindings.countSuggestedBindings = (uint32_t)bindings.size();
        XR_CHECK(xrSuggestInteractionProfileBindings(instance_, &suggestedBindings));
    }

    // Oculus Touch.
    if (touchControllersSupported_)
    {
        XrPath oculusTouchInteractionProfilePath;
        xrStringToPath(instance_, "/interaction_profiles/oculus/touch_controller", &oculusTouchInteractionProfilePath);

        std::vector<XrActionSuggestedBinding> oculus_touch_bindings{{
                                                                            {xr_inputs_.triggerClickAction, triggerValuePath[LEFT]},
                                                                            {xr_inputs_.triggerClickAction, triggerValuePath[RIGHT]},
                                                                            {xr_inputs_.triggerTouchAction, triggerTouchPath[LEFT]},
                                                                            {xr_inputs_.triggerTouchAction, triggerTouchPath[RIGHT]},
                                                                            {xr_inputs_.triggerValueAction, triggerValuePath[LEFT]},
                                                                            {xr_inputs_.triggerValueAction, triggerValuePath[RIGHT]},
                                                                            {xr_inputs_.squeezeClickAction, squeezeValuePath[LEFT]},
                                                                            {xr_inputs_.squeezeClickAction, squeezeValuePath[RIGHT]},
                                                                            {xr_inputs_.squeezeValueAction, squeezeValuePath[LEFT]},
                                                                            {xr_inputs_.squeezeValueAction, squeezeValuePath[RIGHT]},
                                                                            {xr_inputs_.gripPoseAction, gripPosePath[LEFT]},
                                                                            {xr_inputs_.gripPoseAction, gripPosePath[RIGHT]},
                                                                            {xr_inputs_.aimPoseAction, aimPosePath[LEFT]},
                                                                            {xr_inputs_.aimPoseAction, aimPosePath[RIGHT]},
                                                                            {xr_inputs_.menuClickAction, menuClickPath[LEFT]},
                                                                            {xr_inputs_.thumbstickClickAction, stickClickPath[LEFT]},
                                                                            {xr_inputs_.thumbstickClickAction, stickClickPath[RIGHT]},
                                                                            {xr_inputs_.thumbstickTouchAction, stickTouchPath[LEFT]},
                                                                            {xr_inputs_.thumbstickTouchAction, stickTouchPath[RIGHT]},
                                                                            {xr_inputs_.thumbstickXAction, stickXPath[LEFT]},
                                                                            {xr_inputs_.thumbstickXAction, stickXPath[RIGHT]},
                                                                            {xr_inputs_.thumbstickYAction, stickYPath[LEFT]},
                                                                            {xr_inputs_.thumbstickYAction, stickYPath[RIGHT]},
                                                                            {xr_inputs_.thumbRestTouchAction, thumbRestTouchPath[LEFT]},
                                                                            {xr_inputs_.thumbRestTouchAction, thumbRestTouchPath[RIGHT]},
                                                                            //{xr_inputs_.thumbRestClickAction, thumbRestClickPath[LEFT]},
                                                                            //{xr_inputs_.thumbRestClickAction, thumbRestClickPath[RIGHT]},
                                                                            {xr_inputs_.buttonAXClickAction, XA_ClickPath[LEFT]},
                                                                            {xr_inputs_.buttonAXClickAction, XA_ClickPath[RIGHT]},
                                                                            {xr_inputs_.buttonAXTouchAction, XA_TouchPath[LEFT]},
                                                                            {xr_inputs_.buttonAXTouchAction, XA_TouchPath[RIGHT]},
                                                                            {xr_inputs_.buttonBYClickAction, YB_ClickPath[LEFT]},
                                                                            {xr_inputs_.buttonBYClickAction, YB_ClickPath[RIGHT]},
                                                                            {xr_inputs_.buttonBYTouchAction, YB_TouchPath[LEFT]},
                                                                            {xr_inputs_.buttonBYTouchAction, YB_TouchPath[RIGHT]},
                                                                            {xr_inputs_.vibrateAction, hapticPath[LEFT]},
                                                                            {xr_inputs_.vibrateAction, hapticPath[RIGHT]}}};

        XrInteractionProfileSuggestedBinding suggestedBindings{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
        suggestedBindings.interactionProfile = oculusTouchInteractionProfilePath;
        suggestedBindings.suggestedBindings = oculus_touch_bindings.data();
        suggestedBindings.countSuggestedBindings = (uint32_t)oculus_touch_bindings.size();
        XR_CHECK(xrSuggestInteractionProfileBindings(instance_, &suggestedBindings));
    }

    // Touch Pro
    if (touchProControllersSupported_)
    {
        XrPath oculusTouchProInteractionProfilePath;
        xrStringToPath(instance_, "/interaction_profiles/facebook/touch_controller_pro", &oculusTouchProInteractionProfilePath);

        std::vector<XrActionSuggestedBinding> oculus_touch_pro_bindings{{
                                                                            {xr_inputs_.triggerClickAction, triggerValuePath[LEFT]},
                                                                            {xr_inputs_.triggerClickAction, triggerValuePath[RIGHT]},
                                                                            {xr_inputs_.triggerTouchAction, triggerTouchPath[LEFT]},
                                                                            {xr_inputs_.triggerTouchAction, triggerTouchPath[RIGHT]},
                                                                            {xr_inputs_.triggerValueAction, triggerValuePath[LEFT]},
                                                                            {xr_inputs_.triggerValueAction, triggerValuePath[RIGHT]},
                                                                            {xr_inputs_.squeezeClickAction, squeezeValuePath[LEFT]},
                                                                            {xr_inputs_.squeezeClickAction, squeezeValuePath[RIGHT]},
                                                                            {xr_inputs_.squeezeValueAction, squeezeValuePath[LEFT]},
                                                                            {xr_inputs_.squeezeValueAction, squeezeValuePath[RIGHT]},
                                                                            {xr_inputs_.gripPoseAction, gripPosePath[LEFT]},
                                                                            {xr_inputs_.gripPoseAction, gripPosePath[RIGHT]},
                                                                            {xr_inputs_.aimPoseAction, aimPosePath[LEFT]},
                                                                            {xr_inputs_.aimPoseAction, aimPosePath[RIGHT]},
                                                                            {xr_inputs_.menuClickAction, menuClickPath[LEFT]},
                                                                            {xr_inputs_.thumbstickClickAction, stickClickPath[LEFT]},
                                                                            {xr_inputs_.thumbstickClickAction, stickClickPath[RIGHT]},
                                                                            {xr_inputs_.thumbstickTouchAction, stickTouchPath[LEFT]},
                                                                            {xr_inputs_.thumbstickTouchAction, stickTouchPath[RIGHT]},
                                                                            {xr_inputs_.thumbstickXAction, stickXPath[LEFT]},
                                                                            {xr_inputs_.thumbstickXAction, stickXPath[RIGHT]},
                                                                            {xr_inputs_.thumbstickYAction, stickYPath[LEFT]},
                                                                            {xr_inputs_.thumbstickYAction, stickYPath[RIGHT]},
                                                                            {xr_inputs_.thumbRestTouchAction, thumbRestTouchPath[LEFT]},
                                                                            {xr_inputs_.thumbRestTouchAction, thumbRestTouchPath[RIGHT]},
                                                                            {xr_inputs_.thumbRestForceAction, thumbRestForcePath[LEFT]},
                                                                            {xr_inputs_.thumbRestForceAction, thumbRestForcePath[RIGHT]},
                                                                            {xr_inputs_.thumbProximityAction, thumbProximityPath[LEFT]},
                                                                            {xr_inputs_.thumbProximityAction, thumbProximityPath[RIGHT]},
                                                                            //{xr_inputs_.pinchValueAction, pinchValuePath[LEFT]},
                                                                            //{xr_inputs_.pinchValueAction, pinchValuePath[RIGHT]},
                                                                            //{xr_inputs_.pinchForceAction, pinchForcePath[LEFT]},
                                                                            //{xr_inputs_.pinchForceAction, pinchForcePath[RIGHT]},-
                                                                            //{xr_inputs_.thumbRestClickAction, thumbRestClickPath[LEFT]},
                                                                            //{xr_inputs_.thumbRestClickAction, thumbRestClickPath[RIGHT]},
                                                                            //{xr_inputs_.trackpadXAction, trackPad_X_Path[LEFT]},
                                                                            //{xr_inputs_.trackpadXAction, trackPad_X_Path[RIGHT]},
                                                                            //{xr_inputs_.trackpadYAction, trackPad_Y_Path[LEFT]},
                                                                            //{xr_inputs_.trackpadYAction, trackPad_Y_Path[RIGHT]},
                                                                            {xr_inputs_.buttonAXClickAction, XA_ClickPath[LEFT]},
                                                                            {xr_inputs_.buttonAXClickAction, XA_ClickPath[RIGHT]},
                                                                            {xr_inputs_.buttonAXTouchAction, XA_TouchPath[LEFT]},
                                                                            {xr_inputs_.buttonAXTouchAction, XA_TouchPath[RIGHT]},
                                                                            {xr_inputs_.buttonBYClickAction, YB_ClickPath[LEFT]},
                                                                            {xr_inputs_.buttonBYClickAction, YB_ClickPath[RIGHT]},
                                                                            {xr_inputs_.buttonBYTouchAction, YB_TouchPath[LEFT]},
                                                                            {xr_inputs_.buttonBYTouchAction, YB_TouchPath[RIGHT]},
                                                                            {xr_inputs_.vibrateAction, hapticPath[LEFT]},
                                                                            {xr_inputs_.vibrateAction, hapticPath[RIGHT]}}};

        XrInteractionProfileSuggestedBinding suggestedBindings{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
        suggestedBindings.interactionProfile = oculusTouchProInteractionProfilePath;
        suggestedBindings.suggestedBindings = oculus_touch_pro_bindings.data();
        suggestedBindings.countSuggestedBindings = (uint32_t)oculus_touch_pro_bindings.size();
        XR_CHECK(xrSuggestInteractionProfileBindings(instance_, &suggestedBindings));
    }

    if (htcViveFocus3ControllersSupported_)
    {
        XrPath htcInteractionProfilePath;
        xrStringToPath(instance_, "/interaction_profiles/htc/vive_focus3_controller", &htcInteractionProfilePath);

        std::vector<XrActionSuggestedBinding> htc_vive_focus3_bindings{{
                                                                                {xr_inputs_.triggerClickAction, triggerValuePath[LEFT]},
                                                                                {xr_inputs_.triggerClickAction, triggerValuePath[RIGHT]},
                                                                                {xr_inputs_.triggerTouchAction, triggerTouchPath[LEFT]},
                                                                                {xr_inputs_.triggerTouchAction, triggerTouchPath[RIGHT]},
                                                                                {xr_inputs_.triggerValueAction, triggerValuePath[LEFT]},
                                                                                {xr_inputs_.triggerValueAction, triggerValuePath[RIGHT]},
                                                                                {xr_inputs_.squeezeClickAction, squeezeClickPath[LEFT]},
                                                                                {xr_inputs_.squeezeClickAction, squeezeClickPath[RIGHT]},
                                                                                {xr_inputs_.squeezeTouchAction, squeezeTouchPath[LEFT]},
                                                                                {xr_inputs_.squeezeTouchAction, squeezeTouchPath[RIGHT]},
                                                                                {xr_inputs_.squeezeValueAction, squeezeValuePath[LEFT]},
                                                                                {xr_inputs_.squeezeValueAction, squeezeValuePath[RIGHT]},
                                                                                {xr_inputs_.gripPoseAction, gripPosePath[LEFT]},
                                                                                {xr_inputs_.gripPoseAction, gripPosePath[RIGHT]},
                                                                                {xr_inputs_.aimPoseAction, aimPosePath[LEFT]},
                                                                                {xr_inputs_.aimPoseAction, aimPosePath[RIGHT]},
                                                                                {xr_inputs_.menuClickAction, menuClickPath[LEFT]},
                                                                                {xr_inputs_.thumbstickClickAction, stickClickPath[LEFT]},
                                                                                {xr_inputs_.thumbstickClickAction, stickClickPath[RIGHT]},
                                                                                {xr_inputs_.thumbstickTouchAction, stickTouchPath[LEFT]},
                                                                                {xr_inputs_.thumbstickTouchAction, stickTouchPath[RIGHT]},
                                                                                {xr_inputs_.thumbstickXAction, stickXPath[LEFT]},
                                                                                {xr_inputs_.thumbstickXAction, stickXPath[RIGHT]},
                                                                                {xr_inputs_.thumbstickYAction, stickYPath[LEFT]},
                                                                                {xr_inputs_.thumbstickYAction, stickYPath[RIGHT]},
                                                                                {xr_inputs_.thumbRestTouchAction, thumbRestTouchPath[LEFT]},
                                                                                {xr_inputs_.thumbRestTouchAction, thumbRestTouchPath[RIGHT]},
                                                                                {xr_inputs_.buttonAXClickAction, XA_ClickPath[LEFT]},
                                                                                {xr_inputs_.buttonAXClickAction, XA_ClickPath[RIGHT]},
                                                                                //{xr_inputs_.buttonAXTouchAction, XA_TouchPath[LEFT]},
                                                                                //{xr_inputs_.buttonAXTouchAction, XA_TouchPath[RIGHT]},
                                                                                {xr_inputs_.buttonBYClickAction, YB_ClickPath[LEFT]},
                                                                                {xr_inputs_.buttonBYClickAction, YB_ClickPath[RIGHT]},
                                                                                //{xr_inputs_.buttonBYTouchAction, YB_TouchPath[LEFT]},
                                                                                //{xr_inputs_.buttonBYTouchAction, YB_TouchPath[RIGHT]}, // Are these supported on HTC? Not sure
                                                                                {xr_inputs_.vibrateAction, hapticPath[LEFT]},
                                                                                {xr_inputs_.vibrateAction, hapticPath[RIGHT]}}};

        XrInteractionProfileSuggestedBinding suggestedBindings{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
        suggestedBindings.interactionProfile = htcInteractionProfilePath;
        suggestedBindings.suggestedBindings = htc_vive_focus3_bindings.data();
        suggestedBindings.countSuggestedBindings = (uint32_t)htc_vive_focus3_bindings.size();
        XR_CHECK(xrSuggestInteractionProfileBindings(instance_, &suggestedBindings));
    }

    if (byteDanceControllersSupported_)
    {
        const bool is_pico_3 = ((headsetType_ == HeadsetType::PICO_NEO_3_) || (headsetType_ == HeadsetType::PICO_NEO_3_EYE_));
        const bool is_pico_4 = ((headsetType_ == HeadsetType::PICO_NEO_4_) || (headsetType_ == HeadsetType::PICO_NEO_4_EYE_));

        if (is_pico_3)
        {
            XrPath pico_neo3_interaction_profile_path;
            xrStringToPath(instance_, "/interaction_profiles/pico/neo3_controller", &pico_neo3_interaction_profile_path);

            std::vector<XrActionSuggestedBinding> pico_neo3_bindings{{
                                                                             {xr_inputs_.triggerClickAction, triggerClickPath[LEFT]},
                                                                             {xr_inputs_.triggerClickAction, triggerClickPath[RIGHT]},
                                                                             {xr_inputs_.triggerTouchAction, triggerTouchPath[LEFT]},
                                                                             {xr_inputs_.triggerTouchAction, triggerTouchPath[RIGHT]},
                                                                             {xr_inputs_.triggerValueAction, triggerValuePath[LEFT]},
                                                                             {xr_inputs_.triggerValueAction, triggerValuePath[RIGHT]},
                                                                             {xr_inputs_.squeezeClickAction, squeezeClickPath[LEFT]},
                                                                             {xr_inputs_.squeezeClickAction, squeezeClickPath[RIGHT]},
                                                                             {xr_inputs_.squeezeTouchAction, squeezeTouchPath[LEFT]},
                                                                             {xr_inputs_.squeezeTouchAction, squeezeTouchPath[RIGHT]},
                                                                             {xr_inputs_.squeezeValueAction, squeezeValuePath[LEFT]},
                                                                             {xr_inputs_.squeezeValueAction, squeezeValuePath[RIGHT]},
                                                                             {xr_inputs_.gripPoseAction, gripPosePath[LEFT]},
                                                                             {xr_inputs_.gripPoseAction, gripPosePath[RIGHT]},
                                                                             {xr_inputs_.aimPoseAction, aimPosePath[LEFT]},
                                                                             {xr_inputs_.aimPoseAction, aimPosePath[RIGHT]},
                                                                             {xr_inputs_.menuClickAction, menuClickPath[LEFT]},
                                                                             {xr_inputs_.menuClickAction, menuClickPath[RIGHT]}, // Pico 3 is the only one that allows access to System menu button events
                                                                             {xr_inputs_.thumbstickClickAction, stickClickPath[LEFT]},
                                                                             {xr_inputs_.thumbstickClickAction, stickClickPath[RIGHT]},
                                                                             {xr_inputs_.thumbstickTouchAction, stickTouchPath[LEFT]},
                                                                             {xr_inputs_.thumbstickTouchAction, stickTouchPath[RIGHT]},
                                                                             {xr_inputs_.thumbstickXAction, stickXPath[LEFT]},
                                                                             {xr_inputs_.thumbstickXAction, stickXPath[RIGHT]},
                                                                             {xr_inputs_.thumbstickYAction, stickYPath[LEFT]},
                                                                             {xr_inputs_.thumbstickYAction, stickYPath[RIGHT]},
                                                                             {xr_inputs_.buttonAXClickAction, XA_ClickPath[LEFT]},
                                                                             {xr_inputs_.buttonAXClickAction, XA_ClickPath[RIGHT]},
                                                                             //{xr_inputs_.buttonAXTouchAction, XA_TouchPath[LEFT]},
                                                                             //{xr_inputs_.buttonAXTouchAction, XA_TouchPath[RIGHT]},
                                                                             {xr_inputs_.buttonBYClickAction, YB_ClickPath[LEFT]},
                                                                             {xr_inputs_.buttonBYClickAction, YB_ClickPath[RIGHT]},
                                                                             //{xr_inputs_.buttonBYTouchAction, YB_TouchPath[LEFT]},
                                                                             //{xr_inputs_.buttonBYTouchAction, YB_TouchPath[RIGHT]},
                                                                             {xr_inputs_.vibrateAction, hapticPath[LEFT]},
                                                                             {xr_inputs_.vibrateAction, hapticPath[RIGHT]}}};

            XrInteractionProfileSuggestedBinding suggestedBindings{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
            suggestedBindings.interactionProfile = pico_neo3_interaction_profile_path;
            suggestedBindings.suggestedBindings = pico_neo3_bindings.data();
            suggestedBindings.countSuggestedBindings = (uint32_t)pico_neo3_bindings.size();
            XR_CHECK(xrSuggestInteractionProfileBindings(instance_, &suggestedBindings));
        }
        else if (is_pico_4)
        {
            XrPath pico_neo4_interaction_profile_path;
            xrStringToPath(instance_, "/interaction_profiles/bytedance/pico4_controller", &pico_neo4_interaction_profile_path);

            std::vector<XrActionSuggestedBinding> pico_neo4_bindings{{
                                                                             {xr_inputs_.triggerClickAction, triggerClickPath[LEFT]},
                                                                             {xr_inputs_.triggerClickAction, triggerClickPath[RIGHT]},
                                                                             {xr_inputs_.triggerTouchAction, triggerTouchPath[LEFT]},
                                                                             {xr_inputs_.triggerTouchAction, triggerTouchPath[RIGHT]},
                                                                             {xr_inputs_.triggerValueAction, triggerValuePath[LEFT]},
                                                                             {xr_inputs_.triggerValueAction, triggerValuePath[RIGHT]},
                                                                             {xr_inputs_.squeezeClickAction, squeezeClickPath[LEFT]},
                                                                             {xr_inputs_.squeezeClickAction, squeezeClickPath[RIGHT]},
                                                                             {xr_inputs_.squeezeTouchAction, squeezeTouchPath[LEFT]},
                                                                             {xr_inputs_.squeezeTouchAction, squeezeTouchPath[RIGHT]},
                                                                             {xr_inputs_.squeezeValueAction, squeezeValuePath[LEFT]},
                                                                             {xr_inputs_.squeezeValueAction, squeezeValuePath[RIGHT]},
                                                                             {xr_inputs_.gripPoseAction, gripPosePath[LEFT]},
                                                                             {xr_inputs_.gripPoseAction, gripPosePath[RIGHT]},
                                                                             {xr_inputs_.aimPoseAction, aimPosePath[LEFT]},
                                                                             {xr_inputs_.aimPoseAction, aimPosePath[RIGHT]},
                                                                             {xr_inputs_.menuClickAction, menuClickPath[LEFT]},
                                                                             {xr_inputs_.thumbstickClickAction, stickClickPath[LEFT]},
                                                                             {xr_inputs_.thumbstickClickAction, stickClickPath[RIGHT]},
                                                                             {xr_inputs_.thumbstickTouchAction, stickTouchPath[LEFT]},
                                                                             {xr_inputs_.thumbstickTouchAction, stickTouchPath[RIGHT]},
                                                                             {xr_inputs_.thumbstickXAction, stickXPath[LEFT]},
                                                                             {xr_inputs_.thumbstickXAction, stickXPath[RIGHT]},
                                                                             {xr_inputs_.thumbstickYAction, stickYPath[LEFT]},
                                                                             {xr_inputs_.thumbstickYAction, stickYPath[RIGHT]},
                                                                             {xr_inputs_.buttonAXClickAction, XA_ClickPath[LEFT]},
                                                                             {xr_inputs_.buttonAXClickAction, XA_ClickPath[RIGHT]},
                                                                             //{xr_inputs_.buttonAXTouchAction, XA_TouchPath[LEFT]},
                                                                             //{xr_inputs_.buttonAXTouchAction, XA_TouchPath[RIGHT]},
                                                                             {xr_inputs_.buttonBYClickAction, YB_ClickPath[LEFT]},
                                                                             {xr_inputs_.buttonBYClickAction, YB_ClickPath[RIGHT]},
                                                                             //{xr_inputs_.buttonBYTouchAction, YB_TouchPath[LEFT]},
                                                                             //{xr_inputs_.buttonBYTouchAction, YB_TouchPath[RIGHT]},
                                                                             {xr_inputs_.vibrateAction, hapticPath[LEFT]},
                                                                             {xr_inputs_.vibrateAction, hapticPath[RIGHT]}}};

            XrInteractionProfileSuggestedBinding suggestedBindings{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
            suggestedBindings.interactionProfile = pico_neo4_interaction_profile_path;
            suggestedBindings.suggestedBindings = pico_neo4_bindings.data();
            suggestedBindings.countSuggestedBindings = (uint32_t)pico_neo4_bindings.size();
            XR_CHECK(xrSuggestInteractionProfileBindings(instance_, &suggestedBindings));
        }


    }

    XrActionSpaceCreateInfo actionSpaceInfo{XR_TYPE_ACTION_SPACE_CREATE_INFO};
    actionSpaceInfo.action = xr_inputs_.gripPoseAction;
    actionSpaceInfo.poseInActionSpace.orientation.w = 1.f;
    actionSpaceInfo.subactionPath = xr_inputs_.handSubactionPath[LEFT];
    XR_CHECK(xrCreateActionSpace(session_, &actionSpaceInfo, &xr_inputs_.gripSpace[LEFT]));

    actionSpaceInfo.subactionPath = xr_inputs_.handSubactionPath[RIGHT];
    XR_CHECK(xrCreateActionSpace(session_, &actionSpaceInfo, &xr_inputs_.gripSpace[RIGHT]));

    actionSpaceInfo.action = xr_inputs_.aimPoseAction;
    actionSpaceInfo.subactionPath = xr_inputs_.handSubactionPath[LEFT];
    xrCreateActionSpace(session_, &actionSpaceInfo, &xr_inputs_.aimSpace[LEFT]);

    actionSpaceInfo.subactionPath = xr_inputs_.handSubactionPath[RIGHT];
    XR_CHECK(xrCreateActionSpace(session_, &actionSpaceInfo, &xr_inputs_.aimSpace[RIGHT]));

    XrSessionActionSetsAttachInfo attachInfo{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
    attachInfo.countActionSets = 1;
    attachInfo.actionSets = &xr_inputs_.actionSet;
    XR_CHECK(xrAttachSessionActionSets(session_, &attachInfo));
}

void XrApp::handleXrEvents() {
  XrEventDataBuffer eventDataBuffer = {};

  // Poll for events
  for (;;) {
    XrEventDataBaseHeader* baseEventHeader = (XrEventDataBaseHeader*)(&eventDataBuffer);
    baseEventHeader->type = XR_TYPE_EVENT_DATA_BUFFER;
    baseEventHeader->next = nullptr;
    XrResult res;
    XR_CHECK(res = xrPollEvent(instance_, &eventDataBuffer));
    if (res != XR_SUCCESS) {
      break;
    }

    switch (baseEventHeader->type) {
    case XR_TYPE_EVENT_DATA_EVENTS_LOST:
      IGL_LOG_INFO("xrPollEvent: received XR_TYPE_EVENT_DATA_EVENTS_LOST event");
      break;
    case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING:
      IGL_LOG_INFO("xrPollEvent: received XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING event");
      break;
    case XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED:  {

      IGL_LOG_INFO("xrPollEvent: received XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED event");

        XrInteractionProfileState profile_state = {XR_TYPE_INTERACTION_PROFILE_STATE};
        res = xrGetCurrentInteractionProfile(session_, xr_inputs_.handSubactionPath[LEFT], &profile_state);

        if (res == XR_SUCCESS) {
            XrPath profile_path = profile_state.interactionProfile;

            uint32_t length = 0;
            char profile_str[XR_MAX_PATH_LENGTH] = {};

            res = xrPathToString(instance_, profile_path, XR_MAX_PATH_LENGTH, &length, profile_str);

            if (res == XR_SUCCESS) {
                std::string profile = profile_str;

                if (profile == "/interaction_profiles/facebook/touch_controller_pro") {
                    IGL_LOG_INFO("Using Touch Pro controllers");
                }
            }
        }
      break;
    }
    case XR_TYPE_EVENT_DATA_PERF_SETTINGS_EXT: {
      const XrEventDataPerfSettingsEXT* perf_settings_event =
          (XrEventDataPerfSettingsEXT*)(baseEventHeader);
      (void)perf_settings_event; // suppress unused warning
      IGL_LOG_INFO(
          "xrPollEvent: received XR_TYPE_EVENT_DATA_PERF_SETTINGS_EXT event: type %d subdomain %d "
          ": level %d -> level %d",
          perf_settings_event->type,
          perf_settings_event->subDomain,
          perf_settings_event->fromLevel,
          perf_settings_event->toLevel);
    } break;
    case XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING:
      IGL_LOG_INFO("xrPollEvent: received XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING event");
      break;
    case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED: {
      const XrEventDataSessionStateChanged* session_state_changed_event =
          (XrEventDataSessionStateChanged*)(baseEventHeader);
      IGL_LOG_INFO(
          "xrPollEvent: received XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED: %d for session %p at "
          "time %lld",
          session_state_changed_event->state,
          (void*)session_state_changed_event->session,
          session_state_changed_event->time);

      switch (session_state_changed_event->state) {
      case XR_SESSION_STATE_READY:
      case XR_SESSION_STATE_STOPPING:
        handleSessionStateChanges(session_state_changed_event->state);
        break;
      default:
        break;
      }
    } break;
    default:
      IGL_LOG_INFO("xrPollEvent: Unknown event");
      break;
    }
  }
}

void XrApp::handleSessionStateChanges(XrSessionState state) {
  if (state == XR_SESSION_STATE_READY) {
#if !defined(IGL_CMAKE_BUILD)
    assert(resumed_);
#endif // IGL_CMAKE_BUILD
    assert(sessionActive_ == false);

    XrSessionBeginInfo sessionBeginInfo{
        XR_TYPE_SESSION_BEGIN_INFO,
        nullptr,
        viewConfigProps_.viewConfigurationType,
    };

    XrResult result;
    XR_CHECK(result = xrBeginSession(session_, &sessionBeginInfo));

    sessionActive_ = (result == XR_SUCCESS);
    IGL_LOG_INFO("XR session active");
  } else if (state == XR_SESSION_STATE_STOPPING) {
    assert(resumed_ == false);
    assert(sessionActive_);
    XR_CHECK(xrEndSession(session_));
    sessionActive_ = false;
    IGL_LOG_INFO("XR session inactive");
  }
}

XrFrameState XrApp::beginFrame() {
  const auto& appParams = renderSession_->appParams();
  if (appParams.quadLayerParamsGetter) {
    quadLayersParams_ = appParams.quadLayerParamsGetter();
    numQuadLayersPerView_ = quadLayersParams_.numQuads() > 0 ? quadLayersParams_.numQuads() : 1;
  } else {
    quadLayersParams_ = {};
    numQuadLayersPerView_ = 1;
  }
  updateSwapchainProviders();

  XrFrameWaitInfo waitFrameInfo = {XR_TYPE_FRAME_WAIT_INFO};

  XrFrameState frameState = {XR_TYPE_FRAME_STATE};

  XR_CHECK(xrWaitFrame(session_, &waitFrameInfo, &frameState));

  XrFrameBeginInfo beginFrameInfo = {XR_TYPE_FRAME_BEGIN_INFO};

  XR_CHECK(xrBeginFrame(session_, &beginFrameInfo));

  XrSpaceLocation loc = {
      loc.type = XR_TYPE_SPACE_LOCATION,
  };
  XR_CHECK(xrLocateSpace(headSpace_, currentSpace_, frameState.predictedDisplayTime, &loc));
  headPose_ = loc.pose;
  headPoseTime_ = frameState.predictedDisplayTime;

  XrViewState viewState = {XR_TYPE_VIEW_STATE};

  XrViewLocateInfo projectionInfo = {
      XR_TYPE_VIEW_LOCATE_INFO,
      nullptr,
      viewConfigProps_.viewConfigurationType,
      frameState.predictedDisplayTime,
      headSpace_,
  };

  uint32_t numViews = views_.size();

  XR_CHECK(xrLocateViews(
      session_, &projectionInfo, &viewState, views_.size(), &numViews, views_.data()));

  for (size_t i = 0; i < kNumViews; i++) {

#ifndef EXTERNAL_XR_BUILD
    XrPosef eyePose = views_[i].pose;
    XrPosef_Multiply(&viewStagePoses_[i], &headPose_, &eyePose);
    XrPosef viewTransformXrPosef{};
    XrPosef_Invert(&viewTransformXrPosef, &viewStagePoses_[i]);
    XrMatrix4x4f xrMat4{};
    XrMatrix4x4f_CreateFromRigidTransform(&xrMat4, &viewTransformXrPosef);
    viewTransforms_[i] = glm::make_mat4(xrMat4.m);
    cameraPositions_[i] = glm::vec3(eyePose.position.x, eyePose.position.y, eyePose.position.z);
#endif
  }

  if (handsTrackingSupported_) {
    updateHandTracking();
  }

  return frameState;
}

namespace {
void copyFov(igl::shell::Fov& dst, const XrFovf& src) {
  dst.angleLeft = src.angleLeft;
  dst.angleRight = src.angleRight;
  dst.angleUp = src.angleUp;
  dst.angleDown = src.angleDown;
}
} // namespace

void XrApp::render() {
  if (useQuadLayerComposition_) {
    shellParams_->clearColorValue = igl::Color{0.0f, 0.0f, 0.0f, 0.0f};
  } else {
    shellParams_->clearColorValue.reset();
  }

  shellParams_->xr_app_ptr_ = this;

  if (!renderSession_->pre_update()) {
    return;
  }

  if (useSinglePassStereo_) {
    for (size_t quadLayer = 0; quadLayer < numQuadLayersPerView_; quadLayer++) {
      auto surfaceTextures = swapchainProviders_[quadLayer]->getSurfaceTextures();
      for (size_t j = 0; j < shellParams_->viewParams.size(); j++) {
        shellParams_->viewParams[j].viewMatrix = viewTransforms_[j];
        shellParams_->viewParams[j].cameraPosition = cameraPositions_[j];
        copyFov(shellParams_->viewParams[j].fov, views_[j].fov);
      }
      if (useQuadLayerComposition_) {
        renderSession_->setCurrentQuadLayer(quadLayer);
      }
      renderSession_->update(std::move(surfaceTextures));
      swapchainProviders_[quadLayer]->releaseSwapchainImages();
    }

  } else {
    const uint32_t numSwapChains = numQuadLayersPerView_ * kNumViews;
    for (uint32_t swapChainIndex = 0; swapChainIndex < numSwapChains; swapChainIndex++) {
      const uint32_t view = swapChainIndex % kNumViews;
      shellParams_->viewParams[0].viewMatrix = viewTransforms_[view];
      copyFov(shellParams_->viewParams[0].fov, views_[view].fov);
      auto surfaceTextures = swapchainProviders_[swapChainIndex]->getSurfaceTextures();
      if (useQuadLayerComposition_) {
        const uint32_t quadLayerIndexPerView = swapChainIndex / kNumViews;
        renderSession_->setCurrentQuadLayer(quadLayerIndexPerView);
      }
#if ENABLE_CLOUDXR
        shellParams_->viewParams[0].cameraPosition = cameraPositions_[view];
        shellParams_->current_view_id_ = view;
#endif
      renderSession_->update(surfaceTextures);
      swapchainProviders_[swapChainIndex]->releaseSwapchainImages();
    }
  }

    renderSession_->post_update();
}

void XrApp::setupProjectionAndDepth(std::vector<XrCompositionLayerProjectionView>& projectionViews,
                                    std::vector<XrCompositionLayerDepthInfoKHR>& depthInfos) {
  const auto& appParams = renderSession_->appParams();
  auto numQuadLayers = kNumViews * numQuadLayersPerView_;
  projectionViews.resize(numQuadLayers);
  depthInfos.resize(numQuadLayers);

  size_t layer = 0;
  for (size_t i = 0; i < numQuadLayersPerView_; i++) {
    for (size_t view = 0; view < kNumViews; view++, layer++) {
      depthInfos[layer] = {
          XR_TYPE_COMPOSITION_LAYER_DEPTH_INFO_KHR,
          nullptr,
      };
      projectionViews[layer] = {
          XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW,
          &depthInfos[layer],
          viewStagePoses_[view],
          views_[view].fov,
      };
      const XrRect2Di imageRect = {{0, 0},
                                   {
                                       (int32_t)viewports_[view].recommendedImageRectWidth,
                                       (int32_t)viewports_[view].recommendedImageRectHeight,
                                   }};
      auto swapChainIndex = useSinglePassStereo_ ? i : layer;
      auto subImageIndex = useSinglePassStereo_ ? static_cast<uint32_t>(view) : 0;
      projectionViews[layer].subImage = {
          swapchainProviders_[swapChainIndex]->colorSwapchain(),
          imageRect,
          subImageIndex,
      };
      depthInfos[layer].subImage = {
          swapchainProviders_[swapChainIndex]->colorSwapchain(),
          imageRect,
          subImageIndex,
      };
      depthInfos[layer].minDepth = appParams.depthParams.minDepth;
      depthInfos[layer].maxDepth = appParams.depthParams.maxDepth;
      depthInfos[layer].nearZ = appParams.depthParams.nearZ;
      depthInfos[layer].farZ = appParams.depthParams.farZ;
	  
#if ENABLE_CLOUDXR
      if (should_override_eye_poses_)
      {
        projectionViews[layer].pose = override_eye_poses_[layer];
      }
#endif
    }
  }
}

void XrApp::endFrameQuadLayerComposition(XrFrameState frameState) {
  const auto& appParams = renderSession_->appParams();

  std::vector<XrCompositionLayerQuad> quadLayers(static_cast<size_t>(kNumViews) *
                                                 numQuadLayersPerView_);
  XrVector3f position = {0.f, 0.f, 0.f};
#if USE_LOCAL_AR_SPACE
  position.z = -1.f;
#endif
  XrExtent2Df size = {appParams.sizeX, appParams.sizeY};
  size_t layer = 0;
  for (size_t i = 0; i < numQuadLayersPerView_; i++) {
    if (useQuadLayerComposition_ && quadLayersParams_.numQuads() > 0) {
      IGL_ASSERT(i < quadLayersParams_.positions.size());
      IGL_ASSERT(i < quadLayersParams_.sizes.size());
      auto glmPos = quadLayersParams_.positions[i];
      auto glmSize = quadLayersParams_.sizes[i];
      position = {glmPos.x, glmPos.y, glmPos.z};
      size = {glmSize.x, glmSize.y};
#if USE_LOCAL_AR_SPACE
      position.z = -1.f;
#endif
    }
    XrEyeVisibility eye = XR_EYE_VISIBILITY_LEFT;
    for (size_t view = 0; view < kNumViews; view++, layer++) {
      quadLayers[layer].next = nullptr;
      quadLayers[layer].type = XR_TYPE_COMPOSITION_LAYER_QUAD;
      quadLayers[layer].layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
      quadLayers[layer].space = currentSpace_;
      quadLayers[layer].eyeVisibility = eye;
      memset(&quadLayers[layer].subImage, 0, sizeof(XrSwapchainSubImage));
      quadLayers[layer].pose = {{0.f, 0.f, 0.f, 1.f}, position};
      quadLayers[layer].size = size;
      if (eye == XR_EYE_VISIBILITY_LEFT) {
        eye = XR_EYE_VISIBILITY_RIGHT;
      }
    }
  }

  std::vector<XrCompositionLayerProjectionView> projectionViews;
  std::vector<XrCompositionLayerDepthInfoKHR> depthInfos;
  setupProjectionAndDepth(projectionViews, depthInfos);

  IGL_ASSERT(quadLayers.size() == projectionViews.size());
  for (size_t i = 0; i < quadLayers.size(); i++) {
    quadLayers[i].subImage = projectionViews[i].subImage;
  }

  std::vector<const XrCompositionLayerBaseHeader*> layers(numQuadLayersPerView_ *
                                                          static_cast<std::size_t>(kNumViews + 1));
  uint32_t layerIndex = 0;
  XrCompositionLayerPassthroughFB compositionLayer{XR_TYPE_COMPOSITION_LAYER_PASSTHROUGH_FB};
  if (passthroughSupported_) {
    compositionLayer.next = nullptr;
    compositionLayer.layerHandle = passthrougLayer_;
    layers[layerIndex++] = (const XrCompositionLayerBaseHeader*)&compositionLayer;
  }

  for (auto& quadLayer : quadLayers) {
    IGL_ASSERT(layerIndex < layers.size());
    layers[layerIndex++] = (const XrCompositionLayerBaseHeader*)&quadLayer;
  }

  const XrFrameEndInfo endFrameInfo = {XR_TYPE_FRAME_END_INFO,
                                       nullptr,
                                       frameState.predictedDisplayTime,
                                       additiveBlendingSupported_
                                           ? XR_ENVIRONMENT_BLEND_MODE_ADDITIVE
                                           : XR_ENVIRONMENT_BLEND_MODE_OPAQUE,
                                       layerIndex,
                                       layers.data()};
  XR_CHECK(xrEndFrame(session_, &endFrameInfo));
}

void XrApp::endFrame(XrFrameState frameState) {
  if (useQuadLayerComposition_) {
    endFrameQuadLayerComposition(frameState);
    return;
  }

  std::vector<XrCompositionLayerProjectionView> projectionViews;
  std::vector<XrCompositionLayerDepthInfoKHR> depthInfos;
  setupProjectionAndDepth(projectionViews, depthInfos);

  XrCompositionLayerProjection projection = {
      XR_TYPE_COMPOSITION_LAYER_PROJECTION,
      nullptr,
      XR_COMPOSITION_LAYER_CORRECT_CHROMATIC_ABERRATION_BIT,
      currentSpace_,
      static_cast<uint32_t>(kNumViews),
      projectionViews.data(),
  };

  const XrCompositionLayerBaseHeader* const layers[] = {
      (const XrCompositionLayerBaseHeader*)&projection,
  };
  const XrFrameEndInfo endFrameInfo = {XR_TYPE_FRAME_END_INFO,
                                       nullptr,
                                       frameState.predictedDisplayTime,
                                       additiveBlendingSupported_
                                           ? XR_ENVIRONMENT_BLEND_MODE_ADDITIVE
                                           : XR_ENVIRONMENT_BLEND_MODE_OPAQUE,
                                       1,
                                       layers};
  XR_CHECK(xrEndFrame(session_, &endFrameInfo));
}

void XrApp::update() {
  if (!initialized_ || !resumed_ || !sessionActive_) {
    return;
  }

  auto frameState = beginFrame();
  pollActions(true);
  render();
  endFrame(frameState);
}

void XrApp::pollActions(const bool mainThread) {
    if (!initialized_ || !resumed_ || !sessionActive_) {
        return;
    }

    if (mainThread && !enableMainThreadPolling_) {
        return;
    }
    else if (!mainThread && !enableAsyncPolling_) {
        return;
    }

    xr_inputs_.handActive = {XR_FALSE, XR_FALSE};

    const XrActiveActionSet activeActionSet{xr_inputs_.actionSet, XR_NULL_PATH};
    XrActionsSyncInfo syncInfo{XR_TYPE_ACTIONS_SYNC_INFO};
    syncInfo.countActiveActionSets = 1;
    syncInfo.activeActionSets = &activeActionSet;
    XR_CHECK(xrSyncActions(session_, &syncInfo));

    for (int controller_id = LEFT; controller_id < NUM_SIDES; controller_id++)
    {
        XrActionStateGetInfo getInfo{XR_TYPE_ACTION_STATE_GET_INFO};
        getInfo.subactionPath = xr_inputs_.handSubactionPath[controller_id];
        getInfo.action = xr_inputs_.gripPoseAction;
        XrActionStatePose poseState{XR_TYPE_ACTION_STATE_POSE};
        XR_CHECK(xrGetActionStatePose(session_, &getInfo, &poseState));
        xr_inputs_.handActive[controller_id] = poseState.isActive;
    }
}

XrTime XrApp::get_predicted_display_time_ns()
{
    struct timespec now_ts = {0};
    clock_gettime(CLOCK_MONOTONIC, &now_ts);

    //if (instance_ && !xrConvertTimespecTimeToTimeKHR_) {
//        XR_LOAD(instance_, xrConvertTimespecTimeToTimeKHR_);
//    }

    XrTime now_time = 0;

//    if (xrConvertTimespecTimeToTimeKHR_) {
//        xrConvertTimespecTimeToTimeKHR_(instance_, &now_ts, &now_time);
//    }
//    else{
        now_time = ((uint64_t)(now_ts.tv_sec * 1e9) + now_ts.tv_nsec);
  //  }

    return now_time;
}

float XrApp::getCurrentRefreshRate() {
  if (!session_ || !refreshRateExtensionSupported_ || (currentRefreshRate_ > 0.0f)) {
    return currentRefreshRate_;
  }

  queryCurrentRefreshRate();
  return currentRefreshRate_;
}

void XrApp::queryCurrentRefreshRate() {
  const XrResult result = xrGetDisplayRefreshRateFB_(session_, &currentRefreshRate_);
  if (result == XR_SUCCESS) {
    IGL_LOG_INFO("getCurrentRefreshRate success, current Hz = %.2f.", currentRefreshRate_);
  }
}

float XrApp::getMaxRefreshRate() {
  if (!session_ || !refreshRateExtensionSupported_) {
    return 0.0f;
  }

  const std::vector<float>& supportedRefreshRates = getSupportedRefreshRates();

  if (supportedRefreshRates.empty()) {
    return 0.0f;
  }

  const float maxRefreshRate = supportedRefreshRates.back();
  IGL_LOG_INFO("getMaxRefreshRate Hz = %.2f.", maxRefreshRate);
  return maxRefreshRate;
}

bool XrApp::setRefreshRate(float refreshRate) {
  if (!session_ || !refreshRateExtensionSupported_ || (refreshRate == currentRefreshRate_) ||
      !isRefreshRateSupported(refreshRate)) {
    return false;
  }

  const XrResult result = xrRequestDisplayRefreshRateFB_(session_, refreshRate);
  if (result != XR_SUCCESS) {
    return false;
  }

  IGL_LOG_INFO(
      "setRefreshRate SUCCESS, changed from %.2f Hz to %.2f Hz", currentRefreshRate_, refreshRate);
  currentRefreshRate_ = refreshRate;

  return true;
}

void XrApp::setMaxRefreshRate() {
  if (!session_ || !refreshRateExtensionSupported_) {
    return;
  }

  const float maxRefreshRate = getMaxRefreshRate();

  if (maxRefreshRate > 0.0f) {
    setRefreshRate(maxRefreshRate);
  }
}

bool XrApp::isRefreshRateSupported(float refreshRate) {
  if (!session_ || !refreshRateExtensionSupported_) {
    return false;
  }

  const std::vector<float>& supportedRefreshRates = getSupportedRefreshRates();
  return std::find(supportedRefreshRates.begin(), supportedRefreshRates.end(), refreshRate) !=
         supportedRefreshRates.end();
}

const std::vector<float>& XrApp::getSupportedRefreshRates() {
  if (!session_ || !refreshRateExtensionSupported_) {
    return supportedRefreshRates_;
  }

  if (supportedRefreshRates_.empty()) {
    querySupportedRefreshRates();
  }

  return supportedRefreshRates_;
}

void XrApp::querySupportedRefreshRates() {
  if (!session_ || !refreshRateExtensionSupported_ || !supportedRefreshRates_.empty()) {
    return;
  }

  uint32_t numRefreshRates = 0;
  XrResult result = xrEnumerateDisplayRefreshRatesFB_(session_, 0, &numRefreshRates, nullptr);

  if ((result == XR_SUCCESS) && (numRefreshRates > 0)) {
    supportedRefreshRates_.resize(numRefreshRates);
    result = xrEnumerateDisplayRefreshRatesFB_(
        session_, numRefreshRates, &numRefreshRates, supportedRefreshRates_.data());

    if (result == XR_SUCCESS) {
      std::sort(supportedRefreshRates_.begin(), supportedRefreshRates_.end());
    }

    for (float refreshRate : supportedRefreshRates_) {
      IGL_LOG_INFO("querySupportedRefreshRates Hz = %.2f.", refreshRate);
    }
  }
}

bool XrApp::isSharpeningEnabled() const {
  return compositionLayerSettingsSupported_ &&
  ((compositionLayerSettings_.layerFlags & XR_COMPOSITION_LAYER_SETTINGS_QUALITY_SHARPENING_BIT_FB) != 0);
}

void XrApp::setSharpeningEnabled(const bool enabled) {
  if (!compositionLayerSettingsSupported_ || (enabled == isSharpeningEnabled())) {
    return;
  }
    if (enabled) {
      compositionLayerSettings_.layerFlags |= XR_COMPOSITION_LAYER_SETTINGS_QUALITY_SHARPENING_BIT_FB;
    }
    else {
      compositionLayerSettings_.layerFlags &= ~XR_COMPOSITION_LAYER_SETTINGS_QUALITY_SHARPENING_BIT_FB;
    }
    IGL_LOG_INFO("Link Sharpening is now %s", isSharpeningEnabled() ? "ON" : "OFF");
}

} // namespace igl::shell::openxr
