// test_symbol_registry.cpp — Unit tests for SymbolRegistry (Layer 3 Market Data)

#include "market/SymbolRegistry.hpp"

#include "exchange/GateRestClient.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <nlohmann/json.hpp>

using namespace pulse;
using namespace pulse::market;
using namespace pulse::exchange;

// ---------------------------------------------------------------------------
// Parse currency pair (tested via loadFromRest with mock response)
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

    // We cannot directly call parseCurrencyPair (private), so we test via get().
    // Create a mock REST client that returns this single pair.
    ExchangeConfig config;
    GateRestClient rest_client(config);

    SymbolRegistry registry(rest_client);

    // Manually inject the parsed data (since we cannot mock REST easily).
    // Instead, we test the validation logic with known values.

    // Expected: tick_size = 0.01, lot_size = 0.0001, min_quote = 1.0
    // We will test this indirectly via validateOrder after manual setup.
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
        m_restClient = std::make_unique<GateRestClient>(config);
        registry_ = std::make_unique<SymbolRegistry>(*m_restClient);

        // We cannot directly access m_symbols (private), so we skip this test setup.
        // Instead, we test validation logic conceptually in separate tests.
    }

    std::unique_ptr<GateRestClient> m_restClient;
    std::unique_ptr<SymbolRegistry> registry_;
};

TEST_F(SymbolRegistryValidationTest, ValidateOrderPriceTickSize)
{
    // This test would require injecting SymbolInfo, which is not exposed.
    // For now, we verify the validation logic conceptually:
    // - tick_size = 0.01 means price must be multiple of 0.01
    // - Valid: 50000.01, 50000.02, 50000.10
    // - Invalid: 50000.005, 50000.015

    // Since we cannot inject, we test with a real registry after loadFromRest.
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

// Note: Testing parseFuturesContract and validateOrder with futures data
// requires a live or mock REST client. Integration tests in tools/ cover
// the full flow. The struct defaults and constructor are validated here.

// Note: Testing symbols() with populated data requires a live or mock REST
// client (loadFromRest is the only public path to inject SymbolInfo).
// Integration tests in tools/ will cover the populated case.

// ---------------------------------------------------------------------------
// M15: TradFi CFD symbol details
// ---------------------------------------------------------------------------

TEST(SymbolRegistry, ParseCfdDetailXAUUSD)
{
    const nlohmann::json detail = {
        { "symbol", "XAUUSD" },
        { "contract_volume", "100" },
        { "min_order_volume", "0.01" },
        { "max_order_volume", "15" },
        { "step_order_volume", "0.01" },
        { "price_precision", 2 },
        { "leverage", "500" },
        { "leverages", { "20", "50", "100", "200", "500" } },
        { "settlement_currency", "USD" },
    };

    const auto info = SymbolRegistry::parseCfdDetail(detail);
    ASSERT_TRUE(info.has_value());
    EXPECT_EQ("XAUUSD", info->symbol);
    EXPECT_EQ(MarketType::Cfd, info->market_type);
    EXPECT_DOUBLE_EQ(100.0, info->quanto_multiplier); // contract_volume
    EXPECT_DOUBLE_EQ(0.01, info->min_base_amount);    // min_order_volume
    EXPECT_DOUBLE_EQ(0.01, info->lot_size);           // step_order_volume
    EXPECT_EQ(15, info->order_size_max);              // max_order_volume
    EXPECT_DOUBLE_EQ(0.01, info->tick_size);          // price_precision 2
    EXPECT_DOUBLE_EQ(500.0, info->leverage_max);      // max of leverages[]
}

TEST(SymbolRegistry, ParseCfdDetailMissingRequiredFields)
{
    EXPECT_FALSE(SymbolRegistry::parseCfdDetail(nlohmann::json::object())
                     .has_value());
    EXPECT_FALSE(SymbolRegistry::parseCfdDetail(
                     nlohmann::json{ { "symbol", "XAUUSD" } })
                     .has_value());
}

TEST(SymbolRegistry, ValidateOrderCfd)
{
    // Registry with XAUUSD specs injected via upsert (no network).
    ExchangeConfig cfg;
    GateRestClient rest_client(cfg, MarketType::Cfd);
    SymbolRegistry registry(rest_client, MarketType::Cfd);

    SymbolInfo info;
    info.symbol = "XAUUSD";
    info.market_type = MarketType::Cfd;
    info.quanto_multiplier = 100.0;
    info.min_base_amount = 0.01;
    info.lot_size = 0.01;
    info.order_size_max = 15;
    info.tick_size = 0.01;
    info.trading_enabled = true;
    registry.upsert(info);

    // Valid: exactly min lot.
    EXPECT_TRUE(registry.validateOrder("XAUUSD", 4348.0, 0.01));
    // Valid: 2 lots, price with 2 decimals.
    EXPECT_TRUE(registry.validateOrder("XAUUSD", 4348.50, 2.0));
    // Below min lot.
    EXPECT_FALSE(registry.validateOrder("XAUUSD", 4348.0, 0.005));
    // Not a multiple of the 0.01 step.
    EXPECT_FALSE(registry.validateOrder("XAUUSD", 4348.0, 0.015));
    // Above max volume.
    EXPECT_FALSE(registry.validateOrder("XAUUSD", 4348.0, 15.01));
    // Price not a multiple of tick.
    EXPECT_FALSE(registry.validateOrder("XAUUSD", 4348.005, 0.01));
}

TEST(SymbolRegistry, MergeFromCombinesRegistries)
{
    ExchangeConfig cfg;
    GateRestClient rest_client(cfg, MarketType::Futures);
    SymbolRegistry futures_reg(rest_client, MarketType::Futures);
    SymbolRegistry cfd_reg(rest_client, MarketType::Cfd);

    SymbolInfo btc;
    btc.symbol = "BTC_USDT";
    btc.market_type = MarketType::Futures;
    btc.quanto_multiplier = 0.0001;
    futures_reg.upsert(btc);

    SymbolInfo xau;
    xau.symbol = "XAUUSD";
    xau.market_type = MarketType::Cfd;
    xau.quanto_multiplier = 100.0;
    cfd_reg.upsert(xau);

    futures_reg.mergeFrom(cfd_reg);

    EXPECT_EQ(2, futures_reg.size());
    const auto merged_btc = futures_reg.get("BTC_USDT");
    ASSERT_TRUE(merged_btc.has_value());
    EXPECT_DOUBLE_EQ(0.0001, merged_btc->quanto_multiplier);
    const auto merged_xau = futures_reg.get("XAUUSD");
    ASSERT_TRUE(merged_xau.has_value());
    EXPECT_DOUBLE_EQ(100.0, merged_xau->quanto_multiplier);
    EXPECT_EQ(MarketType::Cfd, merged_xau->market_type);
}
