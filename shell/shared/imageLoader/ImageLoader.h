/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <IGLU/texture_loader/IData.h>
#include <IGLU/texture_loader/TextureLoaderFactory.h>
#include <igl/IGL.h>
#include <memory>
#include <string>
#include <vector>

namespace igl::shell {

class FileLoader;

struct ImageData {
  TextureDesc desc;
  std::unique_ptr<iglu::textureloader::IData> data;
};

class ImageLoader {
 public:
  explicit ImageLoader(FileLoader& fileLoader);
  virtual ~ImageLoader() = default;
  virtual ImageData loadImageData(std::string /*imageName*/) noexcept {
    return checkerboard();
  }

 protected:
  [[nodiscard]] const FileLoader& fileLoader() const noexcept {
    return fileLoader_;
  }

  [[nodiscard]] FileLoader& fileLoader() noexcept {
    return fileLoader_;
  }

 protected:
  [[nodiscard]] ImageData loadImageDataFromFile(const std::string& fileName) noexcept;
  [[nodiscard]] ImageData loadImageDataFromMemory(const uint8_t* data, uint32_t length) noexcept;

 private:
  static ImageData checkerboard() noexcept;

  FileLoader& fileLoader_;
  std::unique_ptr<iglu::textureloader::TextureLoaderFactory> factory_;
};

} // namespace igl::shell
