// param_change_log.cpp — ring-buffer audit log for parameter changes

#include "core/ParamChangeLog.hpp"

#include <algorithm>

namespace pulse::core
{

ParamChangeLog::ParamChangeLog(std::size_t capacity)
    : m_capacity{ capacity > 0 ? capacity : 1 }
{
    m_entries.reserve(m_capacity);
}

void ParamChangeLog::record(ParamChangeEntry entry)
{
    std::lock_guard lock{ m_mutex };
    m_entries.push_back(std::move(entry));
    if (m_entries.size() > m_capacity)
    {
        // Evict the oldest entries to stay at capacity.
        m_entries.erase(m_entries.begin(),
                        m_entries.begin() + (m_entries.size() - m_capacity));
    }
}

std::vector<ParamChangeEntry> ParamChangeLog::snapshot() const
{
    std::lock_guard lock{ m_mutex };
    auto result = m_entries; // Copy under the lock.
    std::reverse(result.begin(), result.end()); // Newest first.
    return result;
}

std::size_t ParamChangeLog::size() const
{
    std::lock_guard lock{ m_mutex };
    return m_entries.size();
}

std::size_t ParamChangeLog::capacity() const
{
    return m_capacity;
}

} // namespace pulse::core
