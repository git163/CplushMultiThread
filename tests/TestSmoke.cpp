// tests/TestSmoke.cpp — 验证 GTest 是否正确接入。
// 有了第一个真实测试后可以删除此文件。

#include <gtest/gtest.h>

TEST(SmokeTest, TrivialAssertion) {
    EXPECT_EQ(1 + 1, 2);
}