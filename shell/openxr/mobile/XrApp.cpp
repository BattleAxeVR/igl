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

#if IGL_PLATFORM_ANDROID
#include <android/asset_manager.h>
#include <android/log.h>
#include <android_native_app_glue.h>
#endif

#include <glm/gtc/type_ptr.hpp>

#include <xr_linear.h>

#if IGL_PLATFORM_ANDROID
#include <shell/shared/fileLoader/android/FileLoaderAndroid.h>
#include <shell/shared/imageLoader/android/ImageLoaderAndroid.h>
#include <shell/shared/platform/android/PlatformAndroid.h>
#endif
#if IGL_PLATFORM_WIN
#include <shell/shared/platform/win/PlatformWin.h>
#endif

#include <shell/shared/input/InputDispatcher.h>
#include <shell/shared/input/IntentListener.h>
#include <shell/shared/renderSession/AppParams.h>
#include <shell/shared/renderSession/DefaultRenderSessionFactory.h>
#include <shell/shared/renderSession/ShellParams.h>

#include <shell/openxr/XrCompositionProjection.h>
#include <shell/openxr/XrCompositionQuad.h>
#include <shell/openxr/XrHands.h>
#include <shell/openxr/XrLog.h>
#include <shell/openxr/XrPassthrough.h>
#include <shell/openxr/XrSwapchainProvider.h>
#include <shell/openxr/impl/XrAppImpl.h>
#include <shell/openxr/impl/XrSwapchainProviderImpl.h>

#if ENABLE_CLOUDXR
#include "../src/cpp/ok_defines.h"
#include "../src/cpp/OKConfig.h"
BVR::OKConfig ok_config_s;
#endif

#if !IGL_PLATFORM_ANDROID
struct android_app {};
struct AAssetManager {};
#endif

