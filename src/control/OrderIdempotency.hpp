#pragma once
// OrderIdempotency.hpp — duplicate suppression for control-plane open_order
//
// Why this exists: an external strategy runtime (GT) retries an `open_order`
// call whose reply was lost to a timeout. Without a de-duplication gate the
// retry creates a SECOND exchange order — the single worst failure mode of a
// networked execution path.
//
// The cache is deliberately ignorant of orders and of the exchange:
//   1. it stores a caller-computed fingerprint, so a key reused with different
//      parameters is rejected instead of silently replaying the wrong order,
//   2. it stores an in-flight flag, so a concurrent retry cannot double-send
//      while the first attempt is still inside the order flow, and
//   3. it stores the terminal Result<OrderResponse>, so a late retry receives
//      the exact outcome the first attempt produced. Success AND a definitive
//      rejection are both cacheable: a deterministic rejection must not be
//      re-executed either. The caller decides which failures those are —
//      pre-flight failures use abort() (the key stays retryable), and a
//      transport failure that may have reached the exchange is cached as an
//      explicit "outcome unknown" result, never as a plain rejection.
//
// The class performs no I/O and never calls the exchange. It is a pure state
// machine over (key, fingerprint) pairs, which is what makes it trivially
// testable and safe to call from the control-socket threads.
//
// Thread-safety: every public method may be called concurrently from any
// thread. One std::mutex guards the entry map. No method blocks on another
// lock and no method performs REST traffic, so a critical section is a few
// microseconds. begin() releases the mutex before it returns, so the (slow)
// exchange round-trip that follows never holds it.

#include "core/PulseError.hpp"
#include "execution/OrderExecutor.hpp"

#include <chrono>
#include <cstddef>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

namespace pulse::control
{

/// Per-key request de-duplication for `open_order`.
///
/// Ownership: one instance per EngineServices (engine lifetime).
/// Layer: L9 (control plane) — off the market-data hot path.
///
/// State machine per key:
///
///     absent --begin()--> inFlight --complete()--> completed
///                            |                        |
///                     begin() = InFlight        begin(same fp)   = Replay
///                            |                  begin(other fp)  = Conflict
///                        abort()  (only for a pre-flight failure)
///                            v
///                          absent
///
/// A completed entry becomes absent when its TTL elapses; an in-flight entry
/// is never expired by the TTL (only abort() releases it), because dropping
/// the guard while the order may still be executing is what allows a retry to
/// double-send.
class OrderIdempotency
{
  public:
    /// What the caller must do with a key returned by begin().
    enum class Decision
    {
        /// First sighting: an in-flight entry was inserted and the caller now
        /// owns it (it must call complete() or abort()).
        New,
        /// Another call with this key is still executing. The caller must
        /// reject the duplicate without touching the exchange.
        InFlight,
        /// The previous attempt finished. The caller must replay the cached
        /// outcome (see cachedOutcome()) and place nothing.
        Replay,
        /// The key is already bound to a different order. The caller must
        /// reject rather than treat the two orders as one.
        Conflict,
        /// The cache cannot take a new key without dropping a live one (a
        /// replayable outcome or an in-flight guard). The caller must reject
        /// the request as retryable and place nothing.
        Capacity,
    };

    /// Entries kept before new keys are refused. Bounds memory when a buggy
    /// or hostile client streams unique keys at the control socket. The cap is
    /// enforced by FAILING CLOSED (Decision::Capacity), never by eviction:
    /// dropping a cached outcome would shrink the replay guarantee and let a
    /// late retry place a second exchange order, which is the exact failure
    /// this class exists to prevent.
    static constexpr std::size_t kDefaultMaxEntries = 1024;

    /// How long a completed outcome stays replayable. A retry arriving later
    /// than this is treated as a new order — the client's own timeout is the
    /// real bound, and 10 minutes is far beyond any Gate REST timeout.
    static constexpr std::chrono::seconds kDefaultTtl{ 600 };

    /// `ttl` is a constructor parameter (not a constant) so tests can pass 0 s
    /// and observe expiry deterministically. `max_entries` is clamped to at
    /// least 1; a zero-sized cache could not hold its own in-flight entry.
    explicit OrderIdempotency(
        std::chrono::seconds ttl = kDefaultTtl,
        std::size_t max_entries = kDefaultMaxEntries);

    /// Register an attempt for `key`.
    ///
    /// Order of checks for an existing entry: in-flight beats fingerprint, so
    /// a duplicate that arrives while the first attempt is still in the order
    /// flow is reported as InFlight (the order really is executing) rather
    /// than Conflict. Both refuse to place anything, so the distinction only
    /// shapes the error message.
    ///
    /// An in-flight entry never expires and is never dropped to make room: it
    /// is the guard that stops a concurrent/late retry from double-sending,
    /// and an order that is still executing (a POST blocked behind the shared
    /// REST mutex, a suspended process) can outlive any TTL or a burst of
    /// newer keys. When the cache is full of live entries, begin() returns
    /// Capacity instead of evicting one.
    ///
    /// Returns New only when the caller must go on and place the order; every
    /// other Decision means "do not place".
    [[nodiscard]] Decision begin(const std::string &key,
                                 const std::string &fingerprint);

    /// Publish the terminal outcome for `key` and clear the in-flight flag,
    /// refreshing the expiry. A success Result and a PulseError are stored
    /// identically — both answer "what happened to this order" and both must
    /// stop a retry from reaching the exchange.
    ///
    /// If the entry vanished while the order was in flight (defensive: only a
    /// mismatched abort() can do that — TTL expiry skips in-flight entries and
    /// nothing is ever evicted), the outcome is stored under a fresh entry
    /// anyway: dropping it would let the next retry place a second order for a
    /// key that already reached the exchange.
    void complete(const std::string &key,
                  Result<execution::OrderResponse> outcome);

    /// Drop the entry for `key`.
    ///
    /// Only for a request that failed BEFORE any order could be sent (e.g. a
    /// pre-flight margin gate). Erasing makes the key usable again, so the
    /// caller may retry; it must never be used after an order reached the
    /// exchange.
    void abort(const std::string &key);

    /// Cached outcome for `key`, if the previous attempt finished and the
    /// entry has not expired. Empty while in flight and for unknown keys.
    [[nodiscard]] std::optional<Result<execution::OrderResponse>>
    cachedOutcome(const std::string &key) const;

    /// Number of live entries (after purging expired ones). For tests and
    /// diagnostics.
    [[nodiscard]] std::size_t size() const;

  private:
    struct Entry
    {
        std::string fingerprint;
        bool inFlight{ false };
        std::optional<Result<execution::OrderResponse>> outcome;
        std::chrono::steady_clock::time_point expiry{};
    };

    /// Erase every COMPLETED entry whose expiry is at or before `now`.
    ///
    /// In-flight entries are skipped: their expiry bounds a caller that never
    /// completed (reclaimed after one TTL), but while the attempt is still
    /// executing the guard must survive any TTL, or a retry arriving after the
    /// TTL would be treated as a first sighting and place a second order.
    ///
    /// Const because the read-side accessors (cachedOutcome / size) apply the
    /// same expiry rule; a const query that returned a half-expired view
    /// would make the cache look bigger and staler than it is.
    void purgeExpiredLocked(std::chrono::steady_clock::time_point now) const;

    std::chrono::seconds m_ttl;
    std::size_t m_maxEntries;
    mutable std::mutex m_mutex;
    mutable std::unordered_map<std::string, Entry> m_entries;
};

} // namespace pulse::control
