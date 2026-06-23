// newsFeed.cpp — News article ingestion for AI analysis (Layer 4 AI Analysis)
//
// Architecture:
//   1. poll() dispatches to the configured provider's URL builder
//   2. A single HTTP GET is issued with the provider's API key
//   3. The JSON response is parsed (non-throwing) via the provider-specific parser
//   4. Articles are deduplicated by URL and added to the rolling deque
//   5. On any error, poll() logs a warning and returns 0 (never throws)
//
// Supported providers:
//   - "newsapi"     — NewsAPI v2 everything endpoint (newsapi.org)
//   - "cryptopanic" — CryptoPanic posts API v1 (cryptopanic.com)
//
// HTTP pattern:
//   - Each request creates its own curl easy handle (per-request, stateless)
//   - curl_global_init is called exactly once via std::call_once (process-safe)
//   - No retry logic — polling will naturally retry on the next interval

#include "ai/news_feed.hpp"

#include "logging/logger.hpp"

#include <curl/curl.h>

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

// Per-request HTTP timeout (5 seconds — news feeds are not latency-critical).
constexpr long kHttpTimeoutMs = 5'000;

// URL-encode a string (percent-encoding for query parameters).
// Only encodes characters that are unsafe in URL query strings.
std::string urlEncode(const std::string &s)
{
    std::string result;
    result.reserve(s.size() * 2);

    for (const char c : s)
    {
        if (std::isalnum(static_cast<unsigned char>(c))
            || '-' == c || '_' == c || '.' == c || '~' == c)
        {
            result += c;
        }
        else if (' ' == c)
        {
            result += "%20";
        }
        else
        {
            // Hex-encode the character
            static constexpr char kHexChars[] = "0123456789ABCDEF";
            result += '%';
            result += kHexChars[(static_cast<unsigned char>(c) >> 4) & 0x0F];
            result += kHexChars[static_cast<unsigned char>(c) & 0x0F];
        }
    }

    return result;
}

// Parse an ISO 8601 timestamp string (e.g. "2023-01-15T10:30:00Z") to Unix ms.
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

// Get current time as Unix milliseconds (fallback for missing timestamps).
std::int64_t nowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// NewsFeed implementation
// ---------------------------------------------------------------------------

NewsFeed::NewsFeed(const NewsConfig &config) : m_config(config)
{
    ensureCurlInit();
}

// ---------------------------------------------------------------------------
// poll — fetch recent articles and add to rolling window
//
// Steps:
//   1. Check if feed is enabled
//   2. Select provider-specific URL builder
//   3. HTTP GET with provider API key
//   4. Parse JSON response (non-throwing)
//   5. Dispatch to provider-specific parser
//   6. Deduplicate articles by URL
//   7. Append new articles to rolling deque
//   8. Trim deque to maxArticles (evict oldest from front)
//   9. Return count of new articles (0 on any error)
// ---------------------------------------------------------------------------
int NewsFeed::poll()
{
    // 1. Check if feed is enabled
    if (!m_config.enabled)
    {
        return 0;
    }

    // 2. Build the request URL based on provider
    std::string url;
    if ("newsapi" == m_config.provider)
    {
        url = buildNewsapiUrl();
    }
    else if ("cryptopanic" == m_config.provider)
    {
        url = buildCryptopanicUrl();
    }
    else
    {
        PULSE_LOG_WARN("news", "unknown provider: {} — skipping poll", m_config.provider);
        return 0;
    }

    if (url.empty())
    {
        PULSE_LOG_WARN("news", "failed to build URL — skipping poll");
        return 0;
    }

    PULSE_LOG_DEBUG("news", "polling {} via {}", m_config.provider, url);

    // Check API key
    if (m_config.apiKey.empty())
    {
        PULSE_LOG_WARN("news", "no API key configured — skipping poll");
        return 0;
    }

    // 3. HTTP GET
    const HttpResponse response = doRequest(url);

    // 4. Check HTTP status
    if (0 == response.status_code)
    {
        PULSE_LOG_WARN("news", "request failed: {}", response.body);
        return 0;
    }

    if (response.status_code < 200 || response.status_code >= 300)
    {
        PULSE_LOG_WARN("news", "HTTP {}: {}", response.status_code, response.body.substr(0, 200));
        return 0;
    }

    // 5. Parse JSON (non-throwing)
    auto json_data = nlohmann::json::parse(response.body, nullptr, false);
    if (json_data.is_discarded())
    {
        PULSE_LOG_WARN("news", "JSON parse failed: {}", response.body.substr(0, 200));
        return 0;
    }

    // 6. Dispatch to provider-specific parser
    std::vector<NewsArticle> new_articles;
    if ("newsapi" == m_config.provider)
    {
        new_articles = parseNewsapi(json_data);
    }
    else if ("cryptopanic" == m_config.provider)
    {
        new_articles = parseCryptopanic(json_data);
    }

    // 7. Deduplicate and add to rolling window
    int new_count = 0;

    std::lock_guard<std::mutex> lock(m_mutex);

    for (auto &article : new_articles)
    {
        // Dedup by URL
        if (m_seenUrls.count(article.url) > 0)
        {
            continue;
        }

        m_seenUrls.insert(article.url);
        m_articles.push_back(std::move(article));
        ++new_count;
    }

    // 8. Trim to maxArticles (evict oldest from front)
    while (m_articles.size() > m_config.maxArticles)
    {
        m_seenUrls.erase(m_articles.front().url);
        m_articles.pop_front();
    }

    PULSE_LOG_INFO("news", "polled {} new article(s), window size: {}", new_count, m_articles.size());
    return new_count;
}

