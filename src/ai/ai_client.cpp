// ai_client.cpp — HTTP client for OpenAI / Claude LLM backends implementation (Layer 4 AI Analysis)
//
// Architecture:
//   1. analyze() is the public entry point — it orchestrates request building,
//      transport, response parsing, and retry logic
//   2. Backend-specific request/response handling is isolated in build_*_request()
//      and parse_*_response() methods
//   3. The default transport uses libcurl (per-request handles, matching GateRestClient)
//   4. An injectable transport allows unit tests to bypass the network entirely
//
// Retry strategy:
//   - Retries are attempted for: transport errors, HTTP 5xx, and AiTimeout
//   - Non-retryable errors (4xx, AiResponseInvalid, AiSchemaMismatch) return immediately
//   - Backoff is exponential: 500ms, 1000ms, 2000ms, ... (base × 2^attempt)
//   - Maximum attempts = config.maxRetries + 1 (first attempt + retries)
//
// Error flow:
//   curl transport failure → PulseError{NetworkDisconnected|AiTimeout, ...}
//   HTTP 4xx              → PulseError{AiBackendError, ...}
//   HTTP 5xx              → PulseError{AiBackendError, ...} (retryable)
//   JSON parse failure    → PulseError{AiResponseInvalid, ...}
//   Schema mismatch       → PulseError{AiSchemaMismatch, ...}

#include "ai/ai_client.hpp"

#include "logging/logger.hpp"

#include <curl/curl.h>

