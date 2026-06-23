// gate_auth.cpp — Gate.io v4 API request signing implementation (Layer 1 Exchange)
//
// Dependencies: OpenSSL (libcrypto) for SHA-512 and HMAC-SHA512.
// All functions are pure — no global state, no I/O, thread-safe by design.

#include "exchange/gate_auth.hpp"

#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/sha.h>

#include <chrono>
#include <iomanip>
#include <sstream>

namespace pulse::exchange
{

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

namespace
{

// Convert a raw byte buffer to lowercase hex string.
// 1. Iterate each byte in the buffer
// 2. Format as two-digit lowercase hex with zero-padding
// 3. Accumulate into an ostringstream
std::string toHex(const unsigned char *data, std::size_t len)
{
    std::ostringstream oss;
    for (std::size_t i = 0; i < len; ++i)
    {
        oss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(data[i]);
    }
    return oss.str();
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

std::string sha512Hex(const std::string &data)
{
    // 1. Compute raw SHA-512 digest using the EVP API (preferred over deprecated SHA512())
    unsigned char hash[SHA512_DIGEST_LENGTH];
    SHA512(reinterpret_cast<const unsigned char *>(data.data()), data.size(), hash);

    // 2. Encode as lowercase hex
    return toHex(hash, sizeof(hash));
}

std::string hmacSha512Hex(const std::string &secret, const std::string &payload)
{
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int digest_len = 0;

    // 1. Compute HMAC-SHA512 using OpenSSL's EVP interface
    HMAC(EVP_sha512(),
            secret.data(),
            static_cast<int>(secret.size()),
            reinterpret_cast<const unsigned char *>(payload.data()),
            payload.size(),
            digest,
            &digest_len);

    // 2. Encode as lowercase hex
    return toHex(digest, digest_len);
}

std::string unixSeconds()
{
    // Gate.io v4 API expects Unix timestamp in seconds (not milliseconds).
    auto now_s =
            std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch())
                    .count();
    return std::to_string(now_s);
}

std::string buildSignPayload(
        const std::string &method,
        const std::string &path,
        const std::string &query,
        const std::string &body,
        const std::string &timestamp)
{
    // Canonical v4 sign payload format:
    //   METHOD\nPATH\nQUERY\nSHA512_HEX(BODY)\nTIMESTAMP
    //
    // 1. Hash the request body (empty string for GET/DELETE)
    // 2. Concatenate all components with newline separators
    const std::string body_hash = sha512Hex(body);
    return method + "\n" + path + "\n" + query + "\n" + body_hash + "\n" + timestamp;
}

std::string signRequest(
        const std::string &secret,
        const std::string &method,
        const std::string &path,
        const std::string &query,
        const std::string &body,
        const std::string &timestamp)
{
    // 1. Build the canonical sign payload
    // 2. HMAC-SHA512 it with the secret key
    const std::string payload = buildSignPayload(method, path, query, body, timestamp);
    return hmacSha512Hex(secret, payload);
}

} // namespace pulse::exchange
