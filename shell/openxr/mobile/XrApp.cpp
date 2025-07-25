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
#include <android_native_app_glue.h>
#endif

#include <glm/gtc/type_ptr.hpp>

#include <xr_linear.h>

#if IGL_PLATFORM_ANDROID
#include <shell/shared/fileLoader/android/FileLoaderAndroid.h>
#include <shell/shared/platform/android/PlatformAndroid.h>
#endif
#if IGL_PLATFORM_WINDOWS
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

#if ENABLE_PASSTHROUGH
  passthrough_.reset();
#endif

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
  PFN_xrEnumerateInstanceExtensionProperties xrEnumerateInstanceExtensionProperties = nullptr;
  const XrResult result =
      xrGetInstanceProcAddr(XR_NULL_HANDLE,
                            "xrEnumerateInstanceExtensionProperties",
                            (PFN_xrVoidFunction*)&xrEnumerateInstanceExtensionProperties);
  XR_CHECK(result);
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
  additionalOptionalExtensions.push_back(XR_FB_EYE_TRACKING_SOCIAL_EXTENSION_NAME);
  additionalOptionalExtensions.push_back(XR_HTC_VIVE_FOCUS3_CONTROLLER_INTERACTION_EXTENSION_NAME);
  additionalOptionalExtensions.push_back(XR_BD_CONTROLLER_INTERACTION_EXTENSION_NAME);
#endif

#if SUPPORT_BODY_TRACKING_FB
  additionalOptionalExtensions.push_back(XR_FB_BODY_TRACKING_EXTENSION_NAME);
  additionalOptionalExtensions.push_back(XR_META_BODY_TRACKING_FULL_BODY_EXTENSION_NAME);
  additionalOptionalExtensions.push_back(XR_META_BODY_TRACKING_FIDELITY_EXTENSION_NAME);
#endif

#if SUPPORT_OPENXR_SIMULTANEOUS_HANDS_AND_CONTROLLERS
  additionalOptionalExtensions.push_back(XR_META_SIMULTANEOUS_HANDS_AND_CONTROLLERS_EXTENSION_NAME);
#endif

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

  const XrResult initResult = xrCreateInstance(&instanceCreateInfo, &instance_);
  XR_CHECK(initResult);
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

#if (ENABLE_META_OPENXR_FEATURES && 0)
  if (simultaneousHandsAndControllersSupported()) {
    XR_CHECK(xrGetInstanceProcAddr(instance_,
                                   "xrResumeSimultaneousHandsAndControllersTrackingMETA",
                                   (PFN_xrVoidFunction*)(&xrResumeSimultaneousHandsAndControllersTrackingMETA_)));

    XR_CHECK(xrGetInstanceProcAddr(instance_,
                                   "xrPauseSimultaneousHandsAndControllersTrackingMETA",
                                   (PFN_xrVoidFunction*)(&xrPauseSimultaneousHandsAndControllersTrackingMETA_)));
}

#endif

  return true;
} // namespace igl::shell::openxr

