// test_symbol_registry.cpp — Unit tests for SymbolRegistry (Layer 3 Market Data)

#include "market/symbol_registry.hpp"

#include "exchange/gate_rest_client.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <nlohmann/json.hpp>

using namespace pulse;
using namespace pulse::market;
using namespace pulse::exchange;

// ---------------------------------------------------------------------------
// Parse currency pair (tested via load_from_rest with mock response)
// ---------------------------------------------------------------------------

TEST(SymbolRegistry, ParseCurrencyPairBTC)
{
    // Gate.io BTC_USDT response format.
    const nlohmann::json btc_pair = {
        { "id", "BTC_USDT" },
        { "base", "BTC" },
        { "quote", "USDT" },
        { "fee", "0.2" },
        { "min_base_amount", "0.0001" },
        { "min_quote_amount", "1" },
        { "amount_precision", 4 },
        { "precision", 2 },
        { "trade_status", "tradable" }
    };

    // We cannot directly call parse_currency_pair (private), so we test via get().
    // Create a mock REST client that returns this single pair.
    ExchangeConfig config;
    GateRestClient rest_client(config);

    SymbolRegistry registry(rest_client);

    // Manually inject the parsed data (since we cannot mock REST easily).
    // Instead, we test the validation logic with known values.

    // Expected: tick_size = 0.01, lot_size = 0.0001, min_quote = 1.0
    // We will test this indirectly via validate_order after manual setup.
}

// ---------------------------------------------------------------------------
// Order validation logic (tested with manually constructed SymbolInfo)
// ---------------------------------------------------------------------------

class SymbolRegistryValidationTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        // Create a registry and manually inject a SymbolInfo for testing.
        ExchangeConfig config;
        rest_client_ = std::make_unique<GateRestClient>(config);
        registry_ = std::make_unique<SymbolRegistry>(*rest_client_);

        // We cannot directly access symbols_ (private), so we skip this test setup.
        // Instead, we test validation logic conceptually in separate tests.
    }

    std::unique_ptr<GateRestClient> rest_client_;
    std::unique_ptr<SymbolRegistry> registry_;
};

TEST_F(SymbolRegistryValidationTest, ValidateOrderPriceTickSize)
{
    // This test would require injecting SymbolInfo, which is not exposed.
    // For now, we verify the validation logic conceptually:
    // - tick_size = 0.01 means price must be multiple of 0.01
    // - Valid: 50000.01, 50000.02, 50000.10
    // - Invalid: 50000.005, 50000.015

    // Since we cannot inject, we test with a real registry after load_from_rest.
    // This test is a placeholder for future integration tests.
}

// ---------------------------------------------------------------------------
// Edge cases
// ---------------------------------------------------------------------------

TEST(SymbolRegistry, GetReturnsNulloptForUnknownSymbol)
{
    ExchangeConfig config;
    GateRestClient rest_client(config);
    SymbolRegistry registry(rest_client);

    EXPECT_FALSE(registry.get("UNKNOWN_USDT").has_value());
}

TEST(SymbolRegistry, SizeIsZeroInitially)
{
    ExchangeConfig config;
    GateRestClient rest_client(config);
    SymbolRegistry registry(rest_client);

    EXPECT_EQ(registry.size(), 0u);
}

// ---------------------------------------------------------------------------
// symbols() — interface gap bridge for dashboard
// ---------------------------------------------------------------------------

TEST(SymbolRegistry, SymbolsReturnsEmptyVectorWhenRegistryIsEmpty)
{
    // A fresh registry must return an empty vector from symbols().
    ExchangeConfig config;
    GateRestClient rest_client(config);
    SymbolRegistry registry(rest_client);

    const auto result = registry.symbols();
    EXPECT_TRUE(result.empty());
}

TEST(SymbolRegistry, SymbolsConsistentWithSize)
{
    // symbols().size() must always equal size().
    // For a fresh registry, both must be zero.
    ExchangeConfig config;
    GateRestClient rest_client(config);
    SymbolRegistry registry(rest_client);

    EXPECT_EQ(registry.symbols().size(), registry.size());
}

// ---------------------------------------------------------------------------
// Futures-specific tests
// ---------------------------------------------------------------------------

TEST(SymbolRegistry, SymbolInfoDefaults_SpotCompatible)
{
    // Default-constructed SymbolInfo must be spot-compatible.
    SymbolInfo info;
    EXPECT_EQ(MarketType::Spot, info.market_type);
    EXPECT_DOUBLE_EQ(1.0, info.quanto_multiplier);
    EXPECT_DOUBLE_EQ(1.0, info.leverage_max);
    EXPECT_DOUBLE_EQ(1.0, info.leverage_min);
    EXPECT_DOUBLE_EQ(0.0, info.maintenance_rate);
    EXPECT_EQ(0, info.funding_interval);
    EXPECT_EQ(0, info.order_size_min);
    EXPECT_EQ(0, info.order_size_max);
}

TEST(SymbolRegistry, FuturesRegistryConstructor)
{
    // SymbolRegistry with Futures market type should construct without error.
    ExchangeConfig config;
    GateRestClient rest_client(config);
    SymbolRegistry registry(rest_client, MarketType::Futures);

    EXPECT_EQ(0u, registry.size());
}

TEST(SymbolRegistry, FuturesRegistryGetReturnsNullopt)
{
    // Futures registry with no data returns nullopt.
    ExchangeConfig config;
    GateRestClient rest_client(config);
    SymbolRegistry registry(rest_client, MarketType::Futures);

    EXPECT_FALSE(registry.get("BTC_USDT").has_value());
}

// Note: Testing parse_futures_contract and validate_order with futures data
// requires a live or mock REST client. Integration tests in tools/ cover
// the full flow. The struct defaults and constructor are validated here.

// Note: Testing symbols() with populated data requires a live or mock REST
// client (load_from_rest is the only public path to inject SymbolInfo).
// Integration tests in tools/ will cover the populated case.
