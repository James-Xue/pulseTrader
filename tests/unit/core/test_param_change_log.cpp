// test_param_change_log.cpp — parameter-change audit ring buffer

#include "core/ParamChangeLog.hpp"

#include <gtest/gtest.h>

#include <thread>
#include <vector>

using namespace pulse::core;

namespace
{

ParamChangeEntry make_entry(std::int64_t ts, const std::string &source)
{
    return { ts, "momentum_scalper_BTC_USDT", "min_confidence", 0.6, 0.7, source };
}

} // anonymous namespace

TEST(ParamChangeLog, RecordAndSnapshotRoundTrip)
{
    ParamChangeLog log;
    log.record(make_entry(100, "ai"));
    log.record(make_entry(200, "manual"));
    log.record(make_entry(300, "ai"));

    const auto snap = log.snapshot();
    ASSERT_EQ(3u, snap.size());
    EXPECT_EQ(300, snap[0].ts_ns);  // newest first
    EXPECT_EQ("ai", snap[0].source);
    EXPECT_EQ("manual", snap[1].source);
    EXPECT_EQ(100, snap[2].ts_ns);
    EXPECT_EQ("min_confidence", snap[0].param_name);
    EXPECT_DOUBLE_EQ(0.6, snap[0].old_value);
    EXPECT_DOUBLE_EQ(0.7, snap[0].new_value);
    EXPECT_EQ("momentum_scalper_BTC_USDT", snap[0].strategy_id);
}

TEST(ParamChangeLog, RingCapacityEvictsOldest)
{
    ParamChangeLog log{ 256 };
    for (std::int64_t i = 0; i < 300; ++i)
    {
        log.record(make_entry(i, "ai"));
    }

    EXPECT_EQ(256u, log.size());
    EXPECT_EQ(256u, log.capacity());
    const auto snap = log.snapshot();
    EXPECT_EQ(299, snap.front().ts_ns); // newest kept
    EXPECT_EQ(44, snap.back().ts_ns);   // oldest 44 entries evicted
}

TEST(ParamChangeLog, SnapshotNewestFirst)
{
    ParamChangeLog log;
    for (std::int64_t i = 0; i < 5; ++i)
    {
        log.record(make_entry(i, "manual"));
    }
    const auto snap = log.snapshot();
    ASSERT_EQ(5u, snap.size());
    for (std::size_t i = 0; i < snap.size(); ++i)
    {
        EXPECT_EQ(static_cast<std::int64_t>(snap.size() - 1 - i), snap[i].ts_ns);
    }
}

TEST(ParamChangeLog, ConcurrentRecordIsSafe)
{
    ParamChangeLog log{ 256 };
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t)
    {
        threads.emplace_back([&log]()
        {
            for (int i = 0; i < 500; ++i)
            {
                log.record(make_entry(i, "ai"));
            }
        });
    }
    for (auto &th : threads)
    {
        th.join();
    }

    EXPECT_EQ(256u, log.size()); // bounded, no crash
    const auto snap = log.snapshot();
    EXPECT_EQ(256u, snap.size());
}
