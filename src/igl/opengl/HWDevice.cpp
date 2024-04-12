/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <igl/opengl/HWDevice.h>

#include <igl/opengl/IContext.h>

namespace igl {
namespace opengl {

std::vector<HWDeviceDesc> HWDevice::queryDevices(const HWDeviceQueryDesc& /*desc*/,
                                                 Result* outResult) {
  std::vector<HWDeviceDesc> devices;

  HWDeviceDesc defaultDevice(1L, HWDeviceType::DiscreteGpu, 0, "Default");
  devices.push_back(defaultDevice);

  Result::setOk(outResult);
  return devices;
}

std::unique_ptr<IDevice> HWDevice::create(const HWDeviceDesc& /*desc*/,
                                          RenderingAPI api,
                                          EGLNativeWindowType nativeWindow,
                                          Result* outResult) {
  auto context = createContext(api, nativeWindow, outResult);
  //auto context = createOffscreenContext(api, 1920, 1920, outResult);

  if (!context) {
    Result::setResult(outResult, Result::Code::RuntimeError, "context is null");
    return nullptr;
  }

  return createWithContext(std::move(context), outResult);
}

} // namespace opengl
} // namespace igl
