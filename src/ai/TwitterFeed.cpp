// twitterFeed.cpp — X (Twitter) API v2 social signal ingestion (Layer 4 AI Analysis)
//
// Architecture:
//   1. poll() builds the recent-search URL from configured keywords
//   2. A single HTTP GET is issued with Bearer token authentication
//   3. The JSON response is parsed (non-throwing) and tweets are deduplicated
//   4. New tweets are appended to the rolling deque, oldest evicted when over limit
//   5. On any error, poll() logs a warning and returns 0 (never throws)
//
// HTTP pattern:
//   - Each request creates its own curl easy handle (per-request, stateless)
//   - curl_global_init is called exactly once via std::call_once (process-safe)
//   - No retry logic — polling will naturally retry on the next interval

#include "ai/TwitterFeed.hpp"

#include "logging/Logger.hpp"

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <ctime>
#include <mutex>

namespace pulse::ai
{

// ---------------------------------------------------------------------------
// Anonymous namespace — process-wide curl init and shared helpers
// ---------------------------------------------------------------------------

namespace
{

std::once_flag g_curl_init_flag;

void ensureCurlInit()
{
    std::call_once(g_curl_init_flag, []()
            {
                curl_global_init(CURL_GLOBAL_DEFAULT);
            });
}

// libcurl write callback: appends received data chunks to a string buffer.
size_t curlWriteCallback(void *contents, size_t size, size_t nmemb, std::string *output)
{
    const size_t total = size * nmemb;
    output->append(static_cast<char *>(contents), total);
    return total;
}

// Per-request HTTP timeout (5 seconds — news/social feeds are not latency-critical).
constexpr long kHttpTimeoutMs = 5'000;

// Parse an ISO 8601 timestamp string (e.g. "2023-01-15T10:30:00.000Z") to Unix ms.
// Steps:
//   1. Extract date/time components via sscanf
//   2. Build struct tm and convert to time_t via mktime (interprets as UTC)
//   3. Apply timezone offset if present
//   4. Convert to milliseconds and add fractional seconds
// Returns 0 on parse failure.
std::int64_t parseIso8601(const std::string &s)
{
    int year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;
    int tz_hour = 0, tz_min = 0;
    double frac_sec = 0.0;

    // 1. Parse the main date-time portion
    const int n = std::sscanf(s.c_str(), "%d-%d-%dT%d:%d:%d",
                              &year, &month, &day, &hour, &minute, &second);
    if (6 != n)
    {
        return 0;
    }

    // 2. Parse optional fractional seconds (e.g. ".000")
    const auto dot_pos = s.find('.');
    if (std::string::npos != dot_pos)
    {
        std::sscanf(s.c_str() + dot_pos, "%lf", &frac_sec);
    }

    // 3. Parse optional timezone offset (+HH:MM, -HH:MM, or Z for UTC)
    const auto plus_pos = s.find('+');
    const auto minus_pos = s.rfind('-');
    // The timezone '-' must appear after the 'T' separator (position > 10)
    const auto t_pos = s.find('T');

    if (std::string::npos != plus_pos)
    {
        std::sscanf(s.c_str() + plus_pos, "+%d:%d", &tz_hour, &tz_min);
    }
    else if (std::string::npos != minus_pos && std::string::npos != t_pos && minus_pos > t_pos)
    {
        std::sscanf(s.c_str() + minus_pos, "-%d:%d", &tz_hour, &tz_min);
        tz_hour = -tz_hour;
        tz_min = -tz_min;
    }

    // 4. Build struct tm (months 0-based, years since 1900)
    std::tm tm_val = {};
    tm_val.tm_year = year - 1900;
    tm_val.tm_mon = month - 1;
    tm_val.tm_mday = day;
    tm_val.tm_hour = hour;
    tm_val.tm_min = minute;
    tm_val.tm_sec = second;

    // mktime interprets as UTC — subtract the timezone offset to get true UTC
    const std::time_t t = std::mktime(&tm_val);
    if (static_cast<std::time_t>(-1) == t)
    {
        return 0;
    }

    const auto total_seconds = static_cast<std::int64_t>(t)
                               - static_cast<std::int64_t>(tz_hour) * 3600
                               - static_cast<std::int64_t>(tz_min) * 60;
    return total_seconds * 1000 + static_cast<std::int64_t>(frac_sec * 1000.0);
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// TwitterFeed implementation
// ---------------------------------------------------------------------------

TwitterFeed::TwitterFeed(const TwitterConfig &config) : m_config(config)
{
    ensureCurlInit();
}

// ---------------------------------------------------------------------------
// poll — fetch recent tweets and add to rolling window
//
// Steps:
//   1. Check if feed is enabled
//   2. Build the search URL from configured keywords
//   3. HTTP GET with Bearer token authentication
//   4. Parse JSON response (non-throwing)
//   5. Deduplicate tweets by ID
//   6. Append new tweets to rolling deque
//   7. Trim deque to maxTweets (evict oldest from front)
//   8. Return count of new tweets (0 on any error)
// ---------------------------------------------------------------------------
int TwitterFeed::poll()
{
    // 1. Check if feed is enabled
    if (!m_config.enabled)
    {
        return 0;
    }

    // 2. Build search URL
    const std::string url = buildSearchUrl();
    if (url.empty())
    {
        PULSE_LOG_WARN("twitter", "no keywords configured — skipping poll");
        return 0;
    }

    PULSE_LOG_DEBUG("twitter", "polling: {}", url);

    // 3. HTTP GET with Bearer token
    if (m_config.bearerToken.empty())
    {
        PULSE_LOG_WARN("twitter", "no bearer token configured — skipping poll");
        return 0;
    }

    const HttpResponse response = doRequest(url);

    // 4. Check HTTP status
    if (0 == response.status_code)
    {
        PULSE_LOG_WARN("twitter", "request failed: {}", response.body);
        return 0;
    }

    if (response.status_code < 200 || response.status_code >= 300)
    {
        PULSE_LOG_WARN("twitter", "HTTP {}: {}", response.status_code, response.body.substr(0, 200));
        return 0;
    }

    // 5. Parse JSON (non-throwing)
    auto json_data = nlohmann::json::parse(response.body, nullptr, false);
    if (json_data.is_discarded())
    {
        PULSE_LOG_WARN("twitter", "JSON parse failed: {}", response.body.substr(0, 200));
        return 0;
    }

    // 6. Extract tweet data array
    if (!json_data.contains("data") || !json_data["data"].is_array())
    {
        // No results — this is normal when no tweets match the query
        PULSE_LOG_DEBUG("twitter", "no tweets in response");
        return 0;
    }

    const auto &data_array = json_data["data"];
    int new_count = 0;

    std::lock_guard<std::mutex> lock(m_mutex);

    // 7. Process each tweet
    for (const auto &item : data_array)
    {
        if (!item.is_object())
        {
            continue;
        }

        // Extract required fields
        if (!item.contains("id") || !item.contains("text"))
        {
            continue;
        }

        const std::string id = item["id"].get<std::string>();
        const std::string text = item["text"].get<std::string>();

        // Dedup by tweet ID
        if (m_seenIds.count(id) > 0)
        {
            continue;
        }

        // Extract optional author_id (API v2 returns author_id, not username)
        std::string author;
        if (item.contains("author_id") && item["author_id"].is_string())
        {
            author = item["author_id"].get<std::string>();
        }

        // Parse created_at timestamp (ISO 8601 → Unix ms)
        std::int64_t timestamp = 0;
        if (item.contains("created_at") && item["created_at"].is_string())
        {
            const std::string created_at = item["created_at"].get<std::string>();
            timestamp = parseIso8601(created_at);
            if (0 == timestamp)
            {
                // Fallback to current time if parse fails
                timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::system_clock::now().time_since_epoch()).count();
            }
        }

        // Add to rolling window
        m_seenIds.insert(id);
        m_tweets.push_back(Tweet{id, text, author, timestamp});
        ++new_count;
    }

    // 8. Trim to maxTweets (evict oldest from front)
    while (m_tweets.size() > m_config.maxTweets)
    {
        m_seenIds.erase(m_tweets.front().id);
        m_tweets.pop_front();
    }

    PULSE_LOG_INFO("twitter", "polled {} new tweet(s), window size: {}", new_count, m_tweets.size());
    return new_count;
}

// ---------------------------------------------------------------------------
// recentText — concatenate tweet text for AI prompt inclusion
//
// Format: each tweet on its own line, prefixed with [N] @author:
// Tweets are ordered oldest-first (front of deque).
// ---------------------------------------------------------------------------
std::string TwitterFeed::recentText(std::size_t max_count) const
{
    std::lock_guard<std::mutex> lock(m_mutex);

    if (m_tweets.empty())
    {
        return "";
    }

    // Take the last max_count tweets (most recent)
    const std::size_t start = (m_tweets.size() > max_count) ? (m_tweets.size() - max_count) : 0;
    const std::size_t count = m_tweets.size() - start;

    std::string result;
    result.reserve(count * 200); // Rough estimate

    for (std::size_t i = 0; i < count; ++i)
    {
        const auto &tweet = m_tweets[start + i];
        if (!result.empty())
        {
            result += "\n";
        }
        // Format: [N] @author: text (or [N] text if no author)
        result += "[" + std::to_string(i + 1) + "] ";
        if (!tweet.author.empty())
        {
            result += "@" + tweet.author + ": ";
        }
        result += tweet.text;
    }

    return result;
}

std::size_t TwitterFeed::size() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_tweets.size();
}

void TwitterFeed::clear()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_tweets.clear();
    m_seenIds.clear();
}

