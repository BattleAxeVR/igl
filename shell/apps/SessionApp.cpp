/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */


#if defined(IGL_SHELL_SESSION_EXTERNAL)
#define IGL_SHELL_SESSION IGL_SHELL_SESSION_EXTERNAL
#define IGL_SHELL_PATH <../../../../src/renderSessions/IGL_SHELL_SESSION.h>
#elif defined(IGL_SHELL_SESSION)
#define IGL_SHELL_PATH <shell/renderSessions/IGL_SHELL_SESSION.h>
#else
#error "IGL_SHELL_SESSION must be defined";
#endif

#include IGL_SHELL_PATH
#include <shell/shared/renderSession/DefaultSession.h>

namespace igl::shell {

std::unique_ptr<RenderSession> createDefaultRenderSession(std::shared_ptr<Platform> platform) {
  return std::make_unique<IGL_SHELL_SESSION>(std::move(platform));
}

} // namespace igl::shell