// ---------------------------------------------------------------------------
// recentText — concatenate headlines + snippets for AI prompt inclusion
//
// Format: each article on its own block:
//   [N] Title (Source)
//       Snippet
// Articles are ordered oldest-first (front of deque).
// ---------------------------------------------------------------------------
std::string NewsFeed::recentText(std::size_t max_count) const
{
    std::lock_guard<std::mutex> lock(m_mutex);

    if (m_articles.empty())
    {
        return "";
    }

    // Take the last max_count articles (most recent)
    const std::size_t start = (m_articles.size() > max_count) ? (m_articles.size() - max_count) : 0;
    const std::size_t count = m_articles.size() - start;

    std::string result;
    result.reserve(count * 300); // Rough estimate

    for (std::size_t i = 0; i < count; ++i)
    {
        const auto &article = m_articles[start + i];
        if (!result.empty())
        {
            result += "\n";
        }
        // Format: [N] Title (Source)\n    Snippet
        result += "[" + std::to_string(i + 1) + "] " + article.title;
        if (!article.source.empty())
        {
            result += " (" + article.source + ")";
        }
        if (!article.snippet.empty())
        {
            result += "\n    " + article.snippet;
        }
    }

    return result;
}

std::size_t NewsFeed::size() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_articles.size();
}

void NewsFeed::clear()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_articles.clear();
    m_seenUrls.clear();
}

// ---------------------------------------------------------------------------
// doRequest — single HTTP GET request via libcurl
//
// Steps:
//   1. Create a curl easy handle
//   2. Set URL (API key is embedded in the URL as query param)
//   3. Configure write callback and timeout
//   4. Execute and collect response
//   5. Clean up handle and return
// ---------------------------------------------------------------------------
NewsFeed::HttpResponse NewsFeed::doRequest(const std::string &url) const
{
    HttpResponse response;

    CURL *curl = curl_easy_init();
    if (nullptr == curl)
    {
        response.body = "curl_easy_init failed";
        return response;
    }

    // 1. Configure the request (API key is in the URL query string)
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curlWriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response.body);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, kHttpTimeoutMs);

    // 2. Execute
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

    // 3. Clean up
    curl_easy_cleanup(curl);

    return response;
}

// ---------------------------------------------------------------------------
// buildNewsapiUrl — construct NewsAPI v2 everything endpoint URL
//
// Format: https://newsapi.org/v2/everything?q={keywords}&apiKey={key}
//         &sortBy=publishedAt&pageSize=10
//
// Keywords are joined with OR for broader matching.
// Returns empty string if API key is missing.
// ---------------------------------------------------------------------------
std::string NewsFeed::buildNewsapiUrl() const
{
    if (m_config.apiKey.empty())
    {
        return "";
    }

    // Determine base URL (use config or default)
    std::string base = m_config.baseUrl;
    if (base.empty())
    {
        base = "https://newsapi.org/v2";
    }

    // Build query string from keywords (joined with OR)
    std::string query;
    for (std::size_t i = 0; i < m_config.keywords.size(); ++i)
    {
        if (i > 0)
        {
            query += " OR ";
        }
        query += m_config.keywords[i];
    }

    // If no keywords, use a generic crypto query
    if (query.empty())
    {
        query = "cryptocurrency OR bitcoin OR crypto";
    }

    return base + "/everything?q=" + urlEncode(query)
           + "&apiKey=" + urlEncode(m_config.apiKey)
           + "&sortBy=publishedAt&pageSize=10";
}

// ---------------------------------------------------------------------------
// buildCryptopanicUrl — construct CryptoPanic posts API v1 URL
//
// Format: https://cryptopanic.com/api/v1/posts/?auth_token={key}
//         &currencies=BTC&kind=news
//
// Returns empty string if API key is missing.
// ---------------------------------------------------------------------------
std::string NewsFeed::buildCryptopanicUrl() const
{
    if (m_config.apiKey.empty())
    {
        return "";
    }

    // Determine base URL (use config or default)
    std::string base = m_config.baseUrl;
    if (base.empty())
    {
        base = "https://cryptopanic.com/api/v1";
    }

    return base + "/posts/?auth_token=" + urlEncode(m_config.apiKey)
           + "&currencies=BTC&kind=news";
}

