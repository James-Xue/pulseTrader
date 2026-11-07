// test_command_parser.cpp — Unit tests for the REPL command grammar

#include "control/CommandParser.hpp"

#include <gtest/gtest.h>

using namespace pulse;
using namespace pulse::control;

TEST(CommandParser, StatusMapsToGetStatus)
{
    const auto cmd = parseCommandLine("status");
    ASSERT_TRUE(cmd.has_value());
    EXPECT_EQ("get_status", cmd->method);
}

TEST(CommandParser, AccountAliasesBalance)
{
    for (const auto &line : { "account", "balance" })
    {
        const auto cmd = parseCommandLine(line);
        ASSERT_TRUE(cmd.has_value());
        EXPECT_EQ("get_account", cmd->method);
    }
}

TEST(CommandParser, PositionsAndOrders)
{
    EXPECT_EQ("get_positions", parseCommandLine("positions")->method);
    EXPECT_EQ("get_orders", parseCommandLine("orders")->method);
    EXPECT_EQ("list_strategies", parseCommandLine("strategies")->method);
    EXPECT_EQ("get_risk", parseCommandLine("risk")->method);
}

TEST(CommandParser, ParamsTakesStrategyId)
{
    const auto cmd = parseCommandLine("params momentum_scalper_BTC_USDT");
    ASSERT_TRUE(cmd.has_value());
    EXPECT_EQ("get_strategy_params", cmd->method);
    EXPECT_EQ("momentum_scalper_BTC_USDT", cmd->params["strategy_id"]);
}

TEST(CommandParser, SetParsesNumericValue)
{
    const auto cmd = parseCommandLine("set momentum_scalper_BTC_USDT min_confidence 0.75");
    ASSERT_TRUE(cmd.has_value());
    EXPECT_EQ("set_strategy_param", cmd->method);
    EXPECT_EQ("momentum_scalper_BTC_USDT", cmd->params["strategy_id"]);
    EXPECT_EQ("min_confidence", cmd->params["param"]);
    EXPECT_DOUBLE_EQ(0.75, cmd->params["value"].get<double>());
}

TEST(CommandParser, SetWithBadNumberReturnsNullopt)
{
    EXPECT_FALSE(parseCommandLine("set id param abc").has_value());
}

TEST(CommandParser, OpenParsesRequiredArgs)
{
    const auto cmd = parseCommandLine("open BTC_USDT buy 1");
    ASSERT_TRUE(cmd.has_value());
    EXPECT_EQ("open_order", cmd->method);
    EXPECT_EQ("BTC_USDT", cmd->params["symbol"]);
    EXPECT_EQ("buy", cmd->params["side"]);
    EXPECT_DOUBLE_EQ(1.0, cmd->params["quantity"].get<double>());
}

TEST(CommandParser, OpenParsesAllFlags)
{
    const auto cmd = parseCommandLine(
        "open BTC_USDT sell 2 --type limit --price 65000.5 "
        "--market futures --leverage 10 --reduce-only --client-id abc123");
    ASSERT_TRUE(cmd.has_value());
    EXPECT_EQ("sell", cmd->params["side"]);
    EXPECT_EQ("limit", cmd->params["type"]);
    EXPECT_DOUBLE_EQ(65000.5, cmd->params["price"].get<double>());
    EXPECT_EQ("futures", cmd->params["market_type"]);
    EXPECT_DOUBLE_EQ(10.0, cmd->params["leverage"].get<double>());
    EXPECT_TRUE(cmd->params["reduce_only"].get<bool>());
    EXPECT_EQ("abc123", cmd->params["client_order_id"]);
}

TEST(CommandParser, OpenParsesAttachedSlTp)
{
    const auto cmd = parseCommandLine(
        "open XAUUSD buy 0.01 --market cfd --sl 4380.0 --tp 4420.0");
    ASSERT_TRUE(cmd.has_value());
    EXPECT_EQ("cfd", cmd->params["market_type"]);
    EXPECT_DOUBLE_EQ(4380.0, cmd->params["sl_price"].get<double>());
    EXPECT_DOUBLE_EQ(4420.0, cmd->params["tp_price"].get<double>());
}