#include <chrono>
#include <cstdint>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace pulse::ai
{

// ---------------------------------------------------------------------------
// Process-wide curl initialisation (shared with GateRestClient)
//
// curl_global_init must be called once per process before any curl_easy_init.
// Both GateRestClient and AIClient use their own std::call_once flags, but
// curl_global_init is idempotent — calling it multiple times is safe.
// ---------------------------------------------------------------------------

namespace
{

std::once_flag g_ai_curl_init_flag;

void ensure_curl_init()
{
    std::call_once(g_ai_curl_init_flag, []()
            {
                curl_global_init(CURL_GLOBAL_DEFAULT);
            });
}

// libcurl write callback: appends received data chunks to a string buffer.
// Must return size * nmemb to signal success; any other value aborts the transfer.
size_t curl_write_callback(void *contents, size_t size, size_t nmemb, std::string *output)
{
    const size_t total = size * nmemb;
    output->append(static_cast<char *>(contents), total);
    return total;
}

// Determine if an ErrorCode represents a transient failure that may succeed on retry.
// 1. AiTimeout     — the LLM took too long; a retry with fresh connection may succeed
// 2. AiBackendError — server-side error (5xx) or transport failure; retry with backoff
// Non-retryable: AiResponseInvalid, AiSchemaMismatch (client-side data issues)
bool is_retryable_error(ErrorCode code)
{
    return ErrorCode::AiTimeout == code || ErrorCode::AiBackendError == code;
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// Constructors
// ---------------------------------------------------------------------------

AIClient::AIClient(const AiConfig &config)
    : config_(config)
    , transport_(nullptr)
{
    ensure_curl_init();
}

AIClient::AIClient(const AiConfig &config, HttpTransport transport)
    : config_(config)
    , transport_(std::move(transport))
{
    ensure_curl_init();
}

// ---------------------------------------------------------------------------
// analyze — public entry point
//
// Steps:
//   1. Build backend-specific request (URL, body, headers)
//   2. Attempt the request up to (maxRetries + 1) times
//   3. On transient failure, sleep with exponential backoff and retry
//   4. On success, parse the backend-specific response
//   5. Return AnalysisResult or PulseError
// ---------------------------------------------------------------------------
Result<AnalysisResult> AIClient::analyze(
        const std::string &system_prompt,
        const std::string &user_prompt)
{
    // 1. Build backend-specific request components
    const bool is_claude = ("claude" == config_.backend);
    RequestParts req = is_claude
                               ? build_claude_request(system_prompt, user_prompt)
                               : build_openai_request(system_prompt, user_prompt);

    PULSE_LOG_INFO("ai", "LLM request: backend={}, model={}, url={}",
            config_.backend, config_.model, req.url);

    // 2. Retry loop — first attempt plus up to maxRetries retries
    const std::uint32_t max_attempts = config_.maxRetries + 1;

    for (std::uint32_t attempt = 1; attempt <= max_attempts; ++attempt)
    {
        // 2a. Send the request via injected transport or default curl transport
        Result<nlohmann::json> http_result =
                (nullptr != transport_)
                        ? transport_(req.url, req.body, req.headers)
                        : curl_transport(req.url, req.body, req.headers);

        // 2b. Transport-level failure (network error, timeout, HTTP error)
        if (!ok(http_result))
        {
            const auto &err = error(http_result);

            // Non-retryable error — return immediately
            if (!is_retryable_error(err.code))
            {
                PULSE_LOG_ERROR("ai", "LLM request failed (non-retryable): {} — {}",
                        static_cast<int>(err.code), err.message);
                return err;
            }

            // Retryable error — log and back off if attempts remain
            if (attempt < max_attempts)
            {
                const auto backoff_ms = std::chrono::milliseconds(500 * (1u << (attempt - 1)));
                PULSE_LOG_WARN("ai", "LLM attempt {}/{} failed (code={}): {}, retrying in {}ms",
                        attempt, max_attempts,
                        static_cast<int>(err.code), err.message,
                        backoff_ms.count());
                std::this_thread::sleep_for(backoff_ms);
                continue;
            }

            // Exhausted all retries — return the last error
            PULSE_LOG_ERROR("ai", "LLM request failed after {} attempts: {}",
                    max_attempts, err.message);
            return err;
        }

        // 3. Parse the backend-specific response
        Result<AnalysisResult> parse_result =
                is_claude
                        ? parse_claude_response(value(http_result))
                        : parse_openai_response(value(http_result));

        // 4. Parse failure — schema errors are non-retryable
        if (!ok(parse_result))
        {
            const auto &perr = error(parse_result);
            PULSE_LOG_ERROR("ai", "LLM response parse failed: {} — {}",
                    static_cast<int>(perr.code), perr.message);
            return perr;
        }

        PULSE_LOG_INFO("ai", "LLM analysis complete: sentiment={}, confidence={:.2f}, volatility={}",
                to_string(value(parse_result).sentiment),
                value(parse_result).confidence,
                to_string(value(parse_result).volatility));

        return parse_result;
    }

    // Should not reach here (loop always returns), but satisfy the compiler
    return PulseError{ErrorCode::AiBackendError, "Unexpected: retry loop exited without result"};
}

// ---------------------------------------------------------------------------
// build_openai_request — OpenAI Chat Completions API request
//
// Endpoint: POST {baseUrl}/v1/chat/completions
// Body: {
//   "model": "...",
//   "messages": [
//     {"role": "system", "content": "..."},
//     {"role": "user",   "content": "..."}
//   ],
//   "response_format": {"type": "json_object"}
// }
// Headers: Authorization: Bearer {apiKey}, Content-Type: application/json
// ---------------------------------------------------------------------------
AIClient::RequestParts AIClient::build_openai_request(
        const std::string &system_prompt,
        const std::string &user_prompt) const
{
    const std::string base = resolve_base_url();

    nlohmann::json body = {
            {"model",           config_.model},
            {"messages",        {
                    {{"role", "system"}, {"content", system_prompt}},
                    {{"role", "user"},   {"content", user_prompt}},
            }},
            {"response_format", {{"type", "json_object"}}},
    };

    return RequestParts{
            base + "/v1/chat/completions",
            body.dump(),
            {"Authorization: Bearer " + config_.apiKey,
             "Content-Type: application/json"},
    };
}

// ---------------------------------------------------------------------------
// build_claude_request — Claude Messages API request
//
// Endpoint: POST {baseUrl}/v1/messages
// Body: {
//   "model": "...",
//   "system": "...",
//   "messages": [{"role": "user", "content": "..."}],
//   "max_tokens": 1024
// }
// Headers: x-api-key: {apiKey}, anthropic-version: 2023-06-01, Content-Type: application/json
// ---------------------------------------------------------------------------
AIClient::RequestParts AIClient::build_claude_request(
        const std::string &system_prompt,
        const std::string &user_prompt) const
{
    const std::string base = resolve_base_url();

    nlohmann::json body = {
            {"model",      config_.model},
            {"system",     system_prompt},
            {"messages",   {{{"role", "user"}, {"content", user_prompt}}}},
            {"max_tokens", 1024},
    };

    return RequestParts{
            base + "/v1/messages",
            body.dump(),
            {"x-api-key: " + config_.apiKey,
             "anthropic-version: 2023-06-01",
             "Content-Type: application/json"},
    };
}

// ---------------------------------------------------------------------------
// parse_openai_response — extract AnalysisResult from OpenAI Chat Completions
//
// Response structure:
//   { "choices": [{ "message": { "content": "{...json...}" } }] }
//
// Steps:
//   1. Validate choices[0].message.content exists and is a string
//   2. Parse the content string as JSON (non-throwing)
//   3. Deserialize the inner JSON into AnalysisResult (catches schema errors)
// ---------------------------------------------------------------------------
Result<AnalysisResult> AIClient::parse_openai_response(const nlohmann::json &j) const
{
    // 1. Validate the response structure
    if (!j.contains("choices") || !j["choices"].is_array() || j["choices"].empty())
    {
        return PulseError{ErrorCode::AiResponseInvalid,
                          "OpenAI response missing 'choices' array"};
    }

    const auto &choice = j["choices"][0];
    if (!choice.contains("message") || !choice["message"].contains("content"))
    {
        return PulseError{ErrorCode::AiResponseInvalid,
                          "OpenAI response missing 'choices[0].message.content'"};
    }

    const auto &content = choice["message"]["content"];
    if (!content.is_string())
    {
        return PulseError{ErrorCode::AiResponseInvalid,
                          "OpenAI response 'content' is not a string"};
    }

    // 2. Parse the content string as JSON
    const std::string &text = content.get_ref<const std::string &>();
    auto inner = nlohmann::json::parse(text, nullptr, /*allow_exceptions=*/false);
    if (inner.is_discarded())
    {
        PULSE_LOG_WARN("ai", "OpenAI content is not valid JSON: {}", text.substr(0, 200));
        return PulseError{ErrorCode::AiResponseInvalid,
                          "OpenAI response content is not valid JSON"};
    }

    // 3. Deserialize into AnalysisResult
    try
    {
        AnalysisResult result = inner.get<AnalysisResult>();
        return result;
    }
    catch (const std::exception &e)
    {
        return PulseError{ErrorCode::AiSchemaMismatch,
                          std::string("AnalysisResult deserialization failed: ") + e.what()};
    }
}

// ---------------------------------------------------------------------------
// parse_claude_response — extract AnalysisResult from Claude Messages API
//
// Response structure:
//   { "content": [{ "type": "text", "text": "{...json...}" }] }
//
// Steps:
//   1. Validate content[0].text exists and is a string
//   2. Parse the text string as JSON (non-throwing)
//   3. Deserialize the inner JSON into AnalysisResult (catches schema errors)
// ---------------------------------------------------------------------------
Result<AnalysisResult> AIClient::parse_claude_response(const nlohmann::json &j) const
{
    // 1. Validate the response structure
    if (!j.contains("content") || !j["content"].is_array() || j["content"].empty())
    {
        return PulseError{ErrorCode::AiResponseInvalid,
                          "Claude response missing 'content' array"};
    }

    const auto &block = j["content"][0];
    if (!block.contains("text"))
    {
        return PulseError{ErrorCode::AiResponseInvalid,
                          "Claude response missing 'content[0].text'"};
    }

    const auto &text_field = block["text"];
    if (!text_field.is_string())
    {
        return PulseError{ErrorCode::AiResponseInvalid,
                          "Claude response 'text' is not a string"};
    }

    // 2. Parse the text string as JSON
    const std::string &text = text_field.get_ref<const std::string &>();
    auto inner = nlohmann::json::parse(text, nullptr, /*allow_exceptions=*/false);
    if (inner.is_discarded())
    {
        PULSE_LOG_WARN("ai", "Claude content text is not valid JSON: {}", text.substr(0, 200));
        return PulseError{ErrorCode::AiResponseInvalid,
                          "Claude response text is not valid JSON"};
    }

    // 3. Deserialize into AnalysisResult
    try
    {
        AnalysisResult result = inner.get<AnalysisResult>();
        return result;
    }
    catch (const std::exception &e)
    {
        return PulseError{ErrorCode::AiSchemaMismatch,
                          std::string("AnalysisResult deserialization failed: ") + e.what()};
    }
}

// ---------------------------------------------------------------------------
// curl_transport — default HTTP transport using libcurl
//
// Creates a per-request curl easy handle, configures it for a JSON POST,
// sends the request, and returns the parsed JSON response.
//
// Steps:
//   1. Create curl easy handle
//   2. Build header list from the provided strings
//   3. Configure URL, POST body, timeout, write callback
//   4. Execute the request
//   5. Check HTTP status code (2xx = success)
//   6. Parse response body as JSON (non-throwing)
//   7. Clean up handle and header list
// ---------------------------------------------------------------------------
Result<nlohmann::json> AIClient::curl_transport(
        const std::string &url,
        const std::string &body,
        const std::vector<std::string> &headers) const
{
    // 1. Create curl easy handle
    CURL *curl = curl_easy_init();
    if (nullptr == curl)
    {
        return PulseError{ErrorCode::AiBackendError, "curl_easy_init() failed"};
    }

    // 2. Build HTTP header list
    struct curl_slist *header_list = nullptr;
    for (const auto &h : headers)
    {
        header_list = curl_slist_append(header_list, h.c_str());
    }

    // 3. Configure the request
    std::string response_body;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, header_list);
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_body);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, static_cast<long>(config_.requestTimeoutMs));

    // 4. Execute the request
    CURLcode res = curl_easy_perform(curl);

    // 5. Check for transport-level failure
    if (CURLE_OK != res)
    {
        const ErrorCode code = (CURLE_OPERATION_TIMEDOUT == res)
                                       ? ErrorCode::AiTimeout
                                       : ErrorCode::AiBackendError;
        std::string msg = "curl error " + std::to_string(static_cast<int>(res))
                          + ": " + curl_easy_strerror(res);

        curl_slist_free_all(header_list);
        curl_easy_cleanup(curl);

        PULSE_LOG_WARN("ai", "LLM transport failure: {}", msg);
        return PulseError{code, std::move(msg)};
    }

    // 6. Check HTTP status code
    long status_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status_code);

    // Clean up curl resources before returning (response_body is already captured)
    curl_slist_free_all(header_list);
    curl_easy_cleanup(curl);

    if (status_code < 200 || status_code >= 300)
    {
        const ErrorCode code = (status_code >= 500) ? ErrorCode::AiBackendError : ErrorCode::AiResponseInvalid;
        std::string msg = "HTTP " + std::to_string(status_code) + ": " + response_body.substr(0, 500);
        PULSE_LOG_WARN("ai", "LLM HTTP error: {}", msg);
        return PulseError{code, std::move(msg)};
    }

    // 7. Parse the response body as JSON (non-throwing)
    auto json_data = nlohmann::json::parse(response_body, nullptr, /*allow_exceptions=*/false);
    if (json_data.is_discarded())
    {
        PULSE_LOG_ERROR("ai", "LLM response is not valid JSON: {}", response_body.substr(0, 200));
        return PulseError{ErrorCode::AiResponseInvalid, "LLM response body is not valid JSON"};
    }

    return json_data;
}

// ---------------------------------------------------------------------------
// resolve_base_url — determine the API base URL
//
// Priority:
//   1. If config.baseUrl is non-empty, use it as-is (supports custom proxies)
//   2. Otherwise, use the default for the configured backend:
//      - "claude" → "https://api.anthropic.com"
//      - "openai" → "https://api.openai.com"
// ---------------------------------------------------------------------------
std::string AIClient::resolve_base_url() const
{
    // 1. User-configured base URL takes priority (supports proxies, Azure, etc.)
    if (!config_.baseUrl.empty())
    {
        return config_.baseUrl;
    }

    // 2. Default base URLs per backend
    if ("claude" == config_.backend)
    {
        return "https://api.anthropic.com";
    }
    if ("openai" == config_.backend)
    {
        return "https://api.openai.com";
    }

    // 3. Unknown backend — log a warning and fall back to empty (will fail at request time)
    PULSE_LOG_WARN("ai", "Unknown AI backend '{}', no default base URL", config_.backend);
    return "";
}

} // namespace pulse::ai
