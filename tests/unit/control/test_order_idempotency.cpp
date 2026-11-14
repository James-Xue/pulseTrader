// test_order_idempotency.cpp — duplicate suppression for open_order
//
// The cache is a pure in-memory state machine: no REST, no exchange, no
// engine. These tests drive begin()/complete()/abort() directly so a failure
// names the transition that broke.

#include "control/OrderIdempotency.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <barrier>
#include <chrono>
#include <thread>
#include <vector>

using namespace pulse;
using namespace pulse::control;
using namespace pulse::execution;

namespace
{

/// A successful placement as the order flow would return it.
Result<OrderResponse> successResponse(const std::string &order_id)
{
    OrderResponse resp;
    resp.order_id = order_id;
    resp.status = OrderStatus::Open;
    return resp;
}

} // anonymous namespace

TEST(OrderIdempotency, CompletedSuccessReplaysVerbatim)
{
    OrderIdempotency cache;

    EXPECT_EQ(OrderIdempotency::Decision::New, cache.begin("gt-1", "fp-a"));
    cache.complete("gt-1", successResponse("order-42"));

    // A retry with the same parameters gets the first attempt's answer.
    EXPECT_EQ(OrderIdempotency::Decision::Replay,
              cache.begin("gt-1", "fp-a"));

    const auto cached = cache.cachedOutcome("gt-1");
    ASSERT_TRUE(cached.has_value());
    ASSERT_TRUE(ok(*cached));
    EXPECT_EQ("order-42", value(*cached).order_id);
    EXPECT_EQ(OrderStatus::Open, value(*cached).status);

    // Replaying is not a state change: it stays replayable.
    EXPECT_EQ(OrderIdempotency::Decision::Replay,
              cache.begin("gt-1", "fp-a"));
}

TEST(OrderIdempotency, CachedPulseErrorIsReplayed)
{
    // A rejection is an outcome too. Re-executing it would send a second
    // order on every client retry of a failing request.
    OrderIdempotency cache;

    EXPECT_EQ(OrderIdempotency::Decision::New, cache.begin("gt-2", "fp"));
    cache.complete("gt-2",
                   PulseError{ ErrorCode::InsufficientBalance,
                               "not enough margin" });

    EXPECT_EQ(OrderIdempotency::Decision::Replay, cache.begin("gt-2", "fp"));

    const auto cached = cache.cachedOutcome("gt-2");
    ASSERT_TRUE(cached.has_value());
    ASSERT_FALSE(ok(*cached));
    EXPECT_EQ(ErrorCode::InsufficientBalance, error(*cached).code);
    EXPECT_EQ("not enough margin", error(*cached).message);
}

TEST(OrderIdempotency, SecondBeginWhileInFlightIsRejected)
{
    OrderIdempotency cache;

    EXPECT_EQ(OrderIdempotency::Decision::New, cache.begin("gt-3", "fp"));
    EXPECT_EQ(OrderIdempotency::Decision::InFlight,
              cache.begin("gt-3", "fp"));

    // Nothing has finished yet, so there is nothing to replay.
    EXPECT_FALSE(cache.cachedOutcome("gt-3").has_value());
}

TEST(OrderIdempotency, DifferentFingerprintAfterCompletionConflicts)
{
    OrderIdempotency cache;

    EXPECT_EQ(OrderIdempotency::Decision::New, cache.begin("gt-4", "fp-a"));
    cache.complete("gt-4", successResponse("order-7"));

    EXPECT_EQ(OrderIdempotency::Decision::Conflict,
              cache.begin("gt-4", "fp-b"));
    // The conflict did not overwrite the original outcome.
    const auto cached = cache.cachedOutcome("gt-4");
    ASSERT_TRUE(cached.has_value());
    ASSERT_TRUE(ok(*cached));
    EXPECT_EQ("order-7", value(*cached).order_id);
}

TEST(OrderIdempotency, AbortReleasesTheKeyForRetry)
{
    // abort() is the "failed before anything was sent" path — the key must
    // become usable again, with no cached outcome left behind.
    OrderIdempotency cache;

    EXPECT_EQ(OrderIdempotency::Decision::New, cache.begin("gt-5", "fp"));
    cache.abort("gt-5");

    EXPECT_FALSE(cache.cachedOutcome("gt-5").has_value());
    EXPECT_EQ(OrderIdempotency::Decision::New, cache.begin("gt-5", "fp"));
}

