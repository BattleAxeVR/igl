/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <android/native_window_jni.h>

#include <EGL/egl.h>

#ifndef XR_USE_GRAPHICS_API_OPENGL_ES
#define XR_USE_GRAPHICS_API_OPENGL_ES
#endif
#include <openxr/openxr_platform.h>

#include <shell/openxr/impl/XrAppImpl.h>

namespace igl::shell::openxr::mobile {

class XrAppImplGLES final : public impl::XrAppImpl {
 public:
  std::vector<const char*> getXrRequiredExtensions() const override;
  std::unique_ptr<igl::IDevice> initIGL(XrInstance instance, XrSystemId systemId) override;
  XrSession initXrSession(XrInstance instance, XrSystemId systemId, igl::IDevice& device) override;
  std::unique_ptr<impl::XrSwapchainProviderImpl> createSwapchainProviderImpl() const override;
  void* getGraphicsContext() override;

 private:
  XrGraphicsRequirementsOpenGLESKHR graphicsRequirements_ = {
      .type = XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_ES_KHR,
  };

    XrGraphicsBindingOpenGLESAndroidKHR graphicsBindingAndroidGLES_ = {
      .type = XR_TYPE_GRAPHICS_BINDING_OPENGL_ES_ANDROID_KHR,
    };
};
} // namespace igl::shell::openxr::mobile
