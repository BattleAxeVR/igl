/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <EGL/egl.h>
#include <igl/Texture.h>
#include <igl/opengl/GLIncludes.h>
#include <igl/opengl/PlatformDevice.h>

namespace igl {
namespace opengl {

class ViewTextureTarget;

namespace egl {

class Device;
class Context;

class PlatformDevice : public opengl::PlatformDevice {
 public:
  static constexpr igl::PlatformDeviceType Type = igl::PlatformDeviceType::OpenGLEgl;

  PlatformDevice(Device& owner);
  ~PlatformDevice() override = default;

  /// Returns a texture representing the EGL Surface associated with this device's context.
  std::shared_ptr<ITexture> createTextureFromNativeDrawable(Result* outResult);

  std::shared_ptr<ITexture> createTextureFromNativeDrawable(int width,
                                                            int height,
                                                            Result* outResult);

  /// Returns a texture representing the EGL depth texture associated with this device's context.
  std::shared_ptr<ITexture> createTextureFromNativeDepth(Result* outResult);

  /// This function must be called every time the currently bound EGL read and/or draw surfaces
  /// change, in order to notify IGL of these changes.
  void updateSurfaces(EGLSurface readSurface, EGLSurface drawSurface, Result* outResult);

  EGLSurface createSurface(NativeWindowType nativeWindow, Result* outResult);

  EGLSurface getReadSurface(Result* outResult);

  void setPresentationTime(long long presentationTimeNs, Result* outResult);

 protected:
  bool isType(PlatformDeviceType t) const noexcept override;

 private:
  std::shared_ptr<ViewTextureTarget> drawableTexture_;

  std::pair<EGLint, EGLint> getSurfaceDimensions(const Context& context, Result* outResult);
};

} // namespace egl
} // namespace opengl
} // namespace igl
