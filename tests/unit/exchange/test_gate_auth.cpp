// test_gate_auth.cpp — Unit tests for Gate.io v4 signing functions (Layer 1 Exchange)
//
// Tests verify:
//   1. SHA-512 produces correct hex for known inputs (empty string, "abc")
//   2. HMAC-SHA512 matches known test vectors
//   3. build_sign_payload produces the correct canonical format
//   4. sign_request end-to-end produces a valid signature
//   5. unix_seconds returns a plausible timestamp

#include "pulse/exchange/gate_auth.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <string>

namespace pulse::exchange::test
{

// ---------------------------------------------------------------------------
// SHA-512 tests
// ---------------------------------------------------------------------------

TEST(GateAuthTest, Sha512EmptyString)
{
    // SHA-512("") is a well-known value — 128 hex characters.
    const std::string result = sha512_hex("");
    EXPECT_EQ(128u, result.size());
    EXPECT_EQ("cf83e1357eefb8bdf1542850d66d8007d620e4050b5715dc83f4a921d36ce9ce"
              "47d0d13c5d85f2b0ff8318d2877eec2f63b931bd47417a81a538327af927da3e",
            result);
}

TEST(GateAuthTest, Sha512Abc)
{
    // SHA-512("abc") — NIST test vector.
    const std::string result = sha512_hex("abc");
    EXPECT_EQ(128u, result.size());
    EXPECT_EQ("ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a"
              "2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f",
            result);
}

TEST(GateAuthTest, Sha512JsonBody)
{
    // SHA-512 of a typical JSON body — verify it produces 128 hex chars and is deterministic.
    const std::string body = R"({"currency_pair":"BTC_USDT","side":"buy","amount":"0.001","price":"60000"})";
    const std::string hash1 = sha512_hex(body);
    const std::string hash2 = sha512_hex(body);
    EXPECT_EQ(128u, hash1.size());
    EXPECT_EQ(hash1, hash2); // deterministic
}

// ---------------------------------------------------------------------------
// HMAC-SHA512 tests
// ---------------------------------------------------------------------------

TEST(GateAuthTest, HmacSha512KnownVector)
{
    // RFC 4231 Test Case 2:
    //   Key  = "Jefe"
    //   Data = "what do ya want for nothing?"
    //   HMAC-SHA-512 = 164b7a7bfcf819e2e395fbe73b56e0a387bd64222e831fd610270cd7ea250554
    //                  9758bf75c05a994a6d034f65f8f0e6fdcaeab1a34d4a6b4b636e070a38bce737
    const std::string result = hmac_sha512_hex("Jefe", "what do ya want for nothing?");
    EXPECT_EQ(128u, result.size());
    EXPECT_EQ("164b7a7bfcf819e2e395fbe73b56e0a387bd64222e831fd610270cd7ea250554"
              "9758bf75c05a994a6d034f65f8f0e6fdcaeab1a34d4a6b4b636e070a38bce737",
            result);
}

TEST(GateAuthTest, HmacSha512Deterministic)
{
    // Same inputs must produce the same output.
    const std::string secret = "my_secret_key";
    const std::string payload = "GET\n/api/v4/spot/accounts\n\nhash\n12345";
    const std::string sig1 = hmac_sha512_hex(secret, payload);
    const std::string sig2 = hmac_sha512_hex(secret, payload);
    EXPECT_EQ(sig1, sig2);
}

// ---------------------------------------------------------------------------
// unix_seconds tests
// ---------------------------------------------------------------------------

TEST(GateAuthTest, UnixSecondsIsPlausible)
{
    // 1. Call unix_seconds()
    // 2. Parse as integer
    // 3. Verify it is within 10 seconds of the current time
    const std::string ts_str = unix_seconds();
    ASSERT_FALSE(ts_str.empty());

    const long ts = std::stol(ts_str);
    const auto now_s =
            std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch())
                    .count();

    EXPECT_NEAR(static_cast<double>(now_s), static_cast<double>(ts), 10.0);
}

// ---------------------------------------------------------------------------
// build_sign_payload tests
// ---------------------------------------------------------------------------

TEST(GateAuthTest, BuildSignPayloadFormat)
{
    // Verify the canonical format: METHOD\nPATH\nQUERY\nBODY_HASH\nTIMESTAMP
    const std::string result = build_sign_payload("GET", "/api/v4/spot/accounts", "", "", "1700000000");

    // 1. Should contain 4 newlines (5 components)
    // 2. Should start with "GET\n"
    // 3. Should end with the timestamp
    EXPECT_EQ(0u, result.find("GET\n"));
    EXPECT_EQ(result.size() - 10, result.rfind("1700000000"));

    // Verify body hash is SHA-512 of empty string (for GET requests with no body)
    const std::string empty_body_hash = sha512_hex("");
    EXPECT_NE(std::string::npos, result.find(empty_body_hash));
}

TEST(GateAuthTest, BuildSignPayloadWithQuery)
{
    // Verify query string is included in the payload.
    const std::string result =
            build_sign_payload("GET", "/api/v4/spot/tickers", "currency_pair=BTC_USDT", "", "1700000000");
    EXPECT_NE(std::string::npos, result.find("currency_pair=BTC_USDT"));
}

TEST(GateAuthTest, BuildSignPayloadWithBody)
{
    // Verify body hash changes when body changes.
    const std::string body1 = R"({"amount":"1"})";
    const std::string body2 = R"({"amount":"2"})";

    const std::string payload1 = build_sign_payload("POST", "/api/v4/spot/orders", "", body1, "1700000000");
    const std::string payload2 = build_sign_payload("POST", "/api/v4/spot/orders", "", body2, "1700000000");

    EXPECT_NE(payload1, payload2); // different bodies → different payloads
}

// ---------------------------------------------------------------------------
// sign_request end-to-end test
// ---------------------------------------------------------------------------

TEST(GateAuthTest, SignRequestEndToEnd)
{
    // 1. Sign a request with known inputs
    // 2. Verify the signature is 128 hex chars
    // 3. Verify determinism (same inputs → same output)
    const std::string secret = "test_secret_key_12345";
    const std::string method = "GET";
    const std::string path = "/api/v4/spot/accounts";
    const std::string query = "";
    const std::string body = "";
    const std::string timestamp = "1700000000";

    const std::string sig1 = sign_request(secret, method, path, query, body, timestamp);
    const std::string sig2 = sign_request(secret, method, path, query, body, timestamp);

    EXPECT_EQ(128u, sig1.size());
    EXPECT_EQ(sig1, sig2);

    // Verify it is actually an HMAC of the build_sign_payload
    const std::string payload = build_sign_payload(method, path, query, body, timestamp);
    const std::string expected = hmac_sha512_hex(secret, payload);
    EXPECT_EQ(expected, sig1);
}

TEST(GateAuthTest, SignRequestDifferentSecretsDifferentSigs)
{
    // Different secrets must produce different signatures.
    const std::string sig1 = sign_request("secret_a", "GET", "/api/v4/spot/accounts", "", "", "1700000000");
    const std::string sig2 = sign_request("secret_b", "GET", "/api/v4/spot/accounts", "", "", "1700000000");
    EXPECT_NE(sig1, sig2);
}

} // namespace pulse::exchange::test
