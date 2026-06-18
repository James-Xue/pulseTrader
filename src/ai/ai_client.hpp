#pragma once
// ai_client.hpp — HTTP client for OpenAI / Claude LLM backends (Layer 4 AI Analysis)
//
// Sends prompt pairs (system + user) to an LLM provider and parses the response
// into an AnalysisResult. Supports two backends with different API shapes:
//   1. Claude (Anthropic Messages API) — POST /v1/messages
//   2. OpenAI (Chat Completions API)   — POST /v1/chat/completions
//
// Design rationale:
//   - Injectable HttpTransport for unit testing (no real HTTP in tests)
//   - Per-request curl handles (matches GateRestClient pattern — no persistent state)
//   - Retry with exponential backoff on transient failures (5xx, timeout, transport)
//   - Non-throwing JSON parsing — malformed responses become PulseError, not exceptions
//
// Thread safety:
//   - AIClient stores const config and an optional transport function
//   - Each analyze() call creates its own curl handle — safe to share across threads
//   - curl_global_init is called once per process (shared with GateRestClient)

#include "ai/analysis_result.hpp"
#include "core/config.hpp"
#include "core/error.hpp"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace pulse::ai
{

// ---------------------------------------------------------------------------
// AIClient — sends prompts to an LLM and returns parsed AnalysisResult
//
// Usage (production):
//   AIClient client(config);
//   auto result = client.analyze(system_prompt, user_prompt);
//   if (ok(result)) { use(value(result)); }
//
// Usage (testing):
//   auto mock = [](const auto &, const auto &, const auto &) -> Result<json> {
//       return nlohmann::json{...mock response...};
//   };
//   AIClient client(config, mock);
// ---------------------------------------------------------------------------
class AIClient
{
  public:
    /// Injectable HTTP transport for testing.
    ///
    /// Parameters:
    ///   1. url     — fully-qualified request URL
    ///   2. body    — JSON-encoded request body
    ///   3. headers — HTTP headers (e.g. "Authorization: Bearer ...")
    ///
    /// Returns: parsed JSON response body, or PulseError on failure.
    using HttpTransport = std::function<Result<nlohmann::json>(
            const std::string &url,
            const std::string &body,
            const std::vector<std::string> &headers)>;

    /// Production constructor — uses libcurl for HTTP.
    ///
    /// Calls curl_global_init() on first construction (process-wide, thread-safe).
    explicit AIClient(const AiConfig &config);

    /// Test constructor — inject mock HTTP transport.
    ///
    /// When transport is non-null, analyze() uses it instead of curl.
    /// This allows unit tests to return canned JSON responses without
    /// touching the network.
    AIClient(const AiConfig &config, HttpTransport transport);

    /// Send a prompt to the LLM and parse the response.
    ///
    /// Steps:
    ///   1. Build backend-specific request (URL, body, headers) based on config.backend
    ///   2. Send via transport (injected mock or default curl)
    ///   3. Parse backend-specific response to extract the analysis JSON
    ///   4. Deserialize the analysis JSON into AnalysisResult
    ///   5. Retry on transient failures (up to config.maxRetries attempts)
    ///
    /// Parameters:
    ///   1. system_prompt — the system prompt enforcing JSON schema
    ///   2. user_prompt   — the user prompt with market data
    ///
    /// Returns: AnalysisResult on success, PulseError on failure.
    [[nodiscard]] Result<AnalysisResult> analyze(
            const std::string &system_prompt,
            const std::string &user_prompt);

  private:
    /// Bundle of backend-specific request components.
    struct RequestParts
    {
        std::string url;                      ///< Fully-qualified request URL.
        std::string body;                     ///< JSON-encoded request body.
        std::vector<std::string> headers;     ///< HTTP headers (Authorization, Content-Type, etc.).
    };

    /// Build an OpenAI Chat Completions API request.
    ///
    /// Endpoint: POST {baseUrl}/v1/chat/completions
    /// Body:     {model, messages: [{role:system}, {role:user}], response_format: json_object}
    /// Headers:  Authorization: Bearer {apiKey}, Content-Type: application/json
    [[nodiscard]] RequestParts build_openai_request(
            const std::string &system_prompt,
            const std::string &user_prompt) const;

    /// Build a Claude Messages API request.
    ///
    /// Endpoint: POST {baseUrl}/v1/messages
    /// Body:     {model, system, messages: [{role:user}], max_tokens: 1024}
    /// Headers:  x-api-key: {apiKey}, anthropic-version: 2023-06-01, Content-Type: application/json
    [[nodiscard]] RequestParts build_claude_request(
            const std::string &system_prompt,
            const std::string &user_prompt) const;

    /// Parse an OpenAI Chat Completions response.
    ///
    /// Extracts: choices[0].message.content → parse as JSON → AnalysisResult
    [[nodiscard]] Result<AnalysisResult> parse_openai_response(const nlohmann::json &j) const;

    /// Parse a Claude Messages API response.
    ///
    /// Extracts: content[0].text → parse as JSON → AnalysisResult
    [[nodiscard]] Result<AnalysisResult> parse_claude_response(const nlohmann::json &j) const;

    /// Default HTTP transport using libcurl.
    ///
    /// Creates a per-request curl easy handle, sends the POST, and returns
    /// the parsed JSON response. Non-throwing — parse failures become PulseError.
    [[nodiscard]] Result<nlohmann::json> curl_transport(
            const std::string &url,
            const std::string &body,
            const std::vector<std::string> &headers) const;

    /// Resolve the base URL for the configured backend.
    ///
    /// If config.baseUrl is non-empty, it is used as-is.
    /// Otherwise, defaults are applied:
    ///   - "claude" → "https://api.anthropic.com"
    ///   - "openai" → "https://api.openai.com"
    [[nodiscard]] std::string resolve_base_url() const;

    AiConfig config_;
    HttpTransport transport_; ///< Non-null = use injected transport (testing); null = use curl.
};

} // namespace pulse::ai
