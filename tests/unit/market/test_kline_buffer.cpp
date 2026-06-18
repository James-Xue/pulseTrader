// test_kline_buffer.cpp — Unit tests for KlineBuffer (Layer 3 Market Data)

#include "market/kline_buffer.hpp"

#include <gtest/gtest.h>

#include <thread>
#include <vector>

using namespace pulse;
using namespace pulse::market;

// ---------------------------------------------------------------------------
// Basic functionality
// ---------------------------------------------------------------------------

TEST(KlineBuffer, EmptyBufferReturnsEmptySnapshot)
{
    KlineBuffer buffer(100);
    const auto snap = buffer.snapshot();
    EXPECT_TRUE(snap.empty());
}

TEST(KlineBuffer, EmptyBufferReturnsNulloptLatest)
{
    KlineBuffer buffer(100);
    EXPECT_FALSE(buffer.latest().has_value());
}

TEST(KlineBuffer, PushAndRetrieveSingle)
{
    KlineBuffer buffer(100);

    Kline k;
    k.open_time = 1000;
    k.close_time = 2000;
    k.open = 100.0;
    k.high = 110.0;
    k.low = 95.0;
    k.close = 105.0;
    k.volume = 50.0;
    k.closed = true;

    buffer.push(k);

    EXPECT_EQ(buffer.size(), 1u);
    const auto latest = buffer.latest();
    ASSERT_TRUE(latest.has_value());
    EXPECT_DOUBLE_EQ(latest->close, 105.0);
}

TEST(KlineBuffer, PushMultipleAndSnapshot)
{
    KlineBuffer buffer(100);

    // Push 5 candles.
    for (int i = 0; i < 5; ++i)
    {
        Kline k;
        k.open_time = i * 1000;
        k.close = static_cast<double>(i + 1) * 10.0;
        k.closed = true;
        buffer.push(k);
    }

    EXPECT_EQ(buffer.size(), 5u);

    // Snapshot all.
    const auto snap = buffer.snapshot();
    ASSERT_EQ(snap.size(), 5u);

    // Verify chronological order (oldest first).
    EXPECT_DOUBLE_EQ(snap[0].close, 10.0);
    EXPECT_DOUBLE_EQ(snap[4].close, 50.0);
}

TEST(KlineBuffer, SnapshotPartial)
{
    KlineBuffer buffer(100);

    for (int i = 0; i < 10; ++i)
    {
        Kline k;
        k.close = static_cast<double>(i);
        buffer.push(k);
    }

    // Request last 3 candles.
    const auto snap = buffer.snapshot(3);
    ASSERT_EQ(snap.size(), 3u);
    EXPECT_DOUBLE_EQ(snap[0].close, 7.0);
    EXPECT_DOUBLE_EQ(snap[1].close, 8.0);
    EXPECT_DOUBLE_EQ(snap[2].close, 9.0);
}

// ---------------------------------------------------------------------------
// Ring buffer wrap-around
// ---------------------------------------------------------------------------

TEST(KlineBuffer, RingBufferWrapAround)
{
    // Capacity 5, push 8 candles — oldest 3 are overwritten.
    KlineBuffer buffer(5);

    for (int i = 0; i < 8; ++i)
    {
        Kline k;
        k.close = static_cast<double>(i);
        buffer.push(k);
    }

    EXPECT_EQ(buffer.size(), 5u);

    // Snapshot should return candles 3, 4, 5, 6, 7 (not 0, 1, 2).
    const auto snap = buffer.snapshot();
    ASSERT_EQ(snap.size(), 5u);
    EXPECT_DOUBLE_EQ(snap[0].close, 3.0);
    EXPECT_DOUBLE_EQ(snap[1].close, 4.0);
    EXPECT_DOUBLE_EQ(snap[2].close, 5.0);
    EXPECT_DOUBLE_EQ(snap[3].close, 6.0);
    EXPECT_DOUBLE_EQ(snap[4].close, 7.0);
}

TEST(KlineBuffer, LatestAfterWrapAround)
{
    KlineBuffer buffer(3);

    for (int i = 0; i < 5; ++i)
    {
        Kline k;
        k.close = static_cast<double>(i);
        buffer.push(k);
    }

    const auto latest = buffer.latest();
    ASSERT_TRUE(latest.has_value());
    EXPECT_DOUBLE_EQ(latest->close, 4.0);
}

// ---------------------------------------------------------------------------
// Thread safety (seqlock)
// ---------------------------------------------------------------------------

TEST(KlineBuffer, ConcurrentPushAndSnapshot)
{
    // Writer thread pushes candles, reader thread takes snapshots.
    // Seqlock must prevent torn reads.
    KlineBuffer buffer(100);
    constexpr int kNumPushes = 1000;
    std::atomic<bool> stop{ false };

    // Writer thread.
    std::thread writer([&buffer, &stop]()
    {
        for (int i = 0; i < kNumPushes && !stop.load(); ++i)
        {
            Kline k;
            k.close = static_cast<double>(i);
            k.open_time = i;
            buffer.push(k);
        }
    });

    // Reader thread.
    std::thread reader([&buffer, &stop]()
    {
        int snapshots_taken = 0;
        while (snapshots_taken < 100 && !stop.load())
        {
            const auto snap = buffer.snapshot(10);
            // Snapshot must be consistent: all candles have close >= 0.
            for (const auto &k : snap)
            {
                EXPECT_GE(k.close, 0.0);
            }
            ++snapshots_taken;
        }
    });

    writer.join();
    stop.store(true);
    reader.join();

    // Final size should be min(kNumPushes, capacity).
    EXPECT_EQ(buffer.size(), std::min<std::size_t>(kNumPushes, 100));
}

// ---------------------------------------------------------------------------
// Edge cases
// ---------------------------------------------------------------------------

TEST(KlineBuffer, SnapshotRequestMoreThanAvailable)
{
    KlineBuffer buffer(100);

    for (int i = 0; i < 3; ++i)
    {
        Kline k;
        k.close = static_cast<double>(i);
        buffer.push(k);
    }

    // Request 10, but only 3 available.
    const auto snap = buffer.snapshot(10);
    ASSERT_EQ(snap.size(), 3u);
}

TEST(KlineBuffer, CapacityIsRespected)
{
    KlineBuffer buffer(5);
    EXPECT_EQ(buffer.capacity(), 5u);

    for (int i = 0; i < 10; ++i)
    {
        Kline k;
        buffer.push(k);
    }

    EXPECT_EQ(buffer.size(), 5u);
    EXPECT_EQ(buffer.capacity(), 5u);
}