// ---------------------------------------------------------------------------
// parseNewsapi — parse NewsAPI v2 response into NewsArticle vector
//
// Response structure:
//   {
//     "status": "ok",
//     "totalResults": 123,
//     "articles": [
//       {
//         "source": { "id": "...", "name": "CoinDesk" },
//         "title": "Article headline",
//         "description": "Brief summary...",
//         "url": "https://...",
//         "publishedAt": "2023-01-15T10:30:00Z"
//       }
//     ]
//   }
// ---------------------------------------------------------------------------
std::vector<NewsArticle> NewsFeed::parseNewsapi(const nlohmann::json &j) const
{
    std::vector<NewsArticle> result;

    if (!j.contains("articles") || !j["articles"].is_array())
    {
        PULSE_LOG_DEBUG("news", "NewsAPI response has no articles array");
        return result;
    }

    const auto &articles = j["articles"];

    for (const auto &item : articles)
    {
        if (!item.is_object())
        {
            continue;
        }

        // Extract required fields
        if (!item.contains("url") || !item["url"].is_string())
        {
            continue;
        }
        if (!item.contains("title") || !item["title"].is_string())
        {
            continue;
        }

        NewsArticle article;
        article.url = item["url"].get<std::string>();
        article.title = item["title"].get<std::string>();

        // Extract optional source name
        if (item.contains("source") && item["source"].is_object()
            && item["source"].contains("name") && item["source"]["name"].is_string())
        {
            article.source = item["source"]["name"].get<std::string>();
        }

        // Extract optional description (snippet)
        if (item.contains("description") && item["description"].is_string())
        {
            article.snippet = item["description"].get<std::string>();
        }

        // Parse publishedAt timestamp (ISO 8601 → Unix ms)
        if (item.contains("publishedAt") && item["publishedAt"].is_string())
        {
            article.timestamp = parseIso8601(item["publishedAt"].get<std::string>());
            if (0 == article.timestamp)
            {
                article.timestamp = nowMs();
            }
        }
        else
        {
            article.timestamp = nowMs();
        }

        result.push_back(std::move(article));
    }

    PULSE_LOG_DEBUG("news", "parsed {} article(s) from NewsAPI", result.size());
    return result;
}

// ---------------------------------------------------------------------------
// parseCryptopanic — parse CryptoPanic v1 response into NewsArticle vector
//
// Response structure:
//   {
//     "results": [
//       {
//         "url": "https://...",
//         "title": "Post headline",
//         "source": "CoinDesk",
//         "published_at": "2023-01-15T10:30:00Z"  (optional)
//       }
//     ]
//   }
// ---------------------------------------------------------------------------
std::vector<NewsArticle> NewsFeed::parseCryptopanic(const nlohmann::json &j) const
{
    std::vector<NewsArticle> result;

    // CryptoPanic uses "results" array (not "data" as documented)
    if (!j.contains("results") || !j["results"].is_array())
    {
        PULSE_LOG_DEBUG("news", "CryptoPanic response has no results array");
        return result;
    }

    const auto &posts = j["results"];

    for (const auto &item : posts)
    {
        if (!item.is_object())
        {
            continue;
        }

        // Extract required fields
        if (!item.contains("url") || !item["url"].is_string())
        {
            continue;
        }
        if (!item.contains("title") || !item["title"].is_string())
        {
            continue;
        }

        NewsArticle article;
        article.url = item["url"].get<std::string>();
        article.title = item["title"].get<std::string>();

        // Extract optional source
        if (item.contains("source") && item["source"].is_string())
        {
            article.source = item["source"].get<std::string>();
        }

        // Extract optional snippet (CryptoPanic may not have this)
        if (item.contains("snippet") && item["snippet"].is_string())
        {
            article.snippet = item["snippet"].get<std::string>();
        }

        // Parse timestamp if available (CryptoPanic uses "published_at")
        if (item.contains("published_at") && item["published_at"].is_string())
        {
            article.timestamp = parseIso8601(item["published_at"].get<std::string>());
            if (0 == article.timestamp)
            {
                article.timestamp = nowMs();
            }
        }
        else
        {
            // CryptoPanic may not always provide timestamps
            article.timestamp = nowMs();
        }

        result.push_back(std::move(article));
    }

    PULSE_LOG_DEBUG("news", "parsed {} article(s) from CryptoPanic", result.size());
    return result;
}

} // namespace pulse::ai
