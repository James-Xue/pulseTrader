// test_verify_worker.cpp — unit tests for the pure parsing helpers of the
// login verification (no network I/O — VerifyWorker::run itself is covered
// by manual smoke tests against the live API).

#include "VerifyWorker.hpp"

#include <nlohmann/json.hpp>

#include <gtest/gtest.h>

using namespace pulse::ui;

// ---------------------------------------------------------------------------
// jsonToDouble — Gate.io returns amounts as decimal strings
// ---------------------------------------------------------------------------

TEST(VerifyWorkerTest, JsonToDoubleAcceptsDecimalString)
{
    EXPECT_DOUBLE_EQ(12.5, VerifyWorker::jsonToDouble(nlohmann::json("12.5")));
    EXPECT_DOUBLE_EQ(0.0, VerifyWorker::jsonToDouble(nlohmann::json("0")));
}

TEST(VerifyWorkerTest, JsonToDoubleAcceptsNumber)
{
    EXPECT_DOUBLE_EQ(3.0, VerifyWorker::jsonToDouble(nlohmann::json(3)));
    EXPECT_DOUBLE_EQ(2.25, VerifyWorker::jsonToDouble(nlohmann::json(2.25)));
}

TEST(VerifyWorkerTest, JsonToDoubleDegradesOnGarbage)
{
    // Non-numeric strings, booleans, null and objects all degrade to 0.0 —
    // a bad ticker must never poison the login summary.
    EXPECT_DOUBLE_EQ(0.0, VerifyWorker::jsonToDouble(nlohmann::json("abc")));
    EXPECT_DOUBLE_EQ(0.0, VerifyWorker::jsonToDouble(nlohmann::json(nullptr)));
    EXPECT_DOUBLE_EQ(0.0, VerifyWorker::jsonToDouble(nlohmann::json(true)));
    EXPECT_DOUBLE_EQ(0.0, VerifyWorker::jsonToDouble(nlohmann::json::object()));
}

// ---------------------------------------------------------------------------
// firstObject — responses may be an object or a one-element array
// ---------------------------------------------------------------------------

TEST(VerifyWorkerTest, FirstObjectPassesObjectsThrough)
{
    const nlohmann::json obj = {{"last_price", "3320.5"}};
    const nlohmann::json *p = VerifyWorker::firstObject(obj);
    ASSERT_NE(nullptr, p);
    EXPECT_EQ("3320.5", p->value("last_price", ""));
}

TEST(VerifyWorkerTest, FirstObjectUnwrapsSingleElementArray)
{
    const nlohmann::json arr = nlohmann::json::array({{{"last_price", "3320.5"}}});
    const nlohmann::json *p = VerifyWorker::firstObject(arr);
    ASSERT_NE(nullptr, p);
    EXPECT_EQ("3320.5", p->value("last_price", ""));
}

TEST(VerifyWorkerTest, FirstObjectRejectsEmptyArrayAndScalars)
{
    EXPECT_EQ(nullptr, VerifyWorker::firstObject(nlohmann::json::array()));
    EXPECT_EQ(nullptr, VerifyWorker::firstObject(nlohmann::json(42)));
    EXPECT_EQ(nullptr, VerifyWorker::firstObject(nlohmann::json(nullptr)));
}

TEST(VerifyWorkerTest, FirstObjectRejectsArrayOfScalars)
{
    // An array whose first element is not an object yields nothing.
    const nlohmann::json arr = nlohmann::json::array({"not-an-object"});
    EXPECT_EQ(nullptr, VerifyWorker::firstObject(arr));
}

// ---------------------------------------------------------------------------
// unwrapData — TradFi responses wrap the payload in a "data" object
// ---------------------------------------------------------------------------

TEST(VerifyWorkerTest, UnwrapDataExtractsDataSubObject)
{
    // New TradFi envelope: {"label", "data": {...}, "timestamp"}.
    const nlohmann::json env = {
        {"label", ""},
        {"data", {{"equity", "203.49"}, {"balance", "203.49"}}},
        {"timestamp", 1788075972475LL},
    };
    const nlohmann::json *inner = VerifyWorker::unwrapData(&env);
    ASSERT_NE(nullptr, inner);
    EXPECT_TRUE(inner->contains("equity"));
}

TEST(VerifyWorkerTest, UnwrapDataPassesPlainObjectsThrough)
{
    // Old-style payloads (and spot responses) have no "data" wrapper.
    const nlohmann::json plain = {{"last_price", "4455.08"}};
    const nlohmann::json *inner = VerifyWorker::unwrapData(&plain);
    ASSERT_NE(nullptr, inner);
    EXPECT_EQ("4455.08", inner->value("last_price", ""));
}

TEST(VerifyWorkerTest, UnwrapDataRejectsNullAndScalarData)
{
    EXPECT_EQ(nullptr, VerifyWorker::unwrapData(nullptr));
    const nlohmann::json scalarData = {{"data", "not-an-object"}};
    const nlohmann::json *inner = VerifyWorker::unwrapData(&scalarData);
    ASSERT_NE(nullptr, inner);
    EXPECT_EQ("not-an-object", inner->value("data", ""));
}

