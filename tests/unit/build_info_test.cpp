// Otok (c) Alexander A Pototskiy
// SPDX-License-Identifier: GPL-3.0-only WITH AdditionRef-otok-output-exception

#include <gtest/gtest.h>

#include <filesystem>

#include "otok/core/build_info.hpp"

TEST(build_info, is_set)
{
    EXPECT_FALSE(otok::build_info::version.empty());
    EXPECT_FALSE(otok::build_info::copyright_years.empty());
    EXPECT_FALSE(otok::build_info::revision().empty());
}

TEST(test_setup, data_dir_exists)
{
    EXPECT_TRUE(std::filesystem::is_directory(OTOK_TEST_DATA_DIR)) << OTOK_TEST_DATA_DIR;
}
