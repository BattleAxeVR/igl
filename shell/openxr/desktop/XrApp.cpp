/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <igl/Common.h>

#if IGL_BACKEND_VULKAN
#include <igl/vulkan/Common.h>
#endif // IGL_BACKEND_VULKAN

#if IGL_BACKEND_OPENGL
#include <igl/opengl/GLIncludes.h>
#endif // IGL_BACKEND_OPENGL

#include <shell/openxr/XrApp.h>

#include <algorithm>
#include <array>
#include <cassert>
#include <string>

#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include "xr_linear.h"

#include <glm/gtc/type_ptr.hpp>

#if IGL_PLATFORM_APPLE
#include <shell/shared/platform/mac/PlatformMac.h>
#elif IGL_PLATFORM_WIN
#include <shell/shared/platform/win/PlatformWin.h>
#endif

#include <shell/shared/renderSession/AppParams.h>
#include <shell/shared/renderSession/DefaultSession.h>
#include <shell/shared/renderSession/ShellParams.h>

#include <shell/openxr/XrLog.h>
#include <shell/openxr/XrSwapchainProvider.h>
#include <shell/openxr/impl/XrAppImpl.h>
#include <shell/openxr/impl/XrSwapchainProviderImpl.h>

namespace igl::shell::openxr {
constexpr auto kAppName = "IGL Shell OpenXR";
constexpr auto kEngineName = "IGL";
constexpr auto kSupportedViewConfigType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;

XrApp::XrApp(std::unique_ptr<impl::XrAppImpl>&& impl) :
  impl_(std::move(impl)), shellParams_(std::make_unique<ShellParams>()) {
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

  return true;
}

bool XrApp::createInstance() {
  XrApplicationInfo appInfo = {};
  strcpy(appInfo.applicationName, kAppName);
  appInfo.applicationVersion = 0;
  strcpy(appInfo.engineName, kEngineName);
  appInfo.engineVersion = 0;
  appInfo.apiVersion = XR_CURRENT_API_VERSION;

  XrInstanceCreateInfo instanceCreateInfo = {
      .type = XR_TYPE_INSTANCE_CREATE_INFO,
      .next = impl_->getInstanceCreateExtension(),
      .createFlags = 0,
      .applicationInfo = appInfo,
      .enabledApiLayerCount = 0,
      .enabledApiLayerNames = nullptr,
      .enabledExtensionCount = static_cast<uint32_t>(requiredExtensions_.size()),
      .enabledExtensionNames = requiredExtensions_.data(),
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

  XR_CHECK(xrGetSystemProperties(instance_, systemId_, &systemProps_));

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

    XR_CHECK(xrEnumerateViewConfigurationViews(
        instance_, systemId_, viewConfigType, numViewports, &numViewports, viewports_.data()));

    for (auto& view : viewports_) {
      (void)view; // doesn't compile in release for unused variable
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

void XrApp::createSwapchainProviders(const std::unique_ptr<igl::IDevice>& device) {
  const uint32_t numSwapchainProviders = useSinglePassStereo_ ? 1 : kNumViews;
  const uint32_t numViewsPerSwapchain = useSinglePassStereo_ ? kNumViews : 1;
  swapchainProviders_.reserve(numSwapchainProviders);

  for (size_t i = 0; i < numSwapchainProviders; i++) {
    swapchainProviders_.emplace_back(
        std::make_unique<XrSwapchainProvider>(impl_->createSwapchainProviderImpl(),
                                              platform_,
                                              session_,
                                              viewports_[i],
                                              numViewsPerSwapchain));
    swapchainProviders_.back()->initialize();
  }
}

bool XrApp::initialize(const struct android_app* app) {
  if (initialized_) {
    return false;
  }

  if (!checkExtensions()) {
    return false;
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

  createShellSession(std::move(device), nullptr);

  session_ = impl_->initXrSession(instance_, systemId_, platform_->getDevice());
  if (session_ == XR_NULL_HANDLE) {
    IGL_LOG_ERROR("Failed to initialize graphics system");
    return false;
  }

  // The following are initialization steps that happen after XrSession is created.
  enumerateReferenceSpaces();
  createSwapchainProviders(device);
  createSpaces();
  createActions();

  initialized_ = true;

  return initialized_;
}

void XrApp::createShellSession(std::unique_ptr<igl::IDevice> device, AAssetManager* assetMgr) {
#if IGL_PLATFORM_APPLE
  platform_ = std::make_shared<igl::shell::PlatformMac>(std::move(device));
#elif IGL_PLATFORM_WIN
  platform_ = std::make_shared<igl::shell::PlatformWin>(std::move(device));
#endif
  IGL_ASSERT(platform_ != nullptr);
  renderSession_ = igl::shell::createDefaultRenderSession(platform_);
  shellParams_->shellControlsViewParams = true;
  shellParams_->renderMode = useSinglePassStereo_ ? RenderMode::SinglePassStereo
                                                  : RenderMode::DualPassStereo;
  shellParams_->viewParams.resize(useSinglePassStereo_ ? 2 : 1);
  renderSession_->setShellParams(*shellParams_);
  renderSession_->initialize();
}

void XrApp::createSpaces() {
  XrReferenceSpaceCreateInfo spaceCreateInfo = {
      XR_TYPE_REFERENCE_SPACE_CREATE_INFO,
      nullptr,
      XR_REFERENCE_SPACE_TYPE_VIEW,
      {{0.0f, 0.0f, 0.0f, 1.0f}},
  };

#if USE_LOCAL_AR_SPACE
  spaceCreateInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
#else
  spaceCreateInfo.referenceSpaceType = stageSpaceSupported_ ? XR_REFERENCE_SPACE_TYPE_STAGE
                                                            : XR_REFERENCE_SPACE_TYPE_LOCAL;
#endif // USE_LOCAL_AR_SPACE

  XR_CHECK(xrCreateReferenceSpace(session_, &spaceCreateInfo, &currentSpace_));
  XR_CHECK(xrCreateReferenceSpace(session_, &spaceCreateInfo, &headSpace_));
}

void XrApp::createActions() {

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
        strcpy(actionInfo.actionName, "hand_pose");
        strcpy(actionInfo.localizedActionName, "Hand Pose");
        actionInfo.countSubactionPaths = uint32_t(xr_inputs_.handSubactionPath.size());
        actionInfo.subactionPaths = xr_inputs_.handSubactionPath.data();
        XR_CHECK(xrCreateAction(xr_inputs_.actionSet, &actionInfo, &xr_inputs_.poseAction));

        actionInfo.actionType = XR_ACTION_TYPE_POSE_INPUT;
        strcpy(actionInfo.actionName, "aim_pose");
        strcpy(actionInfo.localizedActionName, "Aim Pose");
        actionInfo.countSubactionPaths = uint32_t(xr_inputs_.handSubactionPath.size());
        actionInfo.subactionPaths = xr_inputs_.handSubactionPath.data();
        XR_CHECK(xrCreateAction(xr_inputs_.actionSet, &actionInfo, &xr_inputs_.aimPoseAction));

        actionInfo.actionType = XR_ACTION_TYPE_FLOAT_INPUT;
        strcpy(actionInfo.actionName, "thumbstick_x");
        strcpy(actionInfo.localizedActionName, "Thumbstick X");
        XR_CHECK(xrCreateAction(xr_inputs_.actionSet, &actionInfo, &xr_inputs_.thumbstickXAction));

        actionInfo.actionType = XR_ACTION_TYPE_FLOAT_INPUT;
        strcpy(actionInfo.actionName, "thumbstick_y");
        strcpy(actionInfo.localizedActionName, "Thumbstick Y");
        XR_CHECK(xrCreateAction(xr_inputs_.actionSet, &actionInfo, &xr_inputs_.thumbstickYAction));

        // Create output actions for vibrating the left and right controller.
        actionInfo.actionType = XR_ACTION_TYPE_VIBRATION_OUTPUT;
        strcpy(actionInfo.actionName, "vibrate_hand");
        strcpy(actionInfo.localizedActionName, "Vibrate Hand");
        actionInfo.countSubactionPaths = uint32_t(xr_inputs_.handSubactionPath.size());
        actionInfo.subactionPaths = xr_inputs_.handSubactionPath.data();
        XR_CHECK(xrCreateAction(xr_inputs_.actionSet, &actionInfo, &xr_inputs_.vibrateAction));

        // Create input actions for quitting the session using the left and right controller.
        // Since it doesn't matter which hand did this, we do not specify subaction paths for it.
        // We will just suggest bindings for both hands, where possible.
        actionInfo.actionType = XR_ACTION_TYPE_BOOLEAN_INPUT;
        strcpy(actionInfo.actionName, "quit_session");
        strcpy(actionInfo.localizedActionName, "Quit Session");
        actionInfo.countSubactionPaths = 0;
        actionInfo.subactionPaths = nullptr;
        XR_CHECK(xrCreateAction(xr_inputs_.actionSet, &actionInfo, &xr_inputs_.quitAction));
    }

    std::array<XrPath, NUM_SIDES> selectPath;
    std::array<XrPath, NUM_SIDES> squeezeValuePath;
    std::array<XrPath, NUM_SIDES> squeezeForcePath;
    std::array<XrPath, NUM_SIDES> squeezeClickPath;
    std::array<XrPath, NUM_SIDES> posePath;
    std::array<XrPath, NUM_SIDES> aimPath;
    std::array<XrPath, NUM_SIDES> stickXPath;
    std::array<XrPath, NUM_SIDES> stickYPath;
    std::array<XrPath, NUM_SIDES> hapticPath;
    std::array<XrPath, NUM_SIDES> menuClickPath;
    std::array<XrPath, NUM_SIDES> bClickPath;
    std::array<XrPath, NUM_SIDES> triggerValuePath;

    xrStringToPath(instance_, "/user/hand/left/input/select/click", &selectPath[LEFT]);
    xrStringToPath(instance_, "/user/hand/right/input/select/click", &selectPath[RIGHT]);
    xrStringToPath(instance_, "/user/hand/left/input/squeeze/value", &squeezeValuePath[LEFT]);
    xrStringToPath(instance_, "/user/hand/right/input/squeeze/value", &squeezeValuePath[RIGHT]);
    xrStringToPath(instance_, "/user/hand/left/input/squeeze/force", &squeezeForcePath[LEFT]);
    xrStringToPath(instance_, "/user/hand/right/input/squeeze/force", &squeezeForcePath[RIGHT]);
    xrStringToPath(instance_, "/user/hand/left/input/squeeze/click", &squeezeClickPath[LEFT]);
    xrStringToPath(instance_, "/user/hand/right/input/squeeze/click", &squeezeClickPath[RIGHT]);
    xrStringToPath(instance_, "/user/hand/left/input/grip/pose", &posePath[LEFT]);
    xrStringToPath(instance_, "/user/hand/right/input/grip/pose", &posePath[RIGHT]);
    xrStringToPath(instance_, "/user/hand/left/input/aim/pose", &aimPath[LEFT]);
    xrStringToPath(instance_, "/user/hand/right/input/aim/pose", &aimPath[RIGHT]);
    xrStringToPath(instance_, "/user/hand/left/input/thumbstick/x", &stickXPath[LEFT]);
    xrStringToPath(instance_, "/user/hand/right/input/thumbstick/x", &stickXPath[RIGHT]);
    xrStringToPath(instance_, "/user/hand/left/input/thumbstick/y", &stickYPath[LEFT]);
    xrStringToPath(instance_, "/user/hand/right/input/thumbstick/y", &stickYPath[RIGHT]);
    xrStringToPath(instance_, "/user/hand/left/output/haptic", &hapticPath[LEFT]);
    xrStringToPath(instance_, "/user/hand/right/output/haptic", &hapticPath[RIGHT]);
    xrStringToPath(instance_, "/user/hand/left/input/menu/click", &menuClickPath[LEFT]);
    xrStringToPath(instance_, "/user/hand/right/input/menu/click", &menuClickPath[RIGHT]);
    xrStringToPath(instance_, "/user/hand/left/input/b/click", &bClickPath[LEFT]);
    xrStringToPath(instance_, "/user/hand/right/input/b/click", &bClickPath[RIGHT]);
    xrStringToPath(instance_, "/user/hand/left/input/trigger/value", &triggerValuePath[LEFT]);
    xrStringToPath(instance_, "/user/hand/right/input/trigger/value", &triggerValuePath[RIGHT]);

    // Suggest bindings for KHR Simple.
    {
        XrPath khrSimpleInteractionProfilePath;
        xrStringToPath(instance_, "/interaction_profiles/khr/simple_controller", &khrSimpleInteractionProfilePath);

        std::vector<XrActionSuggestedBinding> bindings{{// Fall back to a click input for the grab action.
                                                               {xr_inputs_.grabAction, selectPath[LEFT]},
                                                               {xr_inputs_.grabAction, selectPath[RIGHT]},
                                                               {xr_inputs_.poseAction, posePath[LEFT]},
                                                               {xr_inputs_.poseAction, posePath[RIGHT]},
                                                               {xr_inputs_.aimPoseAction, aimPath[LEFT]},
                                                               {xr_inputs_.aimPoseAction, aimPath[RIGHT]},
                                                               {xr_inputs_.quitAction, menuClickPath[LEFT]},
                                                               {xr_inputs_.quitAction, menuClickPath[RIGHT]},
                                                               {xr_inputs_.vibrateAction, hapticPath[LEFT]},
                                                               {xr_inputs_.vibrateAction, hapticPath[RIGHT]}}};

        XrInteractionProfileSuggestedBinding suggestedBindings{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
        suggestedBindings.interactionProfile = khrSimpleInteractionProfilePath;
        suggestedBindings.suggestedBindings = bindings.data();
        suggestedBindings.countSuggestedBindings = (uint32_t)bindings.size();
        XR_CHECK(xrSuggestInteractionProfileBindings(instance_, &suggestedBindings));
    }

    // Suggest bindings for the Oculus Touch.
    {
        XrPath oculusTouchInteractionProfilePath;
        xrStringToPath(instance_, "/interaction_profiles/oculus/touch_controller", &oculusTouchInteractionProfilePath);

        std::vector<XrActionSuggestedBinding> bindings{{{xr_inputs_.grabAction, squeezeValuePath[LEFT]},
                                                        {xr_inputs_.grabAction, squeezeValuePath[RIGHT]},
                                                        {xr_inputs_.poseAction, posePath[LEFT]},
                                                        {xr_inputs_.poseAction, posePath[RIGHT]},
                                                        {xr_inputs_.aimPoseAction, aimPath[LEFT]},
                                                        {xr_inputs_.aimPoseAction, aimPath[RIGHT]},
                                                        {xr_inputs_.thumbstickXAction, stickXPath[LEFT]},
                                                        {xr_inputs_.thumbstickXAction, stickXPath[RIGHT]},
                                                        {xr_inputs_.thumbstickYAction, stickYPath[LEFT]},
                                                        {xr_inputs_.thumbstickYAction, stickYPath[RIGHT]},
                                                        {xr_inputs_.quitAction, menuClickPath[LEFT]},
                                                        {xr_inputs_.vibrateAction, hapticPath[LEFT]},
                                                        {xr_inputs_.vibrateAction, hapticPath[RIGHT]}}};

        XrInteractionProfileSuggestedBinding suggestedBindings{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
        suggestedBindings.interactionProfile = oculusTouchInteractionProfilePath;
        suggestedBindings.suggestedBindings = bindings.data();
        suggestedBindings.countSuggestedBindings = (uint32_t)bindings.size();
        XR_CHECK(xrSuggestInteractionProfileBindings(instance_, &suggestedBindings));
    }

    XrActionSpaceCreateInfo actionSpaceInfo{XR_TYPE_ACTION_SPACE_CREATE_INFO};
    actionSpaceInfo.action = xr_inputs_.poseAction;
    actionSpaceInfo.poseInActionSpace.orientation.w = 1.f;
    actionSpaceInfo.subactionPath = xr_inputs_.handSubactionPath[LEFT];
    XR_CHECK(xrCreateActionSpace(session_, &actionSpaceInfo, &xr_inputs_.handSpace[LEFT]));

    actionSpaceInfo.subactionPath = xr_inputs_.handSubactionPath[RIGHT];
    XR_CHECK(xrCreateActionSpace(session_, &actionSpaceInfo, &xr_inputs_.handSpace[RIGHT]));

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
    case XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED:
      IGL_LOG_INFO("xrPollEvent: received XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED event");
      break;
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
    assert(resumed_);
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
    XrPosef eyePose = views_[i].pose;
    XrPosef_Multiply(&viewStagePoses_[i], &headPose_, &eyePose);
    XrPosef viewTransformXrPosef{};
    XrPosef_Invert(&viewTransformXrPosef, &viewStagePoses_[i]);
    XrMatrix4x4f xrMat4{};
    XrMatrix4x4f_CreateFromRigidTransform(&xrMat4, &viewTransformXrPosef);
    viewTransforms_[i] = glm::make_mat4(xrMat4.m);
    cameraPositions_[i] = glm::vec3(eyePose.position.x, eyePose.position.y, eyePose.position.z);
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

  shellParams_->xr_app_ptr_ = this;

  renderSession_->pre_update();

  if (useSinglePassStereo_) {
    auto surfaceTextures = swapchainProviders_[0]->getSurfaceTextures();
    for (size_t j = 0; j < shellParams_->viewParams.size(); j++) {
      shellParams_->viewParams[j].viewMatrix = viewTransforms_[j];
      shellParams_->viewParams[j].cameraPosition = cameraPositions_[j];
      copyFov(shellParams_->viewParams[j].fov, views_[j].fov);
    }
    renderSession_->update(std::move(surfaceTextures));
    swapchainProviders_[0]->releaseSwapchainImages();
  } else {
    for (size_t i = 0; i < kNumViews; i++) {
      shellParams_->viewParams[0].viewMatrix = viewTransforms_[i];
      shellParams_->viewParams[0].cameraPosition = cameraPositions_[i];
      shellParams_->current_view_id_ = i;
      copyFov(shellParams_->viewParams[0].fov, views_[i].fov);
      auto surfaceTextures = swapchainProviders_[i]->getSurfaceTextures();
      renderSession_->update(surfaceTextures);
      swapchainProviders_[i]->releaseSwapchainImages();
    }
  }

  renderSession_->post_update();
}

void XrApp::endFrame(XrFrameState frameState) {
  const auto& appParams = renderSession_->appParams();

  std::array<XrCompositionLayerQuad, kNumViews> quadLayers{};
  if (useQuadLayerComposition_) {
    XrEyeVisibility eye = XR_EYE_VISIBILITY_LEFT;
    for (auto& layer : quadLayers) {
      layer.next = nullptr;
      layer.type = XR_TYPE_COMPOSITION_LAYER_QUAD;
      layer.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
      layer.space = currentSpace_;
      layer.eyeVisibility = eye;
      memset(&layer.subImage, 0, sizeof(XrSwapchainSubImage));
      layer.pose = {{0.f, 0.f, 0.f, 1.f}, {0.f, 0.f, 0.f}};
      layer.size = {appParams.sizeX, appParams.sizeY};
      if (eye == XR_EYE_VISIBILITY_LEFT) {
        eye = XR_EYE_VISIBILITY_RIGHT;
      }
    }
  }

  std::array<XrCompositionLayerProjectionView, kNumViews> projectionViews;
  std::array<XrCompositionLayerDepthInfoKHR, kNumViews> depthInfos;

  XrCompositionLayerProjection projection = {
      XR_TYPE_COMPOSITION_LAYER_PROJECTION,
      nullptr,
      XR_COMPOSITION_LAYER_CORRECT_CHROMATIC_ABERRATION_BIT,
      currentSpace_,
      static_cast<uint32_t>(kNumViews),
      projectionViews.data(),
  };

  for (size_t i = 0; i < kNumViews; i++) {
    projectionViews[i] = {
        XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW,
        &depthInfos[i],
        viewStagePoses_[i],
        views_[i].fov,
    };
    depthInfos[i] = {
        XR_TYPE_COMPOSITION_LAYER_DEPTH_INFO_KHR,
        nullptr,
    };
    XrRect2Di imageRect = {{0, 0},
                           {
                               (int32_t)viewports_[i].recommendedImageRectWidth,
                               (int32_t)viewports_[i].recommendedImageRectHeight,
                           }};
    auto index = useSinglePassStereo_ ? static_cast<uint32_t>(i) : 0;
    projectionViews[i].subImage = {
        useSinglePassStereo_ ? swapchainProviders_[0]->colorSwapchain()
                             : swapchainProviders_[i]->colorSwapchain(),
        imageRect,
        index,
    };
    depthInfos[i].subImage = {
        useSinglePassStereo_ ? swapchainProviders_[0]->depthSwapchain()
                             : swapchainProviders_[i]->depthSwapchain(),
        imageRect,
        index,
    };
    depthInfos[i].minDepth = appParams.depthParams.minDepth;
    depthInfos[i].maxDepth = appParams.depthParams.maxDepth;
    depthInfos[i].nearZ = appParams.depthParams.nearZ;
    depthInfos[i].farZ = appParams.depthParams.farZ;
    if (useQuadLayerComposition_) {
      quadLayers[i].subImage = projectionViews[i].subImage;
    }
  }

  XrFrameEndInfo endFrameInfo;
  if (useQuadLayerComposition_) {
    std::array<const XrCompositionLayerBaseHeader*, kNumViews> quadLayersBase{};
    for (uint32_t i = 0; i < quadLayers.size(); i++) {
      quadLayersBase[i] = (const XrCompositionLayerBaseHeader*)&quadLayers[i];
    }
    endFrameInfo = {XR_TYPE_FRAME_END_INFO,
                    nullptr,
                    frameState.predictedDisplayTime,
                    additiveBlendingSupported_ ? XR_ENVIRONMENT_BLEND_MODE_ADDITIVE
                                               : XR_ENVIRONMENT_BLEND_MODE_OPAQUE,
                    quadLayersBase.size(),
                    quadLayersBase.data()};
  } else {
    const XrCompositionLayerBaseHeader* const layers[] = {
        (const XrCompositionLayerBaseHeader*)&projection,
    };
    endFrameInfo = {XR_TYPE_FRAME_END_INFO,
                    nullptr,
                    frameState.predictedDisplayTime,
                    additiveBlendingSupported_ ? XR_ENVIRONMENT_BLEND_MODE_ADDITIVE
                                               : XR_ENVIRONMENT_BLEND_MODE_OPAQUE,
                    1,
                    layers};
  }

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
        getInfo.action = xr_inputs_.grabAction;
        getInfo.subactionPath = xr_inputs_.handSubactionPath[controller_id];

        XrActionStateFloat grabValue{XR_TYPE_ACTION_STATE_FLOAT};
        XR_CHECK(xrGetActionStateFloat(session_, &getInfo, &grabValue));

        if (grabValue.isActive == XR_TRUE)
        {
            xr_inputs_.handScale[controller_id] = 1.0f - 0.5f * grabValue.currentState;

            if (grabValue.currentState > 0.9f)
            {
                XrHapticVibration vibration{XR_TYPE_HAPTIC_VIBRATION};
                vibration.amplitude = 0.5;
                vibration.duration = XR_MIN_HAPTIC_DURATION;
                vibration.frequency = XR_FREQUENCY_UNSPECIFIED;

                XrHapticActionInfo hapticActionInfo{XR_TYPE_HAPTIC_ACTION_INFO};
                hapticActionInfo.action = xr_inputs_.vibrateAction;
                hapticActionInfo.subactionPath = xr_inputs_.handSubactionPath[controller_id];
                XR_CHECK(xrApplyHapticFeedback(session_, &hapticActionInfo, (XrHapticBaseHeader*)&vibration));
            }
        }

        getInfo.action = xr_inputs_.poseAction;
        XrActionStatePose poseState{XR_TYPE_ACTION_STATE_POSE};
        XR_CHECK(xrGetActionStatePose(session_, &getInfo, &poseState));
        xr_inputs_.handActive[controller_id] = poseState.isActive;
    }

    XrActionStateGetInfo getInfo{XR_TYPE_ACTION_STATE_GET_INFO, nullptr, xr_inputs_.quitAction, XR_NULL_PATH};
    XrActionStateBoolean quitValue{XR_TYPE_ACTION_STATE_BOOLEAN};
    XR_CHECK(xrGetActionStateBoolean(session_, &getInfo, &quitValue));

    if ((quitValue.isActive == XR_TRUE) && (quitValue.changedSinceLastSync == XR_TRUE) && (quitValue.currentState == XR_TRUE))
    {
        XR_CHECK(xrRequestExitSession(session_));
    }
}

} // namespace igl::shell::openxr