// ---------------------------------------------------------------------------
// parsePermissions — GET /account/main_keys response
// ---------------------------------------------------------------------------

TEST(VerifyWorkerTest, ParsePermissionsExtractsPermsArray)
{
    // Shape (a): a key record holding a perms array.
    const nlohmann::json payload = nlohmann::json::array({{
        {"key", "abc"},
        {"user_id", "12345"},
        {"perms", nlohmann::json::array({
            {{"name", "spot"}, {"read_only", false}},
            {{"name", "futures"}, {"read_only", true}},
        })},
    }});
    const std::vector<PermissionInfo> perms = VerifyWorker::parsePermissions(payload);
    ASSERT_EQ(2, perms.size());
    EXPECT_EQ(QStringLiteral("spot"), perms[0].name);
    EXPECT_FALSE(perms[0].readOnly);
    EXPECT_TRUE(perms[0].known);
    EXPECT_TRUE(perms[0].available);
    EXPECT_EQ(QStringLiteral("futures"), perms[1].name);
    EXPECT_TRUE(perms[1].readOnly);
}

TEST(VerifyWorkerTest, ParsePermissionsAcceptsBarePermsEntry)
{
    // Shape (b): the payload is already a perms entry (or a list of them).
    const nlohmann::json payload = nlohmann::json::array({
        {{"name", "delivery"}, {"read_only", true}},
    });
    const std::vector<PermissionInfo> perms = VerifyWorker::parsePermissions(payload);
    ASSERT_EQ(1, perms.size());
    EXPECT_EQ(QStringLiteral("delivery"), perms[0].name);
    EXPECT_TRUE(perms[0].readOnly);
    EXPECT_TRUE(perms[0].known);
}

TEST(VerifyWorkerTest, ParsePermissionsSkipsMalformedEntries)
{
    // Missing name, non-object entries and scalars are skipped.
    const nlohmann::json payload = nlohmann::json::array({
        {{"key", "no perms here"}},
        {{"name", 42}},
        {{"read_only", true}},
        "junk",
        7,
    });
    const std::vector<PermissionInfo> perms = VerifyWorker::parsePermissions(payload);
    EXPECT_TRUE(perms.empty());
}

TEST(VerifyWorkerTest, ParsePermissionsHandlesScalarPayload)
{
    EXPECT_TRUE(VerifyWorker::parsePermissions(nlohmann::json(nullptr)).empty());
    EXPECT_TRUE(VerifyWorker::parsePermissions(nlohmann::json("x")).empty());
}

TEST(VerifyWorkerTest, ParsePermissionsUnknownReadOnlyWhenMissing)
{
    // read_only absent → known stays false (read/write split undetermined).
    const nlohmann::json payload = nlohmann::json::array({
        {{"name", "wallet"}},
    });
    const std::vector<PermissionInfo> perms = VerifyWorker::parsePermissions(payload);
    ASSERT_EQ(1, perms.size());
    EXPECT_FALSE(perms[0].known);
    EXPECT_TRUE(perms[0].available);
}

// ---------------------------------------------------------------------------
// classifyWriteProbe — verdict of the side-effect-free DELETE probe
// ---------------------------------------------------------------------------

TEST(VerifyWorkerTest, WriteProbeRejectedWith403MeansReadOnly)
{
    EXPECT_EQ(VerifyWorker::WriteProbe::ReadOnly,
              VerifyWorker::classifyWriteProbe(
                  QStringLiteral("HTTP 403 {\"label\":\"INVALID_PERMISSION\"}"), false));
}

TEST(VerifyWorkerTest, WriteProbeOrderNotFoundMeansReadWrite)
{
    // A read-write key answers with ORDER_NOT_FOUND — the write channel works.
    EXPECT_EQ(VerifyWorker::WriteProbe::ReadWrite,
              VerifyWorker::classifyWriteProbe(
                  QStringLiteral("HTTP 404 {\"label\":\"ORDER_NOT_FOUND\"}"), false));
}

TEST(VerifyWorkerTest, WriteProbeNetworkFailureIsUnknown)
{
    EXPECT_EQ(VerifyWorker::WriteProbe::Unknown,
              VerifyWorker::classifyWriteProbe(QStringLiteral(""), false));
    EXPECT_EQ(VerifyWorker::WriteProbe::Unknown,
              VerifyWorker::classifyWriteProbe(
                  QStringLiteral("HTTP 401 {\"label\":\"INVALID_KEY\"}"), false));
    EXPECT_EQ(VerifyWorker::WriteProbe::Unknown,
              VerifyWorker::classifyWriteProbe(
                  QStringLiteral("timeout connecting to api.gateio.ws"), false));
}

TEST(VerifyWorkerTest, WriteProbeSucceededIsReadWrite)
{
    // Cannot happen with a non-existent id, but be explicit anyway.
    EXPECT_EQ(VerifyWorker::WriteProbe::ReadWrite,
              VerifyWorker::classifyWriteProbe(QStringLiteral(""), true));
}
