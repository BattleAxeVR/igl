/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <igl/metal/Framebuffer.h>

namespace igl::metal::macos {

class Framebuffer final : public ::igl::metal::Framebuffer {
 public:
  explicit Framebuffer(const FramebufferDesc& value);
  ~Framebuffer() override = default;

 private:
  bool canCopy(ICommandQueue& cmdQueue,
               id<MTLTexture> texture,
               const TextureRangeDesc& range) const override;
};

} // namespace igl::metal::macos