TEST(CommandParser, SignalsMapsToGetSignals)
{
    const auto cmd = parseCommandLine("signals");
    ASSERT_TRUE(cmd.has_value());
    EXPECT_EQ("get_signals", cmd->method);
}

TEST(CommandParser, OpenWithBadQuantityReturnsNullopt)
{
    EXPECT_FALSE(parseCommandLine("open BTC_USDT buy abc").has_value());
}

TEST(CommandParser, CloseParsesPositionIdOnly)
{
    const auto cmd = parseCommandLine("close BTC_USDT_Buy_1");
    ASSERT_TRUE(cmd.has_value());
    EXPECT_EQ("close_position", cmd->method);
    EXPECT_EQ("BTC_USDT_Buy_1", cmd->params["position_id"]);
}

TEST(CommandParser, CloseParsesQtyAndPrice)
{
    const auto cmd = parseCommandLine("close BTC_USDT_Buy_1 0.5 64000");
    ASSERT_TRUE(cmd.has_value());
    EXPECT_DOUBLE_EQ(0.5, cmd->params["quantity"].get<double>());
    EXPECT_DOUBLE_EQ(64000.0, cmd->params["price"].get<double>());
}

TEST(CommandParser, CancelParsesOrderId)
{
    const auto cmd = parseCommandLine("cancel 123456");
    ASSERT_TRUE(cmd.has_value());
    EXPECT_EQ("cancel_order", cmd->method);
    EXPECT_EQ("123456", cmd->params["order_id"]);
}

TEST(CommandParser, HaltResumePause)
{
    EXPECT_EQ("halt_trading", parseCommandLine("halt")->method);
    EXPECT_EQ("resume_trading", parseCommandLine("resume")->method);
    EXPECT_EQ("pause_strategy",
              parseCommandLine("pause momentum_scalper_BTC_USDT")->method);
    EXPECT_EQ("resume_strategy",
              parseCommandLine("resume-strategy momentum_scalper_BTC_USDT")->method);
}

TEST(CommandParser, AutoTradeParsesOnOff)
{
    const auto on = parseCommandLine("autotrade ema_resonance_scalper_ETH_USDT on");
    ASSERT_TRUE(on.has_value());
    EXPECT_EQ("set_strategy_trading", on->method);
    EXPECT_EQ("ema_resonance_scalper_ETH_USDT", on->params["strategy_id"]);
    EXPECT_TRUE(on->params["enabled"].get<bool>());

    const auto off = parseCommandLine("autotrade ema_resonance_scalper_ETH_USDT off");
    ASSERT_TRUE(off.has_value());
    EXPECT_EQ("set_strategy_trading", off->method);
    EXPECT_FALSE(off->params["enabled"].get<bool>());

    // Bad state → not parsed.
    EXPECT_FALSE(parseCommandLine("autotrade ema_resonance_scalper_ETH_USDT maybe").has_value());
    // Missing state → not parsed.
    EXPECT_FALSE(parseCommandLine("autotrade ema_resonance_scalper_ETH_USDT").has_value());
}

TEST(CommandParser, MarketParsesFlags)
{
    const auto cmd = parseCommandLine(
        "market BTC_USDT --levels 10 --klines 5 --market futures");
    ASSERT_TRUE(cmd.has_value());
    EXPECT_EQ("get_market", cmd->method);
    EXPECT_EQ("BTC_USDT", cmd->params["symbol"]);
    EXPECT_EQ(10, cmd->params["book_levels"].get<int>());
    EXPECT_EQ(5, cmd->params["klines"].get<int>());
    EXPECT_EQ("futures", cmd->params["market_type"]);
}

TEST(CommandParser, LocalCommandsDetected)
{
    EXPECT_TRUE(isLocalCommand("help"));
    EXPECT_TRUE(isLocalCommand("quit"));
    EXPECT_TRUE(isLocalCommand("exit"));
    EXPECT_TRUE(isLocalCommand("?"));
    EXPECT_FALSE(isLocalCommand("status"));
}

