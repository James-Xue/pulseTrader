// kline_buffer.cpp — KlineBuffer implementation (Layer 3 Market Data)

#include "market/KlineBuffer.hpp"

#include <algorithm>

namespace pulse::market
{

KlineBuffer::KlineBuffer(std::size_t capacity)
    : m_capacity{ capacity }
    , m_buffer(capacity)
{
}

void KlineBuffer::push(const Kline &kline)
{
    // Increment seqlock to odd (signal write in progress).
    const std::uint64_t seq = m_seqlock.fetch_add(1, std::memory_order_acq_rel);
    (void)seq; // Suppress unused variable warning.

    // Write the kline to the ring buffer.
    const std::size_t write_pos = m_head.load(std::memory_order_relaxed);
    m_buffer[write_pos] = kline;

    // Advance head and count.
    const std::size_t new_head = (write_pos + 1) % m_capacity;
    m_head.store(new_head, std::memory_order_release);

    const std::size_t current_count = m_count.load(std::memory_order_relaxed);
    if (current_count < m_capacity)
    {
        m_count.store(current_count + 1, std::memory_order_release);
    }

    // Increment seqlock to even (signal write complete).
    m_seqlock.fetch_add(1, std::memory_order_release);
}

std::vector<Kline> KlineBuffer::snapshot(std::size_t n) const
{
    // Retry loop: seqlock ensures consistent snapshot.
    while (true)
    {
        // Step 1: Read seqlock (must be even = no write in progress).
        const std::uint64_t seq1 = m_seqlock.load(std::memory_order_acquire);
        if (1u == (seq1 & 1u))
        {
            // Odd seqlock — write in progress, retry.
            continue;
        }

        // Step 2: Read count and head (these may change, but we will verify later).
        const std::size_t current_count = m_count.load(std::memory_order_acquire);
        if (0 == current_count)
        {
            return {}; // Buffer is empty.
        }

        // Determine how many candles to return.
        const std::size_t num_to_return = (0 == n || n > current_count) ? current_count : n;

        // Step 3: Copy the requested candles.
        // Logical positions: [current_count - num_to_return, current_count - 1]
        // (oldest to newest within the requested range).
        std::vector<Kline> result;
        result.reserve(num_to_return);

        const std::size_t start_logical = current_count - num_to_return;
        for (std::size_t i = 0; i < num_to_return; ++i)
        {
            const std::size_t logical_pos = start_logical + i;
            const std::size_t physical_pos = logicalToPhysical(logical_pos);
            result.push_back(m_buffer[physical_pos]);
        }

        // Step 4: Verify seqlock (must match seq1 and be even).
        const std::uint64_t seq2 = m_seqlock.load(std::memory_order_acquire);
        if (seq1 == seq2)
        {
            return result; // Consistent snapshot.
        }
        // Otherwise, a write occurred during the copy — retry.
    }
}

std::optional<Kline> KlineBuffer::latest() const
{
    // Retry loop: seqlock ensures consistent read.
    while (true)
    {
        const std::uint64_t seq1 = m_seqlock.load(std::memory_order_acquire);
        if (1u == (seq1 & 1u))
        {
            continue; // Write in progress.
        }

        const std::size_t current_count = m_count.load(std::memory_order_acquire);
        if (0 == current_count)
        {
            return std::nullopt;
        }

        // Latest candle is at logical position (current_count - 1).
        const std::size_t physical_pos = logicalToPhysical(current_count - 1);
        const Kline latest_kline = m_buffer[physical_pos];

        const std::uint64_t seq2 = m_seqlock.load(std::memory_order_acquire);
        if (seq1 == seq2)
        {
            return latest_kline;
        }
    }
}

std::size_t KlineBuffer::size() const
{
    return m_count.load(std::memory_order_acquire);
}

std::size_t KlineBuffer::capacity() const
{
    return m_capacity;
}

std::size_t KlineBuffer::logicalToPhysical(std::size_t logical_pos) const
{
    // Logical position 0 = oldest candle.
    // Physical position = (head - count + logical_pos) mod capacity.
    const std::size_t current_count = m_count.load(std::memory_order_relaxed);
    const std::size_t head = m_head.load(std::memory_order_relaxed);

    // Compute the physical index of the oldest candle.
    const std::size_t oldest_physical = (head + m_capacity - current_count) % m_capacity;

    // Offset by logical position.
    return (oldest_physical + logical_pos) % m_capacity;
}

} // namespace pulse::market
