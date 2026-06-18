// test_order_rate_limiter.cpp — Unit tests for OrderRateLimiter (Layer 7 Risk Management)

#include "risk/order_rate_limiter.hpp"

#include <gtest/gtest.h>

#include <thread>
#include <vector>

using namespace pulse::risk;

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

TEST(OrderRateLimiter, DefaultCapacity)
{
    OrderRateLimiter limiter(5);
    EXPECT_DOUBLE_EQ(10.0, limiter.available_tokens()); // burst = 2 * rate.
    EXPECT_FALSE(limiter.is_exhausted());
}

TEST(OrderRateLimiter, CustomBurst)
{
    OrderRateLimiter limiter(5, /*burst_capacity=*/3);
    EXPECT_DOUBLE_EQ(3.0, limiter.available_tokens());
}

// ---------------------------------------------------------------------------
// Basic acquire
// ---------------------------------------------------------------------------

TEST(OrderRateLimiter, AcquireWithinCapacitySucceeds)
{
    OrderRateLimiter limiter(5, /*burst_capacity=*/5);

    // Should be able to acquire 5 tokens.
    for (int i = 0; i < 5; ++i)
    {
        EXPECT_TRUE(limiter.try_acquire());
    }
}

TEST(OrderRateLimiter, AcquireBeyondCapacityFails)
{
    OrderRateLimiter limiter(5, /*burst_capacity=*/3);

    EXPECT_TRUE(limiter.try_acquire());
    EXPECT_TRUE(limiter.try_acquire());
    EXPECT_TRUE(limiter.try_acquire());
    EXPECT_FALSE(limiter.try_acquire()); // Bucket empty.
}

TEST(OrderRateLimiter, IsExhaustedWhenEmpty)
{
    OrderRateLimiter limiter(5, /*burst_capacity=*/2);

    EXPECT_FALSE(limiter.is_exhausted());
    EXPECT_TRUE(limiter.try_acquire());
    EXPECT_TRUE(limiter.try_acquire());
    EXPECT_TRUE(limiter.is_exhausted());
}

// ---------------------------------------------------------------------------
// Refill
// ---------------------------------------------------------------------------

TEST(OrderRateLimiter, TokensRefillOverTime)
{
    OrderRateLimiter limiter(/*rate=*/100, /*burst=*/100);

    // Drain all tokens.
    for (int i = 0; i < 100; ++i)
    {
        (void)limiter.try_acquire();
    }

    // Wait for some tokens to refill (~50ms should add ~5 tokens at 100/sec).
    std::this_thread::sleep_for(std::chrono::milliseconds(60));

    // try_acquire() triggers refill internally, so this should succeed.
    EXPECT_TRUE(limiter.try_acquire());
}

TEST(OrderRateLimiter, TokensCappedAtBurstCapacity)
{
    OrderRateLimiter limiter(/*rate=*/100, /*burst=*/5);

    // Wait longer than needed to fill the bucket.
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Tokens should be capped at burst capacity.
    EXPECT_LE(limiter.available_tokens(), 5.0 + 1.0); // Small tolerance for timing.
}

// ---------------------------------------------------------------------------
// Burst
// ---------------------------------------------------------------------------

TEST(OrderRateLimiter, BurstAllowsShortBurst)
{
    OrderRateLimiter limiter(/*rate=*/5, /*burst=*/10);

    // Should be able to burst 10 orders.
    int acquired = 0;
    for (int i = 0; i < 15; ++i)
    {
        if (limiter.try_acquire())
        {
            ++acquired;
        }
    }

    EXPECT_EQ(10, acquired); // Only burst capacity.
}

TEST(OrderRateLimiter, BurstRecoversAfterDrain)
{
    OrderRateLimiter limiter(/*rate=*/100, /*burst=*/10);

    // Drain.
    for (int i = 0; i < 10; ++i)
    {
        (void)limiter.try_acquire();
    }

    // Wait for partial refill.
    std::this_thread::sleep_for(std::chrono::milliseconds(60));

    // Should have recovered some tokens.
    int acquired = 0;
    for (int i = 0; i < 10; ++i)
    {
        if (limiter.try_acquire())
        {
            ++acquired;
        }
    }
    EXPECT_GT(acquired, 0);
}

// ---------------------------------------------------------------------------
// Reset
// ---------------------------------------------------------------------------

TEST(OrderRateLimiter, ResetRestoresCapacity)
{
    OrderRateLimiter limiter(5, /*burst=*/5);

    // Drain.
    for (int i = 0; i < 5; ++i)
    {
        EXPECT_TRUE(limiter.try_acquire());
    }
    EXPECT_TRUE(limiter.is_exhausted());

    limiter.reset();
    EXPECT_FALSE(limiter.is_exhausted());
    EXPECT_DOUBLE_EQ(5.0, limiter.available_tokens());
}

// ---------------------------------------------------------------------------
// Thread safety
// ---------------------------------------------------------------------------

TEST(OrderRateLimiter, ConcurrentAcquiresAreSerialized)
{
    constexpr std::uint32_t kBurst = 20;
    OrderRateLimiter limiter(/*rate=*/0, /*burst=*/kBurst); // Zero rate = no refill.

    std::atomic<int> acquired_count{ 0 };
    constexpr int kThreadCount = 4;
    constexpr int kAttemptsPerThread = 10;

    std::vector<std::jthread> threads;
    for (int t = 0; t < kThreadCount; ++t)
    {
        threads.emplace_back([&limiter, &acquired_count]()
        {
            for (int i = 0; i < kAttemptsPerThread; ++i)
            {
                if (limiter.try_acquire())
                {
                    acquired_count.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }

    threads.clear(); // Join all.

    // Total acquired should not exceed burst capacity (20 tokens, no refill).
    EXPECT_LE(acquired_count.load(), static_cast<int>(kBurst));
    // All tokens should have been consumed.
    EXPECT_EQ(static_cast<int>(kBurst), acquired_count.load());
}

TEST(OrderRateLimiter, ConcurrentAcquireAndReset)
{
    OrderRateLimiter limiter(/*rate=*/100, /*burst=*/10);

    std::atomic<bool> stop{ false };

    std::jthread acquirer([&limiter, &stop]()
    {
        while (!stop.load())
        {
            (void)limiter.try_acquire();
        }
    });

    std::jthread resetter([&limiter, &stop]()
    {
        while (!stop.load())
        {
            limiter.reset();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    stop.store(true);
    // Should complete without crash.
}
