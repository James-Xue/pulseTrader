#pragma once
// twitter_feed.hpp — X (Twitter) API v2 social signal ingestion (Layer 4 AI Analysis)
//
// Polls the X API v2 recent-search endpoint for tweets matching configured keywords.
// Maintains a rolling window of recent tweets for inclusion in AI analysis prompts.
//
// Architecture:
//   1. poll() fetches up to 10 tweets per call from the X API v2 recent-search endpoint
//   2. Tweets are deduplicated by ID using a hash set
//   3. A rolling deque enforces the maxTweets window (oldest evicted first)
//   4. recent_text() concatenates tweet text for prompt injection
//
// Thread safety:
//   - All public methods are thread-safe (mutex-protected)
//   - poll() may be called from any thread (typically the heartbeat scheduler)
//   - recent_text() is const and safe to call concurrently with itself

#include "core/config.hpp"

#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <unordered_set>

namespace pulse::ai
{

// ---------------------------------------------------------------------------
// Tweet — single tweet record stored in the rolling window
//
// Fields:
//   1. id        — Tweet ID used for deduplication across polls
//   2. text      — Full tweet text content
//   3. author    — Author username (from author_id field in API v2)
//   4. timestamp — Publication time as Unix milliseconds
// ---------------------------------------------------------------------------
struct Tweet
{
    std::string id;        ///< Tweet ID for dedup.
    std::string text;      ///< Tweet text content.
    std::string author;    ///< Author username.
    std::int64_t timestamp; ///< Unix ms.
};

// ---------------------------------------------------------------------------
// TwitterFeed — polls X API v2 and maintains a rolling tweet window
//
// Usage:
//   1. Construct with TwitterConfig from PulseConfig
//   2. Call poll() periodically (every pollIntervalSec seconds)
//   3. Call recent_text() to get concatenated tweets for the AI prompt
// ---------------------------------------------------------------------------
class TwitterFeed
{
public:
    explicit TwitterFeed(const TwitterConfig &config);

    // Poll X API v2 recent-search endpoint.
    // Returns number of new tweets added to the window.
    // Returns 0 if disabled or on error (logged).
    int poll();

    // Get concatenated text of last N tweets for prompt inclusion.
    [[nodiscard]] std::string recent_text(std::size_t max_count = 5) const;

    // Number of tweets in the rolling window.
    [[nodiscard]] std::size_t size() const;

    // Clear the rolling window.
    void clear();

private:
    // HTTP GET request via libcurl (returns body + status_code).
    // Each call creates and destroys its own curl easy handle (stateless).
    struct HttpResponse
    {
        std::string body;
        long status_code = 0;
    };

    HttpResponse do_request(const std::string &url) const;

    // Build the full recent-search URL from config (base URL + query + fields).
    std::string build_search_url() const;

    TwitterConfig config_;
    std::deque<Tweet> tweets_;
    std::unordered_set<std::string> seen_ids_; ///< For dedup.
    mutable std::mutex mutex_;
};

} // namespace pulse::ai
