// OrderIdempotency.cpp — see OrderIdempotency.hpp

#include "control/OrderIdempotency.hpp"

#include <utility>

namespace pulse::control
{

OrderIdempotency::OrderIdempotency(std::chrono::seconds ttl,
                                   std::size_t max_entries)
    : m_ttl{ ttl }
    , m_maxEntries{ max_entries > 0 ? max_entries : 1 }
{
}

OrderIdempotency::Decision OrderIdempotency::begin(const std::string &key,
                                                   const std::string &fingerprint)
{
    const auto now = std::chrono::steady_clock::now();
    std::lock_guard lock(m_mutex);

    // 1. Expired entries are invisible: a retry after the TTL is a new order.
    purgeExpiredLocked(now);

    // 2. Decide from the surviving entry (if any). The in-flight test comes
    //    first: the order is genuinely still executing, and reporting
    //    InFlight matches what the caller is looking at. Both InFlight and
    //    Conflict refuse to place, so the ordering only shapes the message.
    const auto it = m_entries.find(key);
    if (it != m_entries.end())
    {
        if (it->second.inFlight)
        {
            return Decision::InFlight;
        }
        // An empty stored fingerprint means "unknown" — complete() lost it
        // when a defensive fresh entry had to be created for a vanished
        // in-flight key. Replaying is the safe answer: the order already
        // reached the exchange, so refusing to answer (or re-placing it) is
        // worse than returning its outcome.
        if (!it->second.fingerprint.empty()
            && it->second.fingerprint != fingerprint)
        {
            return Decision::Conflict;
        }
        return Decision::Replay;
    }

    // 3. First sighting: fail closed when the cache is full. Evicting a live
    //    entry would be worse than refusing this request: an evicted in-flight
    //    guard lets a retry place a second order, and an evicted completed
    //    outcome lets a late retry do the same. The caller can retry once the
    //    TTL has reclaimed older COMPLETED entries (in-flight ones are never
    //    reclaimed, so a pathological backlog stays rejected rather than
    //    silently losing its guards).
    if (m_entries.size() >= m_maxEntries)
    {
        return Decision::Capacity;
    }

    Entry entry;
    entry.fingerprint = fingerprint;
    entry.inFlight = true;
    entry.expiry = now + m_ttl;
    m_entries.emplace(key, std::move(entry));
    return Decision::New;
}

void OrderIdempotency::complete(const std::string &key,
                                Result<execution::OrderResponse> outcome)
{
    const auto now = std::chrono::steady_clock::now();
    std::lock_guard lock(m_mutex);

    // 1. Common path: refresh the entry the in-flight attempt created.
    //    No purge happens here on purpose — the entry being completed must
    //    survive this call even with a 0 s TTL, so the outcome is observable
    //    at least once. The next begin() applies the expiry.
    auto it = m_entries.find(key);
    if (it != m_entries.end())
    {
        it->second.inFlight = false;
        it->second.outcome = std::move(outcome);
        it->second.expiry = now + m_ttl;
        return;
    }

    // 2. The entry vanished while the order was in flight (defensive: only an
    //    explicit abort() of a mismatched guard can remove it — TTL expiry
    //    skips in-flight entries). Store the outcome anyway: a retry that
    //    finds no entry would place a second order for a key that already
    //    reached the exchange. The fingerprint is unknown here, so it stays
    //    empty (see begin()). The map is allowed to exceed its cap by this
    //    single entry; dropping a terminal outcome is never acceptable, and
    //    the next completed-entry TTL purge brings it back under the cap.
    purgeExpiredLocked(now);
    Entry entry;
    entry.inFlight = false;
    entry.outcome = std::move(outcome);
    entry.expiry = now + m_ttl;
    m_entries.emplace(key, std::move(entry));
}

void OrderIdempotency::abort(const std::string &key)
{
    std::lock_guard lock(m_mutex);
    m_entries.erase(key);
}

std::optional<Result<execution::OrderResponse>>
OrderIdempotency::cachedOutcome(const std::string &key) const
{
    const auto now = std::chrono::steady_clock::now();
    std::lock_guard lock(m_mutex);

    // Expired outcomes are not replayed. Purging here keeps a read-only call
    // from resurrecting a stale entry that begin() would have dropped anyway.
    purgeExpiredLocked(now);

    const auto it = m_entries.find(key);
    if (it == m_entries.end() || !it->second.outcome.has_value())
    {
        return std::nullopt;
    }
    return it->second.outcome;
}

std::size_t OrderIdempotency::size() const
{
    const auto now = std::chrono::steady_clock::now();
    std::lock_guard lock(m_mutex);
    purgeExpiredLocked(now);
    return m_entries.size();
}

void OrderIdempotency::purgeExpiredLocked(
    std::chrono::steady_clock::time_point now) const
{
    // Expiry is inclusive: an entry whose expiry equals `now` is already
    // stale. With a 0 s TTL this makes every COMPLETED entry expire on the
    // very next call, which is the contract the TTL test relies on.
    //
    // In-flight entries are never expired, however old their expiry is: the
    // order may still be executing (a POST blocked behind the shared REST
    // mutex, or a suspended process), and erasing the guard would let a retry
    // place a second exchange order for the same key. Only complete() or an
    // explicit abort() ends an in-flight attempt.
    for (auto it = m_entries.begin(); it != m_entries.end();)
    {
        if (!it->second.inFlight && it->second.expiry <= now)
        {
            it = m_entries.erase(it);
        }
        else
        {
            ++it;
        }
    }
}

} // namespace pulse::control
