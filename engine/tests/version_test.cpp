#include "finforge/version.hpp"

#include <gtest/gtest.h>

TEST(Version, HasSemverGitShaAndBuildType) {
    const std::string v = finforge::engine_version();
    const auto plus = v.find('+');
    ASSERT_NE(plus, std::string::npos) << v;
    EXPECT_EQ(v.substr(0, plus), "0.1.0");
    EXPECT_NE(v.find('.', plus), std::string::npos) << "missing build type: " << v;
}