TEST(CommandParser, UnknownCommandReturnsNullopt)
{
    EXPECT_FALSE(parseCommandLine("frobnicate").has_value());
    EXPECT_FALSE(parseCommandLine("").has_value());
}

TEST(CommandParser, HelpTextMentionsOpenCommand)
{
    EXPECT_NE(std::string::npos, replHelp().find("open"));
    EXPECT_NE(std::string::npos, replHelp().find("halt"));
}

TEST(CommandParser, SwitchDirectionCommand)
{
    const auto cmd = parseCommandLine("switch cfd");
    ASSERT_TRUE(cmd.has_value());
    EXPECT_EQ("switch_direction", cmd->method);
    EXPECT_EQ("cfd", cmd->params["direction"].get<std::string>());
}

TEST(CommandParser, SwitchDirectionRejectsUnknown)
{
    EXPECT_FALSE(parseCommandLine("switch options").has_value());
}

TEST(CommandParser, OpenOrderWithCfdMarketFlag)
{
    const auto cmd = parseCommandLine("open XAUUSD buy 0.01 --market cfd --leverage 500");
    ASSERT_TRUE(cmd.has_value());
    EXPECT_EQ("open_order", cmd->method);
    EXPECT_EQ("cfd", cmd->params["market_type"].get<std::string>());
    EXPECT_EQ(500.0, cmd->params["leverage"].get<double>());
}

TEST(CommandParser, MarketCommandWithCfdFlag)
{
    const auto cmd = parseCommandLine("market XAUUSD --klines 20 --market cfd");
    ASSERT_TRUE(cmd.has_value());
    EXPECT_EQ("get_market", cmd->method);
    EXPECT_EQ("cfd", cmd->params["market_type"].get<std::string>());
}

TEST(CommandParser, SyncCommandMapsToSyncPositions)
{
    const auto cmd = parseCommandLine("sync");
    ASSERT_TRUE(cmd.has_value());
    EXPECT_EQ("sync_positions", cmd->method);
}

TEST(CommandParser, ModifyCommandParsesSlTp)
{
    const auto cmd = parseCommandLine(
        "modify XAUUSD_Buy_1 --sl 4390.5 --tp 4408");
    ASSERT_TRUE(cmd.has_value());
    EXPECT_EQ("modify_sl_tp", cmd->method);
    EXPECT_EQ("XAUUSD_Buy_1", cmd->params["position_id"]);
    EXPECT_DOUBLE_EQ(4390.5, cmd->params["sl_price"].get<double>());
    EXPECT_DOUBLE_EQ(4408.0, cmd->params["tp_price"].get<double>());
}

TEST(CommandParser, ModifyCommandAllowsSingleField)
{
    const auto cmd = parseCommandLine("modify XAUUSD_Buy_1 --sl 4390.5");
    ASSERT_TRUE(cmd.has_value());
    EXPECT_EQ("modify_sl_tp", cmd->method);
    EXPECT_DOUBLE_EQ(4390.5, cmd->params["sl_price"].get<double>());
    EXPECT_FALSE(cmd->params.contains("tp_price"));
}

TEST(CommandParser, ModifyCommandRejectsUnknownFlag)
{
    EXPECT_FALSE(parseCommandLine("modify XAUUSD_Buy_1 --wat 1").has_value());
}

TEST(CommandParser, TriggerListTakesContract)
{
    const auto cmd = parseCommandLine("trigger list SNDK_USDT");
    ASSERT_TRUE(cmd.has_value());
    EXPECT_EQ("list_trigger_orders", cmd->method);
    EXPECT_EQ("SNDK_USDT", cmd->params["contract"]);
}

TEST(CommandParser, TriggerCancelTakesId)
{
    const auto cmd = parseCommandLine("trigger cancel 2088850508826546176");
    ASSERT_TRUE(cmd.has_value());
    EXPECT_EQ("cancel_trigger_order", cmd->method);
    EXPECT_EQ("2088850508826546176", cmd->params["order_id"]);
}

