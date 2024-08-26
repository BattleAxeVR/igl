/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>
#include <igl/IGL.h>
#include <string>

namespace igl::tests {

TEST(CommonTest, BackendTypeToStringTest) {
  ASSERT_EQ(BackendTypeToString(BackendType::Invalid), "Invalid");
  ASSERT_EQ(BackendTypeToString(BackendType::OpenGL), "OpenGL");
  ASSERT_EQ(BackendTypeToString(BackendType::Metal), "Metal");
  ASSERT_EQ(BackendTypeToString(BackendType::Vulkan), "Vulkan");
  // @fb-only
};

TEST(CommonTest, ColorTest) {
  const Color testColor(1.0f, 0.5f, 0.0f);
  ASSERT_EQ(testColor.r, 1.0f);
  ASSERT_EQ(testColor.g, 0.5f);
  ASSERT_EQ(testColor.b, 0.0f);
  ASSERT_EQ(testColor.a, 1.0f);

  const Color testColor2(1.0f, 0.5f, 0.0f, 1.0f);

  ASSERT_EQ(testColor2.r, 1.0f);
  ASSERT_EQ(testColor2.g, 0.5f);
  ASSERT_EQ(testColor2.b, 0.0f);
  ASSERT_EQ(testColor2.a, 1.0f);

  const auto* floatPtr = testColor.toFloatPtr();
  ASSERT_EQ(floatPtr[0], 1.0f);
  ASSERT_EQ(floatPtr[1], 0.5f);
  ASSERT_EQ(floatPtr[2], 0.0f);
  ASSERT_EQ(floatPtr[3], 1.0f);
};

TEST(CommonTest, ResultTest) {
  Result testResult;
  Result testResult2(Result::Code::Ok, "test message2");
  Result testResult3(Result::Code::Ok, std::string("test message3"));
  ASSERT_STREQ(testResult2.message.c_str(), "test message2");
  ASSERT_TRUE(testResult2.isOk());
  ASSERT_STREQ(testResult3.message.c_str(), "test message3");
  ASSERT_TRUE(testResult3.isOk());

  Result::setResult(&testResult, Result::Code::ArgumentInvalid, std::string("new test message"));
  ASSERT_STREQ(testResult.message.c_str(), "new test message");
  ASSERT_FALSE(testResult.isOk());

  Result::setResult(&testResult3, testResult);
  ASSERT_FALSE(testResult3.isOk());

  Result::setResult(&testResult2, std::move(testResult));
  ASSERT_FALSE(testResult2.isOk());
};

TEST(CommonTest, RectTest) {
  const ScissorRect testRect;
  ASSERT_TRUE(testRect.isNull());
  const ScissorRect testRect2{0, 0, 1, 1};
  ASSERT_FALSE(testRect2.isNull());
};
} // namespace igl::tests
