/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <array>
#include <optional>
#include <vector>

#include <igl/ColorSpace.h>
#include <igl/Common.h>
#include <igl/TextureFormat.h>
#include <shell/shared/renderSession/Hands.h>
#include <shell/shared/renderSession/RenderMode.h>
#include <shell/shared/renderSession/ViewParams.h>

namespace igl::shell {

namespace openxr {
    class XrApp;
}

struct ShellParams {
  std::vector<ViewParams> viewParams;
  RenderMode renderMode = RenderMode::Mono;
  bool shellControlsViewParams = false;
  bool rightHandedCoordinateSystem = false;
  glm::vec2 viewportSize = glm::vec2(1024.0f, 768.0f);
  glm::ivec2 nativeSurfaceDimensions = glm::ivec2(2048, 1536);
  igl::TextureFormat defaultColorFramebufferFormat = igl::TextureFormat::BGRA_SRGB;
  igl::ColorSpace swapchainColorSpace = igl::ColorSpace::SRGB_NONLINEAR;
  float viewportScale = 1.f;
  bool shouldPresent = true;
  std::optional<igl::Color> clearColorValue = {};
  std::array<HandMesh, 2> handMeshes = {};
  std::array<HandTracking, 2> handTracking = {};

  int current_view_id_ = 0;
  openxr::XrApp* xr_app_ptr_ = nullptr; // horrible hack but CloudXR needs to poll the Xr State from another thread at >> higher Hz than rendering.
};
} // namespace igl::shell