bool XrApp::createSystem() {
  const XrSystemGetInfo systemGetInfo = {
      .type = XR_TYPE_SYSTEM_GET_INFO,
      .formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY,
  };

  const XrResult result = xrGetSystem(instance_, &systemGetInfo, &systemId_);
  XR_CHECK(result);
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
  PFN_xrInitializeLoaderKHR xrInitializeLoaderKHR = nullptr;
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

  std::unique_ptr<IDevice> device;
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
  //createActions();

#if (ENABLE_PASSTHROUGH && !DRAW_UI)
  if (passthroughSupported()) {
    passthrough_ = std::make_unique<XrPassthrough>(instance_, session_);
    if (!passthrough_->initialize()) {
      return false;
    }
  }
#endif

  if (handTrackingSupported()) {
    hands_ = std::make_unique<XrHands>(instance_, session_, handTrackingMeshSupported());
    if (!hands_->initialize()) {
      return false;
    }
  }

#if ENABLE_FB_REFRESH_RATE
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

#if DRAW_UI
    if (useQuadLayerCompositionForUI_) {
        updateQuadCompositionForUI();
    }
#endif

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

#if DRAW_UI
// NOLINTNEXTLINE(bugprone-exception-escape)
    void XrApp::updateQuadCompositionForUI() noexcept
    {
        const AppParams& appParams = renderSession_->appParams();

        const float default_UI_height = GUI_PANEL_ELEVATION;
        const float default_UI_distance = GUI_PANEL_DISTANCE;
        const float default_UI_scale_x = GUI_PANEL_SCALE_X;
        const float default_UI_scale_y = GUI_PANEL_SCALE_Y;

        QuadLayerParams quadLayersParams = {
                .layerInfo = {{
                                      .position = {0.0f, default_UI_height, default_UI_distance},
                                      .size = {default_UI_scale_x, default_UI_scale_y},
                                      .blendMode = LayerBlendMode::AlphaBlend,
                                      .imageWidth = GUI_PANEL_WIDTH,
                                      .imageHeight = GUI_PANEL_HEIGHT,
                                      .customSrcRGBBlendFactor = igl::BlendFactor::One,
                                      .customSrcAlphaBlendFactor = igl::BlendFactor::One,
                                      .customDstRGBBlendFactor = igl::BlendFactor::One,
                                      .customDstAlphaBlendFactor = igl::BlendFactor::One,
                                      .bothEyesVisible = true,
                              }}};

        if (appParams.quadLayerParamsGetter)
        {
            QuadLayerParams params = appParams.quadLayerParamsGetter();

            if (params.numQuads() > 0)
            {
                quadLayersParams = std::move(params);
            }
        }

        std::array<impl::SwapchainImageInfo, XrComposition::kNumViews> swapchainImageInfo{};

        const size_t numCompositionLayers = compositionLayers_.size();
        const size_t numQuads = quadLayersParams.numQuads();

        for (size_t quadLayerIndex = 0; quadLayerIndex < numQuads; ++quadLayerIndex)
        {
            size_t compositionLayerIndex = quadLayerIndex + 1;

            swapchainImageInfo.fill({
                                            .imageWidth = quadLayersParams.layerInfo[quadLayerIndex].imageWidth,
                                            .imageHeight = quadLayersParams.layerInfo[quadLayerIndex].imageHeight,
                                    });

            if (compositionLayerIndex < numCompositionLayers)
            {
                auto* quadLayer = static_cast<XrCompositionQuad*>(compositionLayers_[compositionLayerIndex].get());
                quadLayer->updateQuadLayerInfo(quadLayersParams.layerInfo[quadLayerIndex]);
                quadLayer->updateSwapchainImageInfo(swapchainImageInfo);
            }
            else
            {
                compositionLayers_.emplace_back(
                        std::make_unique<XrCompositionQuad>(*impl_,
                                                            platform_,
                                                            session_,
                                                            useSinglePassStereo_,
                                                            alphaBlendCompositionSupported(),
                                                            quadLayersParams.layerInfo[quadLayerIndex]));

                compositionLayers_.back()->updateSwapchainImageInfo(swapchainImageInfo);
            }
        }
    }
#endif

void XrApp::createShellSession(std::unique_ptr<IDevice> device, AAssetManager* assetMgr) {

#if IGL_PLATFORM_ANDROID
  platform_ = std::make_shared<PlatformAndroid>(std::move(device));
  IGL_DEBUG_ASSERT(platform_ != nullptr);
  static_cast<FileLoaderAndroid&>(platform_->getFileLoader()).setAssetManager(assetMgr);
#elif IGL_PLATFORM_APPLE
  platform_ = std::make_shared<igl::shell::PlatformMac>(std::move(device));
#elif IGL_PLATFORM_WINDOWS
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

#if ENABLE_CONTROLLERS
void XrApp::createActions()
{
    //BVR::HeadsetType headset_type = BVR::OKOpenXRInterface::compute_headset_type(systemProps_.systemName, systemProps_.systemId, systemProps_.vendorId);
    //ok_inputs_.init(headset_type, instance_, session_);
}

#endif // ENABLE_CONTROLLERS

void XrApp::handleXrEvents() {
  XrEventDataBuffer eventDataBuffer = {};

  // Poll for events
  for (;;) {
    auto* baseEventHeader = (XrEventDataBaseHeader*)(&eventDataBuffer);
    baseEventHeader->type = XR_TYPE_EVENT_DATA_BUFFER;
    baseEventHeader->next = nullptr;
    const XrResult res = xrPollEvent(instance_, &eventDataBuffer);
    XR_CHECK(res);
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
      const XrEventDataPerfSettingsEXT* perfSettingsEvent =
          (XrEventDataPerfSettingsEXT*)(baseEventHeader);
      (void)perfSettingsEvent; // suppress unused warning
      IGL_LOG_INFO(
          "xrPollEvent: received XR_TYPE_EVENT_DATA_PERF_SETTINGS_EXT event: type %d subdomain %d "
          ": level %d -> level %d\n",
          perfSettingsEvent->type,
          perfSettingsEvent->subDomain,
          perfSettingsEvent->fromLevel,
          perfSettingsEvent->toLevel);
    } break;
    case XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING:
      IGL_LOG_INFO(
          "xrPollEvent: received XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING event\n");
      break;
    case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED: {
      const XrEventDataSessionStateChanged* sessionStateChangedEvent =
          (XrEventDataSessionStateChanged*)(baseEventHeader);
      IGL_LOG_INFO(
          "xrPollEvent: received XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED: %d for session %p at "
          "time %lld\n",
          sessionStateChangedEvent->state,
          (void*)sessionStateChangedEvent->session,
          sessionStateChangedEvent->time);

      switch (sessionStateChangedEvent->state) {
      case XR_SESSION_STATE_READY:
      case XR_SESSION_STATE_STOPPING:
        handleSessionStateChanges(sessionStateChangedEvent->state);
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
    IntentEvent event;
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

    const XrResult result = xrBeginSession(session_, &sessionBeginInfo);
    XR_CHECK(result);

    sessionActive_ = (result == XR_SUCCESS);
    IGL_LOG_INFO("XR session active\n");
  } else if (state == XR_SESSION_STATE_STOPPING) {
    assert(sessionActive_);
    XR_CHECK(xrEndSession(session_));
    sessionActive_ = false;
    IGL_LOG_INFO("XR session inactive\n");
  }
}

#if ENABLE_PASSTHROUGH
    void XrApp::setPassThroughEnabled(const bool passThroughEnabled)
    {
        if (passthrough_ && !passThroughEnabled)
        {
            passthrough_->setEnabled(false);
            passthrough_.reset();
        }
        else if (!passthrough_ && passthroughSupported() && passThroughEnabled)
        {
            passthrough_ = std::make_unique<XrPassthrough>(instance_, session_);

            if (!passthrough_->initialize())
            {
                return;
            }

            passthrough_->setEnabled(true);
        }
    }
#endif

XrFrameState XrApp::beginFrame() {

#if (ENABLE_PASSTHROUGH && !DRAW_UI)
  if (passthrough_) {
    passthrough_->setEnabled(passthroughEnabled());
  }
#endif

#if DRAW_UI
    if (useQuadLayerCompositionForUI_) {
        updateQuadCompositionForUI();
    }
#endif

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

#if ENABLE_PASSTHROUGH
  if (passthrough_) {
    if (passthroughEnabled()) {
      shellParams_->clearColorValue = Color{0.0f, 0.0f, 0.0f, 0.0f};
    } else {
      shellParams_->clearColorValue.reset();
    }
  }
  else
#endif

#if USE_FORCE_ZERO_CLEAR
  {
    shellParams_->clearColorValue = Color{0.0f, 0.0f, 0.0f, 0.0f};
  }
#endif

#if ENABLE_CLOUDXR
  shellParams_->xr_app_ptr_ = this;

  if (!renderSession_->pre_update()){
    return;
  }
#endif

#if DRAW_UI
  size_t UI_layer_index = 0;

#if ENABLE_CLOUDXR
    const bool draw_ui = useQuadLayerCompositionForUI_ && !should_override_eye_poses_;
#else
    const bool draw_ui = useQuadLayerCompositionForUI_;
#endif

#if ENABLE_PASSTHROUGH
  if (passthroughEnabled())
  {
    UI_layer_index = 1;
  }
#endif

#endif

  for (size_t layerIndex = 0; layerIndex < compositionLayers_.size(); ++layerIndex) {
    if (!compositionLayers_[layerIndex]->isValid()) {
      continue;
    }

      uint32_t renderPassCount = compositionLayers_[layerIndex]->renderPassesCount();

#if DRAW_UI
        const bool layer_is_UI = (layerIndex == UI_layer_index);

      if (draw_ui && layer_is_UI)
      {
          // UI is mono
          renderPassCount = 1;
      }

      if (layer_is_UI && !draw_ui)
      {
          //continue;
      }
#endif

    for (uint32_t i = 0; i < renderPassCount; ++i) {

      auto surfaceTextures = compositionLayers_[layerIndex]->beginRendering(
          i, views_, viewTransforms_, cameraPositions_, shellParams_->viewParams);

      renderSession_->setCurrentQuadLayer(useQuadLayerComposition_ ? layerIndex : 0);
	  
#if ENABLE_CLOUDXR
      shellParams_->viewParams[0].cameraPosition = cameraPositions_[i];
#endif

#if DRAW_UI
    if (draw_ui && layer_is_UI)
    {
        renderSession_->update_UI(std::move(surfaceTextures));
    }
    else
#endif
    {
        renderSession_->update(std::move(surfaceTextures));
    }

      compositionLayers_[layerIndex]->endRendering(i);
    }
  }

#if ENABLE_CLOUDXR
  renderSession_->post_update();
#endif
}

void XrApp::endFrame(XrFrameState frameState) {

  XrCompositionLayerFlags compositionFlags = XR_COMPOSITION_LAYER_CORRECT_CHROMATIC_ABERRATION_BIT;

  std::vector<const XrCompositionLayerBaseHeader*> layers;
  layers.reserve(1 + compositionLayers_.size() * (useQuadLayerComposition_ ? 2 : 1));

#if ENABLE_CLOUDXR
    size_t cloudxr_layer_index = 0;
#endif

#if ENABLE_PASSTHROUGH
  if (passthroughEnabled()) {
    compositionFlags |= XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;

#if ENABLE_CLOUDXR
    cloudxr_layer_index++;
#endif

  }
#endif

#if DRAW_UI
    size_t UI_layer_index = 0;

#if ENABLE_CLOUDXR
    const bool draw_ui = useQuadLayerCompositionForUI_ && !should_override_eye_poses_;
#else
    const bool draw_ui = useQuadLayerCompositionForUI_;
#endif

  if (draw_ui)
  {
      compositionFlags |= XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
  }

#if ENABLE_CLOUDXR
    cloudxr_layer_index++;
#endif

#endif

#if ENABLE_PASSTHROUGH
  if (passthroughEnabled()) {
    passthrough_->injectLayer(layers);

#if DRAW_UI
    UI_layer_index++;
#endif

#if ENABLE_CLOUDXR
    cloudxr_layer_index++;
#endif
  }
#endif

  const auto& appParams = renderSession_->appParams();


  for (int layer_index = 0; layer_index < (int)compositionLayers_.size(); layer_index++)
  {
#if DRAW_UI
      const bool layer_is_UI = (layer_index == UI_layer_index);

      if (layer_is_UI && !draw_ui)
      {
          continue;
      }
#endif

    const std::unique_ptr<XrComposition>& layer = compositionLayers_[layer_index];

    if (layer->isValid())
    {
#if ENABLE_CLOUDXR
        const bool is_cloudxr_layer = (layer_index == cloudxr_layer_index);

        if (should_override_eye_poses_ && is_cloudxr_layer)
        {
            std::array<XrPosef, XrComposition::kNumViews> viewStagePoseOverrides = viewStagePoses_;

            for (int view = 0; view < XrComposition::kNumViews; view++)
            {
                viewStagePoseOverrides[view] = override_eye_poses_[view];
            }

            layer->doComposition(appParams.depthParams, views_, viewStagePoseOverrides, currentSpace_, compositionFlags, layers);
        }
        else if (should_override_eye_poses_ && !is_cloudxr_layer)
        {
            // Don't bother adding the other layers.
            continue;
        }
        else
#endif
        {
            layer->doComposition(appParams.depthParams, views_, viewStagePoses_, currentSpace_, compositionFlags, layers);
        }
    }
  }

  const XrFrameEndInfo endFrameInfo{
      .type = XR_TYPE_FRAME_END_INFO,
      .next = nullptr,
      .displayTime = frameState.predictedDisplayTime,
      .environmentBlendMode = additiveBlendingSupported_ ? XR_ENVIRONMENT_BLEND_MODE_ADDITIVE : XR_ENVIRONMENT_BLEND_MODE_OPAQUE,
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
  //pollActions();
  render();
  endFrame(frameState);
}

void XrApp::pollActions() {
    if (!initialized_ || !resumed_ || !sessionActive_ || !enableActionPolling_) {
        return;
    }

#if ENABLE_CONTROLLERS
    const XrActiveActionSet activeActionSet{ok_inputs_.actionSet, XR_NULL_PATH};
    XrActionsSyncInfo syncInfo{XR_TYPE_ACTIONS_SYNC_INFO};
    syncInfo.countActiveActionSets = 1;
    syncInfo.activeActionSets = &activeActionSet;
    XR_CHECK(xrSyncActions(session_, &syncInfo));
#endif
}

bool XrApp::passthroughSupported() const noexcept { // NOLINT(bugprone-exception-escape)
#if ENABLE_PASSTHROUGH
  return supportedOptionalXrExtensions_.count(XR_FB_PASSTHROUGH_EXTENSION_NAME) != 0;
#else
  return false;
#endif
}

bool XrApp::passthroughEnabled() const noexcept { // NOLINT(bugprone-exception-escape)
#if ENABLE_PASSTHROUGH
  if (!renderSession_ || !passthrough_) {
    return false;
  }
  const auto& appParams = renderSession_->appParams();
  return appParams.passthroughGetter ? appParams.passthroughGetter() : useQuadLayerComposition_;
#else
  return false;
#endif
}

bool XrApp::handTrackingSupported() const noexcept { // NOLINT(bugprone-exception-escape)
#if IGL_PLATFORM_ANDROID
  return supportedOptionalXrExtensions_.count(XR_EXT_HAND_TRACKING_EXTENSION_NAME) != 0 &&
         handTrackingSystemProps_.supportsHandTracking != 0u;
#endif // IGL_PLATFORM_ANDROID
  return false;
}

bool XrApp::handTrackingMeshSupported() const noexcept { // NOLINT(bugprone-exception-escape)
#if IGL_PLATFORM_ANDROID
  //return supportedOptionalXrExtensions_.count(XR_FB_HAND_TRACKING_MESH_EXTENSION_NAME) != 0;
    return false;
#endif // IGL_PLATFORM_ANDROID
  return false;
}

bool XrApp::refreshRateExtensionSupported() const noexcept { // NOLINT(bugprone-exception-escape)
  return supportedOptionalXrExtensions_.count(XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME) != 0;
}

bool XrApp::instanceCreateInfoAndroidSupported()
    const noexcept { // NOLINT(bugprone-exception-escape)
#if IGL_PLATFORM_ANDROID
  return supportedOptionalXrExtensions_.count(XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME) != 0;
#endif // IGL_PLATFORM_ANDROID
  return false;
}

bool XrApp::alphaBlendCompositionSupported() const noexcept { // NOLINT(bugprone-exception-escape)
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
#if ENABLE_META_OPENXR_FEATURES
  return supportedOptionalXrExtensions_.count(XR_META_BODY_TRACKING_FULL_BODY_EXTENSION_NAME) != 0;
#else
    return false;
#endif
}

bool XrApp::metaBodyTrackingFidelitySupported() const noexcept {
#if ENABLE_META_OPENXR_FEATURES
  return supportedOptionalXrExtensions_.count(XR_META_BODY_TRACKING_FIDELITY_EXTENSION_NAME) != 0;
#else
    return false;
#endif
}

bool XrApp::simultaneousHandsAndControllersSupported() const noexcept {
#if ENABLE_META_OPENXR_FEATURES
  return supportedOptionalXrExtensions_.count(XR_META_SIMULTANEOUS_HANDS_AND_CONTROLLERS_EXTENSION_NAME) != 0;
#else
    return false;
#endif
}

bool XrApp::eyeTrackingSocialFBSupported() const noexcept {
#if ENABLE_META_OPENXR_FEATURES
  return supportedOptionalXrExtensions_.count(XR_FB_EYE_TRACKING_SOCIAL_EXTENSION_NAME) != 0;
#else
    return false;
#endif
}

bool XrApp::htcViveFocus3ControllersSupported() const noexcept {
#if ENABLE_META_OPENXR_FEATURES
  return supportedOptionalXrExtensions_.count(XR_HTC_VIVE_FOCUS3_CONTROLLER_INTERACTION_EXTENSION_NAME) != 0;
#else
    return false;
#endif
}

bool XrApp::byteDanceControllersSupported() const noexcept {
#if ENABLE_META_OPENXR_FEATURES
  return supportedOptionalXrExtensions_.count(XR_BD_CONTROLLER_INTERACTION_EXTENSION_NAME) != 0;
#else
    return false;
#endif
}

#if SUPPORT_CLOUDXR_LINK_SHARPENING
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
#endif

#if SUPPORT_OPENXR_SIMULTANEOUS_HANDS_AND_CONTROLLERS
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
