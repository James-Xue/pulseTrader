// test_single_instance_guard.cpp — Unit tests for pulse/SingleInstanceGuard
//
// Test coverage:
//   1. First instance acquires the lock; a second instance is refused
//   2. Lock is released on destruction (no stale-lock problem)
//   3. Lock file path parent directories are created on demand

#include <gtest/gtest.h>

#include "core/SingleInstanceGuard.hpp"

#include <filesystem>

using namespace pulse;

namespace
{

std::string tempLockPath()
{
    return (std::filesystem::temp_directory_path()
            / "pulsetrader_single_instance_test.lock")
        .string();
}

} // anonymous namespace

TEST(SingleInstanceGuard, SecondInstanceIsRefused)
{
    const auto path = tempLockPath();
    std::filesystem::remove(path);

    {
        SingleInstanceGuard first(path);
        ASSERT_TRUE(first.acquired());

        // A second guard on the same file must NOT acquire — that is the
        // whole point: no two engines at once.
        SingleInstanceGuard second(path);
        EXPECT_FALSE(second.acquired());
    }

    std::filesystem::remove(path);
}

TEST(SingleInstanceGuard, LockReleasedOnDestruction)
{
    const auto path = tempLockPath();
    std::filesystem::remove(path);

    {
        SingleInstanceGuard first(path);
        ASSERT_TRUE(first.acquired());
    } // first destroyed here — flock released by the kernel

    // A fresh guard must now succeed even though the file still exists.
    SingleInstanceGuard after(path);
    EXPECT_TRUE(after.acquired());

    std::filesystem::remove(path);
}

TEST(SingleInstanceGuard, CreatesParentDirectories)
{
    const auto path = (std::filesystem::temp_directory_path()
                       / "pulsetrader_single_instance_test" / "nested"
                       / "engine.lock")
                          .string();
    std::filesystem::remove_all(
        (std::filesystem::temp_directory_path()
         / "pulsetrader_single_instance_test"));

    {
        SingleInstanceGuard guard(path);
        EXPECT_TRUE(guard.acquired());
        EXPECT_TRUE(std::filesystem::exists(path));
    }

    std::filesystem::remove_all(
        (std::filesystem::temp_directory_path()
         / "pulsetrader_single_instance_test"));
}