TEST(OrderIdempotency, ZeroTtlExpiresEveryEntry)
{
    // TTL is a constructor parameter precisely so expiry is observable
    // without sleeping: a 0 s TTL makes an entry stale the moment it is
    // inserted, so the very next call treats the key as a first sighting.
    OrderIdempotency cache{ std::chrono::seconds{ 0 } };

    EXPECT_EQ(OrderIdempotency::Decision::New, cache.begin("gt-6", "fp"));
    cache.complete("gt-6", successResponse("order-1"));
    EXPECT_EQ(OrderIdempotency::Decision::New, cache.begin("gt-6", "fp"));
}

TEST(OrderIdempotency, RefusesNewKeyWhenFullInsteadOfEvicting)
{
    // The cap is enforced fail-closed: dropping a live entry would either
    // remove an in-flight guard (allowing a duplicate order) or shrink the
    // replay window (allowing a late retry to place again). A full cache
    // therefore refuses the NEW key and keeps every old outcome replayable.
    OrderIdempotency cache{ std::chrono::seconds{ 600 }, 3 };

    for (const auto *key : { "k0", "k1", "k2" })
    {
        EXPECT_EQ(OrderIdempotency::Decision::New, cache.begin(key, "fp"));
        cache.complete(key, successResponse(key));
    }
    EXPECT_EQ(3U, cache.size());

    // Fourth key: no eviction, a retryable refusal.
    EXPECT_EQ(OrderIdempotency::Decision::Capacity, cache.begin("k3", "fp"));
    EXPECT_EQ(3U, cache.size());

    // Every completed outcome is still replayable — including the oldest one
    // an eviction policy would have picked first.
    EXPECT_EQ(OrderIdempotency::Decision::Replay, cache.begin("k0", "fp"));
    EXPECT_EQ(OrderIdempotency::Decision::Replay, cache.begin("k1", "fp"));
    EXPECT_EQ(OrderIdempotency::Decision::Replay, cache.begin("k2", "fp"));
}

TEST(OrderIdempotency, InFlightEntrySurvivesTtlExpiry)
{
    // An attempt that outlives the TTL (a POST blocked on the shared REST
    // mutex, a suspended process) must keep its guard: erasing it would let
    // the owner's retry place a second exchange order.
    OrderIdempotency cache{ std::chrono::seconds{ 0 } };

    EXPECT_EQ(OrderIdempotency::Decision::New, cache.begin("gt-9", "fp"));

    // Expiry is already in the past, but the entry is still executing.
    EXPECT_EQ(OrderIdempotency::Decision::InFlight,
              cache.begin("gt-9", "fp"));
    EXPECT_EQ(1U, cache.size());

    // complete() refreshes the expiry; with a 0 s TTL the COMPLETED entry is
    // now reclaimable, which is the pre-existing TTL contract.
    cache.complete("gt-9", successResponse("order-9"));
    EXPECT_EQ(OrderIdempotency::Decision::New, cache.begin("gt-9", "fp"));
}

TEST(OrderIdempotency, InFlightEntryIsNotEvictedWhenFull)
{
    // A burst of unique keys must never evict a guard: the in-flight entry is
    // the one thing that prevents a double-send. The new keys are refused
    // instead, even though the cache holds only that guard.
    OrderIdempotency cache{ std::chrono::seconds{ 600 }, 1 };

    EXPECT_EQ(OrderIdempotency::Decision::New, cache.begin("inflight", "fp"));
    EXPECT_EQ(OrderIdempotency::Decision::Capacity,
              cache.begin("newcomer", "fp"));

    // The guard is intact: the owner retry is still refused as duplicate.
    EXPECT_EQ(OrderIdempotency::Decision::InFlight,
              cache.begin("inflight", "fp"));
    EXPECT_EQ(1U, cache.size());
}

TEST(OrderIdempotency, ConcurrentBeginYieldsExactlyOneNew)
{
    // The whole point: two threads racing with the same key must not both
    // think they own the order. Exactly one gets New, the rest see InFlight.
    OrderIdempotency cache;
    constexpr int kThreads = 8;

    std::barrier gate{ kThreads };
    std::atomic<int> new_count{ 0 };
    std::atomic<int> in_flight_count{ 0 };

    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int i = 0; i < kThreads; ++i)
    {
        threads.emplace_back(
            [&cache, &gate, &new_count, &in_flight_count]()
            {
                gate.arrive_and_wait();
                if (OrderIdempotency::Decision::New
                    == cache.begin("gt-8", "fp"))
                {
                    new_count.fetch_add(1);
                }
                else
                {
                    in_flight_count.fetch_add(1);
                }
            });
    }
    for (auto &thread : threads)
    {
        thread.join();
    }

    EXPECT_EQ(1, new_count.load());
    EXPECT_EQ(kThreads - 1, in_flight_count.load());
}
