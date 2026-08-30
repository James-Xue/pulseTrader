#pragma once
// verify_worker.hpp — background Gate.io credential verification
//
// The login dialog must not block its UI thread on network calls, so the
// verification runs on a detached std::thread and reports back through a
// queued QMetaObject::invokeMethod. Everything here is plain data plus one
// static function — no QObject, no Qt event loop involvement.

#include <QString>

#include "core/config.hpp"

#include <nlohmann/json.hpp>

#include <vector>

namespace pulse::ui
{

/// Permission of the API key for one product group (spot / futures / CFD).
///
/// Filled from GET /account/main_keys (authoritative) when available, or
/// from a side-effect-free write probe (DELETE of a random, non-existent
/// order id) as a fallback. `available` means the read endpoint answered;
/// `known` means the read/write split could be determined.
struct PermissionInfo
{
    QString name;      ///< Raw permission name: "spot", "futures", "tradfi", ...
    bool available = false; ///< Product group is reachable with this key.
    bool readOnly = false;  ///< Meaningful only when available && known.
    bool known = false;     ///< readOnly is authoritative/probe-derived.
};

/// Outcome of a login verification attempt.
struct VerifyResult
{
    bool ok = false;
    QString error;          ///< Human-readable failure reason (shown in the UI).
    QString maskedKey;      ///< Account identity: masked key, e.g. "ab12…cd34".
    double spotUsdt = 0.0;  ///< Available spot USDT balance.
    bool cfdAvailable = false;
    double cfdEquity = 0.0; ///< CFD (TradFi) account equity in USD.
    bool goldAvailable = false;
    double goldPrice = 0.0; ///< XAUUSD last price.
    std::vector<PermissionInfo> permissions; ///< Per-product read/write access.
};

class VerifyWorker
{
  public:
    /// Verify API credentials against Gate.io and collect the login summary:
    ///
    ///   1. GET /api/v4/spot/accounts — proves the key/secret is valid and
    ///      yields the spot USDT balance. Any failure here means the
    ///      credentials are unusable and the whole login fails.
    ///   2. GET /api/v4/tradfi/users/assets — CFD equity. Optional: a 4xx
    ///      (no CFD permission) degrades to cfdAvailable=false, the login
    ///      still succeeds.
    ///   3. GET /api/v4/tradfi/symbols/XAUUSD/tickers — current gold price.
    ///      Optional: failure degrades to goldAvailable=false.
    ///
    /// Never throws; all failures are folded into VerifyResult::error.
    static VerifyResult run(const QString &apiKey, const QString &apiSecret);

    /// Build the exchange config (base URL + proxy resolution with .env
    /// fallback) used by both the login verification and the engine polling.
    static pulse::ExchangeConfig makeExchangeConfig(const QString &apiKey,
                                                    const QString &apiSecret);

    // -----------------------------------------------------------------------
    // Pure parsing helpers (public for unit testing)
    // -----------------------------------------------------------------------

    /// Gate.io returns monetary amounts as decimal strings; accept either
    /// that or a JSON number. Invalid input degrades to 0.0.
    static double jsonToDouble(const nlohmann::json &j);

    /// Normalise a response to its first JSON object: an object passes
    /// through, a one-element array yields its element, anything else
    /// (empty array, scalar) yields nullptr.
    static const nlohmann::json *firstObject(const nlohmann::json &j);

    /// Unwrap the TradFi response envelope. CFD endpoints answer with
    /// {"label", "data": {...}, "timestamp"} — the payload lives in the
    /// "data" sub-object. Returns the inner object when present, otherwise
    /// the object itself. Returns nullptr for a null input.
    static const nlohmann::json *unwrapData(const nlohmann::json *obj);

    /// Parse a GET /account/main_keys response into per-product permissions.
    ///
    /// The response shape varies (a single key record or an array of them;
    /// a record holds {"key", "user_id", "perms": [{"name", "read_only"}]}).
    /// Parsing is lenient: every {"name", "read_only"} entry found anywhere
    /// in the payload is collected; unparseable payloads yield an empty
    /// vector. Only entries with a non-empty name survive.
    static std::vector<PermissionInfo> parsePermissions(const nlohmann::json &mainKeysJson);

    /// Classify the outcome of the write probe (DELETE of a random,
    /// non-existent order id — no side effects on any real order).
    ///
    ///   1. A 403 / INVALID_PERMISSION answer means a read-only key
    ///   2. Any other error (e.g. ORDER_NOT_FOUND) means the write channel
    ///      is open — the key is read-write
    ///   3. Network-level failures (empty message, timeouts, 401) leave the
    ///      outcome Unknown
    ///
    /// `writeSucceeded` (an actually-succeeded DELETE) also classifies as
    /// ReadWrite, though it cannot happen with a non-existent id.
    enum class WriteProbe
    {
        ReadWrite,
        ReadOnly,
        Unknown,
    };
    static WriteProbe classifyWriteProbe(const QString &errorMessage, bool writeSucceeded);
};

} // namespace pulse::ui