TEST(CommandParser, TriggerPlaceParsesFlags)
{
    const auto cmd = parseCommandLine(
        "trigger place SNDK_USDT 1720 --rule 2 --size 2 "
        "--order-type close-short-position --tif ioc");
    ASSERT_TRUE(cmd.has_value());
    EXPECT_EQ("place_trigger_order", cmd->method);
    EXPECT_EQ("SNDK_USDT", cmd->params["contract"]);
    EXPECT_DOUBLE_EQ(1720.0, cmd->params["trigger_price"].get<double>());
    EXPECT_DOUBLE_EQ(2.0, cmd->params["rule"].get<double>());
    EXPECT_DOUBLE_EQ(2.0, cmd->params["size"].get<double>());
    EXPECT_EQ("close-short-position", cmd->params["order_type"]);
    EXPECT_EQ("ioc", cmd->params["tif"]);
}

TEST(CommandParser, TriggerPlaceDefaultsOrderType)
{
    const auto cmd = parseCommandLine("trigger place SNDK_USDT 1720");
    ASSERT_TRUE(cmd.has_value());
    EXPECT_EQ("place_trigger_order", cmd->method);
    EXPECT_FALSE(cmd->params.contains("order_type"));
    EXPECT_FALSE(cmd->params.contains("rule"));
}

TEST(CommandParser, TriggerOrdersSubcommandListsExchangeOrders)
{
    const auto cmd = parseCommandLine("trigger orders SNDK_USDT");
    ASSERT_TRUE(cmd.has_value());
    EXPECT_EQ("list_futures_orders", cmd->method);
    EXPECT_EQ("SNDK_USDT", cmd->params["contract"]);
}

// ---------------------------------------------------------------------------
// M27 — grid commands
// ---------------------------------------------------------------------------

TEST(CommandParser, GridStartParsesOverrides)
{
    const auto cmd = parseCommandLine("grid start --levels 12 --qty 2 --step 5 --anchor 2100");
    ASSERT_TRUE(cmd.has_value());
    EXPECT_EQ("grid_start", cmd->method);
    EXPECT_EQ(12, cmd->params["levels"].get<int>());
    EXPECT_DOUBLE_EQ(2.0, cmd->params["qty_per_level"].get<double>());
    EXPECT_DOUBLE_EQ(5.0, cmd->params["step"].get<double>());
    EXPECT_DOUBLE_EQ(2100.0, cmd->params["anchor"].get<double>());
}

TEST(CommandParser, GridStartWithoutOverrides)
{
    const auto cmd = parseCommandLine("grid start");
    ASSERT_TRUE(cmd.has_value());
    EXPECT_EQ("grid_start", cmd->method);
    EXPECT_TRUE(cmd->params.empty());
}

TEST(CommandParser, GridStatusPauseStop)
{
    const auto status = parseCommandLine("grid status");
    ASSERT_TRUE(status.has_value());
    EXPECT_EQ("grid_status", status->method);

    const auto pause = parseCommandLine("grid pause");
    ASSERT_TRUE(pause.has_value());
    EXPECT_EQ("grid_pause", pause->method);

    const auto stop = parseCommandLine("grid stop");
    ASSERT_TRUE(stop.has_value());
    EXPECT_EQ("grid_stop", stop->method);
}

TEST(CommandParser, GridResume)
{
    const auto resume = parseCommandLine("grid resume");
    ASSERT_TRUE(resume.has_value());
    EXPECT_EQ("grid_resume", resume->method);
    EXPECT_TRUE(resume->params.empty());

    // start-only flags on resume are ignored (params stay empty).
    const auto with_flags = parseCommandLine("grid resume --levels 6");
    ASSERT_TRUE(with_flags.has_value());
    EXPECT_EQ("grid_resume", with_flags->method);
    EXPECT_TRUE(with_flags->params.empty());
}