namespace igl::shell::openxr {
constexpr auto kAppName = "IGL Shell OpenXR";
constexpr auto kEngineName = "IGL";
constexpr auto kSupportedViewConfigType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;

XrApp::XrApp(std::unique_ptr<impl::XrAppImpl>&& impl, bool shouldPresent) :
  impl_(std::move(impl)), shellParams_(std::make_unique<ShellParams>()) {
  shellParams_->shouldPresent = shouldPresent;
  viewports_.fill({XR_TYPE_VIEW_CONFIGURATION_VIEW});
  views_.fill({XR_TYPE_VIEW});
#ifdef USE_COMPOSITION_LAYER_QUAD
  useQuadLayerComposition_ = true;
#endif
}

XrApp::~XrApp() {
  if (!initialized_) {
    return;
  }

  renderSession_.reset();
  compositionLayers_.clear();
  passthrough_.reset();
  hands_.reset();

  if (currentSpace_ != XR_NULL_HANDLE) {
    xrDestroySpace(currentSpace_);
  }
  if (headSpace_ != XR_NULL_HANDLE) {
    xrDestroySpace(headSpace_);
  }
  if (session_ != XR_NULL_HANDLE) {
    xrDestroySession(session_);
  }
  if (instance_ != XR_NULL_HANDLE) {
    xrDestroyInstance(instance_);
  }

  platform_.reset();
}

XrInstance XrApp::instance() const {
  return instance_;
}

XrSession XrApp::session() const {
  return session_;
}

bool XrApp::checkExtensions() {
  XrResult result;
  PFN_xrEnumerateInstanceExtensionProperties xrEnumerateInstanceExtensionProperties;
  XR_CHECK(result =
               xrGetInstanceProcAddr(XR_NULL_HANDLE,
                                     "xrEnumerateInstanceExtensionProperties",
                                     (PFN_xrVoidFunction*)&xrEnumerateInstanceExtensionProperties));
  if (result != XR_SUCCESS) {
    IGL_LOG_ERROR("Failed to get xrEnumerateInstanceExtensionProperties function pointer.\n");
    return false;
  }

  uint32_t numExtensions = 0;
  XR_CHECK(xrEnumerateInstanceExtensionProperties(nullptr, 0, &numExtensions, nullptr));
  IGL_LOG_INFO("xrEnumerateInstanceExtensionProperties found %u extension(s).\n", numExtensions);

  extensions_.resize(numExtensions, {XR_TYPE_EXTENSION_PROPERTIES});

  XR_CHECK(xrEnumerateInstanceExtensionProperties(
      nullptr, numExtensions, &numExtensions, extensions_.data()));
  for (uint32_t i = 0; i < numExtensions; i++) {
    IGL_LOG_INFO("Extension #%d = '%s'.\n", i, extensions_[i].extensionName);
  }

  auto checkExtensionSupported = [this](const char* name) {
    return std::any_of(std::begin(extensions_),
                       std::end(extensions_),
                       [&](const XrExtensionProperties& extension) {
                         return strcmp(extension.extensionName, name) == 0;
                       });
  };

  // Check all required extensions are supported.
  auto requiredExtensionsImpl = impl_->getXrRequiredExtensions();
  for (const char* requiredExtension : requiredExtensionsImpl) {
    if (!checkExtensionSupported(requiredExtension)) {
      IGL_LOG_ERROR("Extension %s is required, but not supported.\n", requiredExtension);
      return false;
    }
  }

  auto checkNeedEnableExtension = [this](const char* name) {
    return std::find_if(std::begin(enabledExtensions_),
                        std::end(enabledExtensions_),
                        [&](const char* extensionName) {
                          return strcmp(extensionName, name) == 0;
                        }) == std::end(enabledExtensions_);
  };

  // Add required extensions to enabledExtensions_.
  for (const char* requiredExtension : requiredExtensionsImpl) {
    if (checkNeedEnableExtension(requiredExtension)) {
      IGL_LOG_INFO("Extension %s is enabled.\n", requiredExtension);
      enabledExtensions_.push_back(requiredExtension);
    }
  }

  // Get list of all optional extensions.
  auto optionalExtensionsImpl = impl_->getXrOptionalExtensions();
  std::vector<const char*> additionalOptionalExtensions = {
#if IGL_PLATFORM_ANDROID
      XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME,
#endif // IGL_PLATFORM_ANDROID
#ifdef XR_FB_composition_layer_alpha_blend
      XR_FB_COMPOSITION_LAYER_ALPHA_BLEND_EXTENSION_NAME,
#endif // XR_FB_composition_layer_alpha_blend
  };

#if ENABLE_META_OPENXR_FEATURES
  additionalOptionalExtensions.push_back(XR_FB_COMPOSITION_LAYER_SETTINGS_EXTENSION_NAME);
  additionalOptionalExtensions.push_back(XR_FB_TOUCH_CONTROLLER_PRO_EXTENSION_NAME);
  additionalOptionalExtensions.push_back(XR_FB_TOUCH_CONTROLLER_PROXIMITY_EXTENSION_NAME);

  additionalOptionalExtensions.push_back(XR_FB_BODY_TRACKING_EXTENSION_NAME);
  additionalOptionalExtensions.push_back(XR_META_BODY_TRACKING_FULL_BODY_EXTENSION_NAME);
  additionalOptionalExtensions.push_back(XR_META_BODY_TRACKING_FIDELITY_EXTENSION_NAME);

  additionalOptionalExtensions.push_back(XR_META_SIMULTANEOUS_HANDS_AND_CONTROLLERS_EXTENSION_NAME);
  additionalOptionalExtensions.push_back(XR_FB_EYE_TRACKING_SOCIAL_EXTENSION_NAME);
#endif

  additionalOptionalExtensions.push_back(XR_HTC_VIVE_FOCUS3_CONTROLLER_INTERACTION_EXTENSION_NAME);
  additionalOptionalExtensions.push_back(XR_BD_CONTROLLER_INTERACTION_EXTENSION_NAME);

  optionalExtensionsImpl.insert(optionalExtensionsImpl.end(),
                                std::begin(XrPassthrough::getExtensions()),
                                std::end(XrPassthrough::getExtensions()));

  optionalExtensionsImpl.insert(optionalExtensionsImpl.end(),
                                std::begin(XrHands::getExtensions()),
                                std::end(XrHands::getExtensions()));

  optionalExtensionsImpl.insert(optionalExtensionsImpl.end(),
                                std::begin(XrRefreshRate::getExtensions()),
                                std::end(XrRefreshRate::getExtensions()));

  optionalExtensionsImpl.insert(optionalExtensionsImpl.end(),
                                std::begin(additionalOptionalExtensions),
                                std::end(additionalOptionalExtensions));

  // Add optional extensions to enabledExtensions_.
  for (const char* optionalExtension : optionalExtensionsImpl) {
    if (checkExtensionSupported(optionalExtension)) {
      supportedOptionalXrExtensions_.insert(optionalExtension);
      if (checkNeedEnableExtension(optionalExtension)) {
        IGL_LOG_INFO("Extension %s is enabled.\n", optionalExtension);
        enabledExtensions_.push_back(optionalExtension);
      }
    } else {
      IGL_LOG_INFO("Warning: Extension %s is not supported.\n", optionalExtension);
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
  appInfo.apiVersion = XR_MAKE_VERSION(1, 0, 34);

  const XrInstanceCreateInfo instanceCreateInfo = {
      .type = XR_TYPE_INSTANCE_CREATE_INFO,
#if IGL_PLATFORM_ANDROID
      .next = instanceCreateInfoAndroidSupported() ? &instanceCreateInfoAndroid_ : nullptr,
#else
      .next = nullptr,
#endif // IGL_PLATFORM_ANDROID
      .createFlags = 0,
      .applicationInfo = appInfo,
      .enabledApiLayerCount = 0,
      .enabledApiLayerNames = nullptr,
      .enabledExtensionCount = static_cast<uint32_t>(enabledExtensions_.size()),
      .enabledExtensionNames = enabledExtensions_.data(),
  };

  XrResult initResult;
  XR_CHECK(initResult = xrCreateInstance(&instanceCreateInfo, &instance_));
  if (initResult != XR_SUCCESS) {
    IGL_LOG_ERROR("Failed to create XR instance: %d.\n", initResult);
    return false;
  }

  XR_CHECK(xrGetInstanceProperties(instance_, &instanceProps_));
  IGL_LOG_INFO("Runtime %s: Version : %u.%u.%u\n",
               instanceProps_.runtimeName,
               XR_VERSION_MAJOR(instanceProps_.runtimeVersion),
               XR_VERSION_MINOR(instanceProps_.runtimeVersion),
               XR_VERSION_PATCH(instanceProps_.runtimeVersion));

#if ENABLE_META_OPENXR_FEATURES
  if (simultaneousHandsAndControllersSupported()) {
    XR_CHECK(xrGetInstanceProcAddr(instance_,
                                   "xrResumeSimultaneousHandsAndControllersTrackingMETA",
                                   (PFN_xrVoidFunction*)(&xrResumeSimultaneousHandsAndControllersTrackingMETA_)));
    IGL_ASSERT(xrResumeSimultaneousHandsAndControllersTrackingMETA_ != nullptr);

    XR_CHECK(xrGetInstanceProcAddr(instance_,
                                   "xrPauseSimultaneousHandsAndControllersTrackingMETA",
                                   (PFN_xrVoidFunction*)(&xrPauseSimultaneousHandsAndControllersTrackingMETA_)));
    IGL_ASSERT(xrPauseSimultaneousHandsAndControllersTrackingMETA_ != nullptr);
}

#endif

  return true;
} // namespace igl::shell::openxr

bool XrApp::createSystem() {
  const XrSystemGetInfo systemGetInfo = {
      .type = XR_TYPE_SYSTEM_GET_INFO,
      .formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY,
  };

  XrResult result;
  XR_CHECK(result = xrGetSystem(instance_, &systemGetInfo, &systemId_));
  if (result != XR_SUCCESS) {
    IGL_LOG_ERROR("Failed to get system.\n");
    return false;
  }

#if ENABLE_META_OPENXR_FEATURES
    XrSystemPropertiesBodyTrackingFullBodyMETA meta_full_body_tracking_properties{ XR_TYPE_SYSTEM_PROPERTIES_BODY_TRACKING_FULL_BODY_META };

    if (metaFullBodyTrackingSupported())
    {
        meta_full_body_tracking_properties.next = systemProps_.next;
        systemProps_.next = &meta_full_body_tracking_properties;
    }

    XrSystemSimultaneousHandsAndControllersPropertiesMETA simultaneous_properties = { XR_TYPE_SYSTEM_SIMULTANEOUS_HANDS_AND_CONTROLLERS_PROPERTIES_META };

    if (simultaneousHandsAndControllersSupported())
    {
        simultaneous_properties.next = systemProps_.next;
        systemProps_.next = &simultaneous_properties;
    }
#endif

  XR_CHECK(xrGetSystemProperties(instance_, systemId_, &systemProps_));

#if ENABLE_META_OPENXR_FEATURES
    //metaFullBodyTrackingSupported_ = meta_full_body_tracking_properties.supportsFullBodyTracking;
    //simultaneousHandsAndControllersSupported_ = simultaneous_properties.supportsSimultaneousHandsAndControllers;
#endif

  IGL_LOG_INFO(
      "System Properties: Name=%s VendorId=%x\n", systemProps_.systemName, systemProps_.vendorId);
  IGL_LOG_INFO("System Graphics Properties: MaxWidth=%d MaxHeight=%d MaxLayers=%d\n",
               systemProps_.graphicsProperties.maxSwapchainImageWidth,
               systemProps_.graphicsProperties.maxSwapchainImageHeight,
               systemProps_.graphicsProperties.maxLayerCount);
  IGL_LOG_INFO("System Tracking Properties: OrientationTracking=%s PositionTracking=%s\n",
               systemProps_.trackingProperties.orientationTracking ? "True" : "False",
               systemProps_.trackingProperties.positionTracking ? "True" : "False");
  IGL_LOG_INFO("System Hand Tracking Properties: Supported=%s\n",
               handTrackingSystemProps_.supportsHandTracking ? "True" : "False");
  return true;
}

bool XrApp::enumerateViewConfigurations() {
  uint32_t numViewConfigs = 0;
  XR_CHECK(xrEnumerateViewConfigurations(instance_, systemId_, 0, &numViewConfigs, nullptr));

  std::vector<XrViewConfigurationType> viewConfigTypes(numViewConfigs);
  XR_CHECK(xrEnumerateViewConfigurations(
      instance_, systemId_, numViewConfigs, &numViewConfigs, viewConfigTypes.data()));

  IGL_LOG_INFO("Available Viewport Configuration Types: %d\n", numViewConfigs);
  auto foundViewConfig = false;
  for (auto& viewConfigType : viewConfigTypes) {
    IGL_LOG_INFO("View configuration type %d : %s\n",
                 viewConfigType,
                 viewConfigType == kSupportedViewConfigType ? "Selected" : "");

    if (viewConfigType != kSupportedViewConfigType) {
      continue;
    }

    // Check properties
    XrViewConfigurationProperties viewConfigProps = {XR_TYPE_VIEW_CONFIGURATION_PROPERTIES};
    XR_CHECK(
        xrGetViewConfigurationProperties(instance_, systemId_, viewConfigType, &viewConfigProps));
    IGL_LOG_INFO("FovMutable=%s ConfigurationType %d\n",
                 viewConfigProps.fovMutable ? "true" : "false",
                 viewConfigProps.viewConfigurationType);

    // Check views
    uint32_t numViewports = 0;
    XR_CHECK(xrEnumerateViewConfigurationViews(
        instance_, systemId_, viewConfigType, 0, &numViewports, nullptr));

    if (!IGL_DEBUG_VERIFY(numViewports == XrComposition::kNumViews)) {
      IGL_LOG_ERROR(
          "numViewports must be %d. Make sure XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO is used.\n",
          XrComposition::kNumViews);
      return false;
    }
	
#if ENABLE_CLOUDXR
      ok_config_s.load();
#endif

    XR_CHECK(xrEnumerateViewConfigurationViews(
        instance_, systemId_, viewConfigType, numViewports, &numViewports, viewports_.data()));

    for (auto& view : viewports_) {
	  
#if ENABLE_CLOUDXR
	view.recommendedImageRectWidth = ok_config_s.per_eye_width_;
	view.recommendedImageRectHeight = ok_config_s.per_eye_height_;
#else
	(void)view; // doesn't compile in release for unused variable
#endif

      IGL_LOG_INFO("Viewport [%d]: Recommended Width=%d Height=%d SampleCount=%d\n",
                   view,
                   view.recommendedImageRectWidth,
                   view.recommendedImageRectHeight,
                   view.recommendedSwapchainSampleCount);

      IGL_LOG_INFO("Viewport [%d]: Max Width=%d Height=%d SampleCount=%d\n",
                   view,
                   view.maxImageRectWidth,
                   view.maxImageRectHeight,
                   view.maxSwapchainSampleCount);
    }

    viewConfigProps_ = viewConfigProps;

    foundViewConfig = true;

    break;
  }

  IGL_DEBUG_ASSERT(
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
  IGL_LOG_INFO("OpenXR stage reference space is %s\n",
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
  IGL_LOG_INFO("OpenXR additive blending %s\n",
               additiveBlendingSupported_ ? "supported" : "not supported");
}

bool XrApp::initialize(const struct android_app* app, const InitParams& params) {
  if (initialized_) {
    return false;
  }

#if IGL_PLATFORM_ANDROID
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

  instanceCreateInfoAndroid_.applicationVM = app->activity->vm;
  instanceCreateInfoAndroid_.applicationActivity = app->activity->clazz;
#endif

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
    IGL_LOG_ERROR("Failed to initialize IGL\n");
    return false;
  }

#if IGL_WGL
  // Single stereo render pass is not supported for OpenGL on Windows.
  useSinglePassStereo_ = false;
#else
  useSinglePassStereo_ = useSinglePassStereo_ && device->hasFeature(igl::DeviceFeatures::Multiview);
#endif

#if IGL_PLATFORM_ANDROID
  createShellSession(std::move(device), app->activity->assetManager);
#else
  createShellSession(std::move(device), nullptr);
#endif

  session_ = impl_->initXrSession(instance_, systemId_, platform_->getDevice(), sessionConfig_);
  if (session_ == XR_NULL_HANDLE) {
    IGL_LOG_ERROR("Failed to initialize graphics system\n");
    return false;
  }

  // The following are initialization steps that happen after XrSession is created.
  enumerateReferenceSpaces();
  enumerateBlendModes();
  createSpaces();
  createActions();

  if (passthroughSupported()) {
    passthrough_ = std::make_unique<XrPassthrough>(instance_, session_);
    if (!passthrough_->initialize()) {
      return false;
    }
  }
  if (handTrackingSupported()) {
    hands_ = std::make_unique<XrHands>(instance_, session_, handTrackingMeshSupported());
    if (!hands_->initialize()) {
      return false;
    }
  }

#if ENABLE_META_OPENXR_FEATURES
  if (refreshRateExtensionSupported()) {
    refreshRate_ = std::make_unique<XrRefreshRate>(instance_, session_);
    if (!refreshRate_->initialize(params.refreshRateParams)) {
      return false;
    }
  }
#endif

  if (hands_) {
    hands_->updateMeshes(shellParams_->handMeshes);
  }

  IGL_DEBUG_ASSERT(renderSession_ != nullptr);
  renderSession_->initialize();

  if (useQuadLayerComposition_) {
    updateQuadComposition();
  } else {
    compositionLayers_.emplace_back(std::make_unique<XrCompositionProjection>(
        *impl_, platform_, session_, useSinglePassStereo_));

    compositionLayers_.back()->updateSwapchainImageInfo(
        {impl::SwapchainImageInfo{
             .imageWidth = viewports_[0].recommendedImageRectWidth,
             .imageHeight = viewports_[0].recommendedImageRectHeight,
         },
         impl::SwapchainImageInfo{
             .imageWidth = viewports_[1].recommendedImageRectWidth,
             .imageHeight = viewports_[1].recommendedImageRectHeight,
         }});
  }

  initialized_ = true;
  return initialized_;
}

// NOLINTNEXTLINE(bugprone-exception-escape)
void XrApp::updateQuadComposition() noexcept {
  const auto& appParams = renderSession_->appParams();

  constexpr uint32_t kQuadLayerDefaultImageSize = 1024;

  const auto aspect = appParams.sizeY / appParams.sizeX;
  QuadLayerParams quadLayersParams = {
      .layerInfo = {{
#if USE_LOCAL_AR_SPACE
          .position = {0.0f, 0.0f, -1.0f},
#else
          .position = {0.0f, 0.0f, 0.0f},
#endif
          .size = {appParams.sizeX, appParams.sizeY},
          .blendMode = LayerBlendMode::AlphaBlend,
          .imageWidth = kQuadLayerDefaultImageSize,
          .imageHeight = static_cast<uint32_t>(kQuadLayerDefaultImageSize * aspect),
      }}};

  if (appParams.quadLayerParamsGetter) {
    auto params = appParams.quadLayerParamsGetter();
    if (params.numQuads() > 0) {
      quadLayersParams = std::move(params);
    }
  }

  std::array<impl::SwapchainImageInfo, XrComposition::kNumViews> swapchainImageInfo{};
  for (size_t i = 0; i < quadLayersParams.numQuads(); ++i) {
    swapchainImageInfo.fill({
        .imageWidth = quadLayersParams.layerInfo[i].imageWidth,
        .imageHeight = quadLayersParams.layerInfo[i].imageHeight,
    });

    if (i < compositionLayers_.size()) {
      auto* quadLayer = static_cast<XrCompositionQuad*>(compositionLayers_[i].get());
      quadLayer->updateQuadLayerInfo(quadLayersParams.layerInfo[i]);
      quadLayer->updateSwapchainImageInfo(swapchainImageInfo);
    } else {
      compositionLayers_.emplace_back(
          std::make_unique<XrCompositionQuad>(*impl_,
                                              platform_,
                                              session_,
                                              useSinglePassStereo_,
                                              alphaBlendCompositionSupported(),
                                              quadLayersParams.layerInfo[i]));
      compositionLayers_.back()->updateSwapchainImageInfo(swapchainImageInfo);
    }
  }

  // Remove any layers that are no longer needed.
  compositionLayers_.resize(quadLayersParams.numQuads());
}

void XrApp::createShellSession(std::unique_ptr<igl::IDevice> device, AAssetManager* assetMgr) {
#if IGL_PLATFORM_ANDROID
  platform_ = std::make_shared<igl::shell::PlatformAndroid>(std::move(device));
  IGL_DEBUG_ASSERT(platform_ != nullptr);
  static_cast<igl::shell::ImageLoaderAndroid&>(platform_->getImageLoader())
      .setAssetManager(assetMgr);
  static_cast<igl::shell::FileLoaderAndroid&>(platform_->getFileLoader()).setAssetManager(assetMgr);
#elif IGL_PLATFORM_APPLE
  platform_ = std::make_shared<igl::shell::PlatformMac>(std::move(device));
#elif IGL_PLATFORM_WIN
  platform_ = std::make_shared<igl::shell::PlatformWin>(std::move(device));
#endif

  auto factory = igl::shell::createDefaultRenderSessionFactory();
  const auto requestedSessionConfigs =
      factory->requestedSessionConfigs(shell::ShellType::OpenXR, {impl_->suggestedSessionConfig()});
  if (IGL_DEBUG_VERIFY_NOT(requestedSessionConfigs.size() != 1)) {
    return;
  }
  sessionConfig_ = requestedSessionConfigs[0];

  renderSession_ = factory->createRenderSession(platform_);
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

#if ENABLE_META_OPENXR_FEATURES
    // Touch Pro
    const bool is_quest_pro = (headsetType_ == HeadsetType::META_QUEST_PRO_);

    if (is_quest_pro && touchProControllersSupported())
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
#endif

    if (htcViveFocus3ControllersSupported())
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

    if (byteDanceControllersSupported())
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
    auto* baseEventHeader = (XrEventDataBaseHeader*)(&eventDataBuffer);
    baseEventHeader->type = XR_TYPE_EVENT_DATA_BUFFER;
    baseEventHeader->next = nullptr;
    XrResult res;
    XR_CHECK(res = xrPollEvent(instance_, &eventDataBuffer));
    if (res != XR_SUCCESS) {
      break;
    }

    switch (baseEventHeader->type) {
    case XR_TYPE_EVENT_DATA_EVENTS_LOST:
      IGL_LOG_INFO("xrPollEvent: received XR_TYPE_EVENT_DATA_EVENTS_LOST event\n");
      break;
    case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING:
      IGL_LOG_INFO("xrPollEvent: received XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING event\n");
      break;
    case XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED:
      IGL_LOG_INFO("xrPollEvent: received XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED event\n");
      break;
    case XR_TYPE_EVENT_DATA_PERF_SETTINGS_EXT: {
      const XrEventDataPerfSettingsEXT* perf_settings_event =
          (XrEventDataPerfSettingsEXT*)(baseEventHeader);
      (void)perf_settings_event; // suppress unused warning
      IGL_LOG_INFO(
          "xrPollEvent: received XR_TYPE_EVENT_DATA_PERF_SETTINGS_EXT event: type %d subdomain %d "
          ": level %d -> level %d\n",
          perf_settings_event->type,
          perf_settings_event->subDomain,
          perf_settings_event->fromLevel,
          perf_settings_event->toLevel);
    } break;
    case XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING:
      IGL_LOG_INFO(
          "xrPollEvent: received XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING event\n");
      break;
    case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED: {
      const XrEventDataSessionStateChanged* session_state_changed_event =
          (XrEventDataSessionStateChanged*)(baseEventHeader);
      IGL_LOG_INFO(
          "xrPollEvent: received XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED: %d for session %p at "
          "time %lld\n",
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
      IGL_LOG_INFO("xrPollEvent: Unknown event\n");
      break;
    }
  }
}

void XrApp::handleActionView(const std::string& data) {
  if (platform_ != nullptr) {
    igl::shell::IntentEvent event;
    event.type = igl::shell::IntentType::ActionView;
    event.data = data;
    platform_->getInputDispatcher().queueEvent(event);
  }
}

void XrApp::handleSessionStateChanges(XrSessionState state) {
  if (state == XR_SESSION_STATE_READY) {
#if !defined(IGL_CMAKE_BUILD)
    assert(resumed_);
#endif // IGL_CMAKE_BUILD
    assert(sessionActive_ == false);

    const XrSessionBeginInfo sessionBeginInfo{
        XR_TYPE_SESSION_BEGIN_INFO,
        nullptr,
        viewConfigProps_.viewConfigurationType,
    };

    XrResult result;
    XR_CHECK(result = xrBeginSession(session_, &sessionBeginInfo));

    sessionActive_ = (result == XR_SUCCESS);
    IGL_LOG_INFO("XR session active\n");
  } else if (state == XR_SESSION_STATE_STOPPING) {
    assert(sessionActive_);
    XR_CHECK(xrEndSession(session_));
    sessionActive_ = false;
    IGL_LOG_INFO("XR session inactive\n");
  }
}

XrFrameState XrApp::beginFrame() {
  if (passthrough_) {
    passthrough_->setEnabled(passthroughEnabled());
  }

  if (useQuadLayerComposition_) {
    updateQuadComposition();
  }

  const XrFrameWaitInfo waitFrameInfo = {XR_TYPE_FRAME_WAIT_INFO};

  XrFrameState frameState = {XR_TYPE_FRAME_STATE};

  XR_CHECK(xrWaitFrame(session_, &waitFrameInfo, &frameState));

  const XrFrameBeginInfo beginFrameInfo = {XR_TYPE_FRAME_BEGIN_INFO};

  XR_CHECK(xrBeginFrame(session_, &beginFrameInfo));

  XrSpaceLocation loc = {
      loc.type = XR_TYPE_SPACE_LOCATION,
  };
  XR_CHECK(xrLocateSpace(headSpace_, currentSpace_, frameState.predictedDisplayTime, &loc));
  const XrPosef headPose = loc.pose;

  XrViewState viewState = {XR_TYPE_VIEW_STATE};

  const XrViewLocateInfo projectionInfo = {
      XR_TYPE_VIEW_LOCATE_INFO,
      nullptr,
      viewConfigProps_.viewConfigurationType,
      frameState.predictedDisplayTime,
      headSpace_,
  };

  uint32_t numViews = views_.size();

  XR_CHECK(xrLocateViews(
      session_, &projectionInfo, &viewState, views_.size(), &numViews, views_.data()));

  for (size_t i = 0; i < XrComposition::kNumViews; i++) {
    const XrPosef eyePose = views_[i].pose;
    XrPosef_Multiply(&viewStagePoses_[i], &headPose, &eyePose);
    XrPosef viewTransformXrPosef{};
    XrPosef_Invert(&viewTransformXrPosef, &viewStagePoses_[i]);
    XrMatrix4x4f xrMat4{};
    XrMatrix4x4f_CreateFromRigidTransform(&xrMat4, &viewTransformXrPosef);
    viewTransforms_[i] = glm::make_mat4(xrMat4.m);
    cameraPositions_[i] = glm::vec3(eyePose.position.x, eyePose.position.y, eyePose.position.z);
  }

  if (hands_) {
    hands_->updateTracking(currentSpace_, shellParams_->handTracking);
  }

  return frameState;
}

void XrApp::render() {
  if (passthrough_) {
    if (passthroughEnabled()) {
      shellParams_->clearColorValue = igl::Color{0.0f, 0.0f, 0.0f, 0.0f};
    } else {
      shellParams_->clearColorValue.reset();
    }
  }
#if USE_FORCE_ZERO_CLEAR
  else {
    shellParams_->clearColorValue = igl::Color{0.0f, 0.0f, 0.0f, 0.0f};
  }
#endif

#if ENABLE_CLOUDXR
  shellParams_->xr_app_ptr_ = this;

  if (!renderSession_->pre_update()){
    return;
  }
#endif

  for (size_t layerIndex = 0; layerIndex < compositionLayers_.size(); ++layerIndex) {
    if (!compositionLayers_[layerIndex]->isValid()) {
      continue;
    }

    for (uint32_t i = 0; i < compositionLayers_[layerIndex]->renderPassesCount(); ++i) {
      auto surfaceTextures = compositionLayers_[layerIndex]->beginRendering(
          i, views_, viewTransforms_, cameraPositions_, shellParams_->viewParams);

      renderSession_->setCurrentQuadLayer(useQuadLayerComposition_ ? layerIndex : 0);
	  
#if ENABLE_CLOUDXR
      shellParams_->viewParams[0].cameraPosition = cameraPositions_[i];
      shellParams_->current_view_id_ = i;
#endif

      renderSession_->update(std::move(surfaceTextures));

      compositionLayers_[layerIndex]->endRendering(i);
    }
  }

#if ENABLE_CLOUDXR
  renderSession_->post_update();
#endif
}

void XrApp::endFrame(XrFrameState frameState) {

  XrCompositionLayerFlags compositionFlags = XR_COMPOSITION_LAYER_CORRECT_CHROMATIC_ABERRATION_BIT;
  if (passthroughEnabled()) {
    compositionFlags |= XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
  }

  std::vector<const XrCompositionLayerBaseHeader*> layers;
  layers.reserve(1 + compositionLayers_.size() * (useQuadLayerComposition_ ? 2 : 1));

  if (passthroughEnabled()) {
    passthrough_->injectLayer(layers);
  }

  const auto& appParams = renderSession_->appParams();
  for (const auto& layer : compositionLayers_) {
    if (layer->isValid()) {

#if ENABLE_CLOUDXR
        if (should_override_eye_poses_)
        {
            std::array<XrPosef, XrComposition::kNumViews> viewStagePoseOverrides = viewStagePoses_;

            for (int view = 0; view < XrComposition::kNumViews; view++)
            {
                viewStagePoseOverrides[view] = override_eye_poses_[view];
            }

            layer->doComposition(appParams.depthParams, views_, viewStagePoseOverrides, currentSpace_, compositionFlags, layers);
        }
        else

#endif
        {
            layer->doComposition(
                    appParams.depthParams, views_, viewStagePoses_, currentSpace_, compositionFlags, layers);
        }
    }
  }

  const XrFrameEndInfo endFrameInfo{
      .type = XR_TYPE_FRAME_END_INFO,
      .next = nullptr,
      .displayTime = frameState.predictedDisplayTime,
      .environmentBlendMode = additiveBlendingSupported_ ? XR_ENVIRONMENT_BLEND_MODE_ADDITIVE
                                                         : XR_ENVIRONMENT_BLEND_MODE_OPAQUE,
      .layerCount = static_cast<uint32_t>(layers.size()),
      .layers = layers.data(),
  };
  XR_CHECK(xrEndFrame(session_, &endFrameInfo));
}

void XrApp::update() {
  if (!initialized_ || !resumed_ || !sessionActive_) {
    return;
  }

  if (platform_ != nullptr) {
    platform_->getInputDispatcher().processEvents();
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
}

bool XrApp::passthroughSupported() const noexcept {
#if ENABLE_PASSTHROUGH
  return supportedOptionalXrExtensions_.count(XR_FB_PASSTHROUGH_EXTENSION_NAME) != 0;
#else
  return false;
#endif
}

bool XrApp::passthroughEnabled() const noexcept {
  if (!renderSession_ || !passthrough_) {
    return false;
  }
  const auto& appParams = renderSession_->appParams();
  return appParams.passthroughGetter ? appParams.passthroughGetter() : useQuadLayerComposition_;
}

bool XrApp::handTrackingSupported() const noexcept {
#if IGL_PLATFORM_ANDROID
  return supportedOptionalXrExtensions_.count(XR_EXT_HAND_TRACKING_EXTENSION_NAME) != 0 &&
         handTrackingSystemProps_.supportsHandTracking != 0u;
#endif // IGL_PLATFORM_ANDROID
  return false;
}

bool XrApp::handTrackingMeshSupported() const noexcept {
#if IGL_PLATFORM_ANDROID
  //return supportedOptionalXrExtensions_.count(XR_FB_HAND_TRACKING_MESH_EXTENSION_NAME) != 0;
    return false;
#endif // IGL_PLATFORM_ANDROID
  return false;
}

bool XrApp::refreshRateExtensionSupported() const noexcept {
  return supportedOptionalXrExtensions_.count(XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME) != 0;
}

bool XrApp::instanceCreateInfoAndroidSupported() const noexcept {
#if IGL_PLATFORM_ANDROID
  return supportedOptionalXrExtensions_.count(XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME) != 0;
#endif // IGL_PLATFORM_ANDROID
  return false;
}

bool XrApp::alphaBlendCompositionSupported() const noexcept {
#ifdef XR_FB_composition_layer_alpha_blend
  return supportedOptionalXrExtensions_.count(XR_FB_COMPOSITION_LAYER_ALPHA_BLEND_EXTENSION_NAME) !=
         0;
#endif // XR_FB_composition_layer_alpha_blend
  return false;
}

bool XrApp::compositionLayerSettingsSupported() const noexcept {
  return supportedOptionalXrExtensions_.count(XR_FB_COMPOSITION_LAYER_SETTINGS_EXTENSION_NAME) != 0;
}

bool XrApp::touchProControllersSupported() const noexcept {
  return supportedOptionalXrExtensions_.count(XR_FB_TOUCH_CONTROLLER_PRO_EXTENSION_NAME) != 0;
}

bool XrApp::touchControllerProximitySupported() const noexcept {
  return supportedOptionalXrExtensions_.count(XR_FB_TOUCH_CONTROLLER_PROXIMITY_EXTENSION_NAME) != 0;
}

bool XrApp::bodyTrackingFBSupported() const noexcept {
  return supportedOptionalXrExtensions_.count(XR_FB_BODY_TRACKING_EXTENSION_NAME) != 0;
}

bool XrApp::metaFullBodyTrackingSupported() const noexcept {
  return supportedOptionalXrExtensions_.count(XR_META_BODY_TRACKING_FULL_BODY_EXTENSION_NAME) != 0;
}

bool XrApp::metaBodyTrackingFidelitySupported() const noexcept {
  return supportedOptionalXrExtensions_.count(XR_META_BODY_TRACKING_FIDELITY_EXTENSION_NAME) != 0;
}

bool XrApp::simultaneousHandsAndControllersSupported() const noexcept {
  return supportedOptionalXrExtensions_.count(XR_META_SIMULTANEOUS_HANDS_AND_CONTROLLERS_EXTENSION_NAME) != 0;
}

bool XrApp::eyeTrackingSocialFBSupported() const noexcept {
  return supportedOptionalXrExtensions_.count(XR_FB_EYE_TRACKING_SOCIAL_EXTENSION_NAME) != 0;
}

bool XrApp::htcViveFocus3ControllersSupported() const noexcept {
  return supportedOptionalXrExtensions_.count(XR_HTC_VIVE_FOCUS3_CONTROLLER_INTERACTION_EXTENSION_NAME) != 0;
}

bool XrApp::byteDanceControllersSupported() const noexcept {
  return supportedOptionalXrExtensions_.count(XR_BD_CONTROLLER_INTERACTION_EXTENSION_NAME) != 0;
}

#if ENABLE_META_OPENXR_FEATURES

bool XrApp::isSharpeningEnabled() const {
  return compositionLayerSettingsSupported() &&
  ((compositionLayerSettings_.layerFlags & XR_COMPOSITION_LAYER_SETTINGS_QUALITY_SHARPENING_BIT_FB) != 0);
}

void XrApp::setSharpeningEnabled(const bool enabled) {
  if (!compositionLayerSettingsSupported() || (enabled == isSharpeningEnabled())) {
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

bool XrApp::setSimultaneousHandsAndControllersEnabled(const bool enabled) {
    if (!simultaneousHandsAndControllersSupported() || (enabled == simultaneousHandsAndControllersEnabled_) || !xrResumeSimultaneousHandsAndControllersTrackingMETA_ || !xrPauseSimultaneousHandsAndControllersTrackingMETA_) {
        return false;
    }

    XrResult result = XR_SUCCESS;

    if (enabled)
    {
        XrSimultaneousHandsAndControllersTrackingResumeInfoMETA resumeInfoMeta{
                XR_TYPE_SIMULTANEOUS_HANDS_AND_CONTROLLERS_TRACKING_RESUME_INFO_META
        };

        result = xrResumeSimultaneousHandsAndControllersTrackingMETA_(session_, &resumeInfoMeta);
    }
    else
    {
        XrSimultaneousHandsAndControllersTrackingPauseInfoMETA pauseInfoMeta{
                XR_TYPE_SIMULTANEOUS_HANDS_AND_CONTROLLERS_TRACKING_PAUSE_INFO_META
        };

        result = xrPauseSimultaneousHandsAndControllersTrackingMETA_(session_, &pauseInfoMeta);
    }

    if (result == XR_SUCCESS)
    {
        simultaneousHandsAndControllersEnabled_ = enabled;
        IGL_LOG_INFO("Simultaneous Hands and Controllers are now %s", areSimultaneousHandsAndControllersEnabled() ? "ON" : "OFF");
        return true;
    }

    return false;
}
#endif

} // namespace igl::shell::openxr