// ---------------------------------------------------------------------------
// doRequest — single HTTP GET request via libcurl
//
// Steps:
//   1. Create a curl easy handle
//   2. Set URL and Authorization: Bearer header
//   3. Configure write callback and timeout
//   4. Execute and collect response
//   5. Clean up handle and return
// ---------------------------------------------------------------------------
TwitterFeed::HttpResponse TwitterFeed::doRequest(const std::string &url) const
{
    HttpResponse response;

    CURL *curl = curl_easy_init();
    if (nullptr == curl)
    {
        response.body = "curl_easy_init failed";
        return response;
    }

    // 1. Build HTTP headers with Bearer token authentication
    struct curl_slist *headers = nullptr;
    headers = curl_slist_append(headers, ("Authorization: Bearer " + m_config.bearerToken).c_str());

    // 2. Configure the request
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curlWriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response.body);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, kHttpTimeoutMs);

    // 3. Execute
    CURLcode res = curl_easy_perform(curl);
    if (CURLE_OK == res)
    {
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response.status_code);
    }
    else
    {
        // Transport-level failure (DNS, TCP, TLS, timeout, etc.)
        response.status_code = 0;
        response.body = "curl error " + std::to_string(static_cast<int>(res)) + ": " + curl_easy_strerror(res);
    }

    // 4. Clean up
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    return response;
}

// ---------------------------------------------------------------------------
// buildSearchUrl — construct the X API v2 recent-search URL
//
// Format: {baseUrl}/tweets/search/recent?query={keywords}&max_results=10
//         &tweet.fields=author_id,created_at
//
// Keywords are joined with OR for broader matching.
// Returns empty string if no keywords are configured.
// ---------------------------------------------------------------------------
std::string TwitterFeed::buildSearchUrl() const
{
    if (m_config.keywords.empty())
    {
        return "";
    }

    // Join keywords with OR (X API query syntax for broader matching)
    std::string query;
    for (std::size_t i = 0; i < m_config.keywords.size(); ++i)
    {
        if (i > 0)
        {
            query += " OR ";
        }
        query += m_config.keywords[i];
    }

    // URL-encode spaces in the query (%20)
    std::string encoded_query;
    encoded_query.reserve(query.size());
    for (const char c : query)
    {
        if (' ' == c)
        {
            encoded_query += "%20";
        }
        else
        {
            encoded_query += c;
        }
    }

    return m_config.baseUrl + "/tweets/search/recent?query=" + encoded_query
           + "&max_results=10&tweet.fields=author_id,created_at";
}

} // namespace pulse::ai
