#pragma once
// newsFeed.hpp — News article ingestion for AI analysis (Layer 4 AI Analysis)
//
// Polls news provider APIs (NewsAPI or CryptoPanic) for articles matching
// configured keywords. Maintains a rolling window of recent articles for
// inclusion in AI analysis prompts.
//
// Architecture:
//   1. poll() dispatches to the configured provider's URL builder and parser
//   2. Articles are deduplicated by URL using a hash set
//   3. A rolling deque enforces the maxArticles window (oldest evicted first)
//   4. recentText() concatenates headlines + snippets for prompt injection
//
// Supported providers:
//   - "newsapi"     — NewsAPI v2 everything endpoint (newsapi.org)
//   - "cryptopanic" — CryptoPanic posts API v1 (cryptopanic.com)
//
// Thread safety:
//   - All public methods are thread-safe (mutex-protected)
//   - poll() may be called from any thread (typically the heartbeat scheduler)
//   - recentText() is const and safe to call concurrently with itself

#include "core/config.hpp"

#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>

#include <nlohmann/json.hpp>

namespace pulse::ai
{

// ---------------------------------------------------------------------------
// NewsArticle — single news record stored in the rolling window
//
// Fields:
//   1. url       — Article URL, used as the deduplication key
//   2. title     — Article headline
//   3. source    — News source name (e.g. "CoinDesk", "Reuters")
//   4. snippet   — Brief description or summary of the article
//   5. timestamp — Publication time as Unix milliseconds
// ---------------------------------------------------------------------------
struct NewsArticle
{
    std::string url;       ///< URL for dedup.
    std::string title;     ///< Article headline.
    std::string source;    ///< News source name.
    std::string snippet;   ///< Brief description/summary.
    std::int64_t timestamp; ///< Unix ms.
};

// ---------------------------------------------------------------------------
// NewsFeed — polls news provider APIs and maintains a rolling article window
//
// Usage:
//   1. Construct with NewsConfig from PulseConfig
//   2. Call poll() periodically (every pollIntervalSec seconds)
//   3. Call recentText() to get concatenated headlines for the AI prompt
// ---------------------------------------------------------------------------
class NewsFeed
{
public:
    explicit NewsFeed(const NewsConfig &config);

    // Poll news provider API. Returns number of new articles.
    // Returns 0 if disabled or on error.
    int poll();

    // Get concatenated headlines + snippets for prompt inclusion.
    [[nodiscard]] std::string recentText(std::size_t max_count = 5) const;

    [[nodiscard]] std::size_t size() const;
    void clear();

private:
    // HTTP GET request via libcurl (returns body + status_code).
    // Each call creates and destroys its own curl easy handle (stateless).
    struct HttpResponse
    {
        std::string body;
        long status_code = 0;
    };

    HttpResponse doRequest(const std::string &url) const;

    // Provider-specific URL builders.
    std::string buildNewsapiUrl() const;
    std::string buildCryptopanicUrl() const;

    // Provider-specific JSON parsers.
    std::vector<NewsArticle> parseNewsapi(const nlohmann::json &j) const;
    std::vector<NewsArticle> parseCryptopanic(const nlohmann::json &j) const;

    NewsConfig m_config;
    std::deque<NewsArticle> m_articles;
    std::unordered_set<std::string> m_seenUrls; ///< For dedup.
    mutable std::mutex m_mutex;
};

} // namespace pulse::ai
