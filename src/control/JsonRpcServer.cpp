// JsonRpcServer.cpp — see JsonRpcServer.hpp

#ifndef ASIO_STANDALONE
#define ASIO_STANDALONE
#endif

#include "control/JsonRpcServer.hpp"

#include "control/EngineServices.hpp"
#include "logging/Logger.hpp"

#include <asio/read_until.hpp>
#include <asio/streambuf.hpp>
#include <asio/write.hpp>

#include <poll.h>

#include <iostream>

namespace pulse::control
{

namespace
{

constexpr const char *kJsonRpcVersion = "2.0";

/// Build a JSON-RPC error response object.
nlohmann::json makeError(int code, const std::string &message,
                         const nlohmann::json &data = nullptr)
{
    nlohmann::json err;
    err["code"] = code;
    err["message"] = message;
    if (!data.is_null())
    {
        err["data"] = data;
    }
    return err;
}

/// Convert an OrderResponse into its JSON form.
nlohmann::json orderResponseToJson(const execution::OrderResponse &resp)
{
    return nlohmann::json{
        { "order_id", resp.order_id },
        { "status", static_cast<int>(resp.status) },
        { "submit_time",
          std::chrono::duration_cast<std::chrono::milliseconds>(
              resp.submit_time.time_since_epoch()).count() },
    };
}

/// Convert a Result<OrderResponse> into an RpcResult.
RpcResult orderResultToRpc(Result<execution::OrderResponse> result)
{
    if (ok(result))
    {
        return RpcResult{ orderResponseToJson(value(result)) };
    }
    return RpcResult{ error(result) };
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// Dispatch (pure, socket-free)
// ---------------------------------------------------------------------------
std::string JsonRpcServer::dispatchLine(const std::string &line,
                                        const MethodRegistry &registry)
{
    nlohmann::json req;
    try
    {
        req = nlohmann::json::parse(line);
    }
    catch (const nlohmann::json::parse_error &)
    {
        return nlohmann::json{
            { "jsonrpc", kJsonRpcVersion },
            { "id", nullptr },
            { "error", makeError(-32700, "Parse error") },
        }.dump();
    }

    // Must be an object with a string "method".
    if (!req.is_object() || !req.contains("method")
        || !req["method"].is_string())
    {
        return nlohmann::json{
            { "jsonrpc", kJsonRpcVersion },
            { "id", nullptr },
            { "error", makeError(-32600, "Invalid request") },
        }.dump();
    }

    const std::string method = req["method"].get<std::string>();

    // Notification: no id → dispatch, return nothing.
    const bool is_notification = !req.contains("id") || req["id"].is_null();
    const nlohmann::json params =
        req.contains("params") ? req["params"] : nlohmann::json::object();

    // Unknown method.
    const auto it = registry.find(method);
    if (registry.end() == it)
    {
        const auto err = makeError(-32601, "Method not found",
                                   nlohmann::json{ { "code",
                                       static_cast<int>(ErrorCode::ControlMethodNotFound) } });
        if (is_notification)
        {
            return "";
        }
        return nlohmann::json{
            { "jsonrpc", kJsonRpcVersion },
            { "id", req["id"] },
            { "error", err },
        }.dump();
    }

    // Dispatch.
    RpcResult result;
    try
    {
        result = it->second(params);
    }
    catch (const std::exception &e)
    {
        result = PulseError{ ErrorCode::ControlProtocolError,
                             std::string("handler exception: ") + e.what() };
    }

    if (is_notification)
    {
        return "";
    }

    nlohmann::json resp;
    resp["jsonrpc"] = kJsonRpcVersion;
    resp["id"] = req["id"];

    if (ok(result))
    {
        resp["result"] = value(result);
    }
    else
    {
        const auto &err = error(result);
        resp["error"] = makeError(-32603, err.message,
                                  nlohmann::json{
                                      { "code", static_cast<int>(err.code) },
                                      { "message", err.message },
                                  });
    }
    return resp.dump();
}

// ---------------------------------------------------------------------------
// Server
// ---------------------------------------------------------------------------
JsonRpcServer::JsonRpcServer(std::string bind_address, std::uint16_t port,
                             MethodRegistry registry)
    : m_bindAddress{ std::move(bind_address) }
    , m_port{ port }
    , m_registry{ std::move(registry) }
    , m_acceptor{ m_ioCtx }
{
}

JsonRpcServer::~JsonRpcServer()
{
    stop();
}

bool JsonRpcServer::start()
{
    try
    {
        asio::ip::tcp::resolver resolver(m_ioCtx);
        auto endpoints = resolver.resolve(m_bindAddress, std::to_string(m_port));
        asio::ip::tcp::endpoint endpoint = *endpoints.begin();
        m_acceptor.open(endpoint.protocol());
        m_acceptor.set_option(asio::ip::tcp::acceptor::reuse_address(true));
        m_acceptor.bind(endpoint);
        m_acceptor.listen(asio::socket_base::max_listen_connections);
    }
    catch (const std::exception &e)
    {
        PULSE_LOG_ERROR("control", "Control socket bind failed on {}:{} — {}",
                        m_bindAddress, m_port, e.what());
        return false;
    }

    // If port 0 was requested, report the actual ephemeral port.
    if (0 == m_port)
    {
        m_port = m_acceptor.local_endpoint().port();
    }

    m_running.store(true, std::memory_order_release);
    m_acceptThread = std::thread(&JsonRpcServer::acceptLoop, this);

    PULSE_LOG_INFO("control", "JSON-RPC control socket listening on {}:{}",
                   m_bindAddress, m_port);
    return true;
}

void JsonRpcServer::stop()
{
    if (!m_running.load(std::memory_order_acquire))
    {
        return;
    }
    m_running.store(false, std::memory_order_release);

    try
    {
        m_acceptor.cancel();
        m_acceptor.close();
    }
    catch (const std::exception &)
    {
    }

    if (m_acceptThread.joinable())
    {
        m_acceptThread.join();
    }

    // Close active session sockets to unblock their read loops.
    {
        std::lock_guard lock(m_sessionMutex);
        for (auto &sock : m_sessionSockets)
        {
            try
            {
                // shutdown() wakes a blocking recv in the session thread;
                // close() alone is not guaranteed to.
                sock->shutdown(asio::ip::tcp::socket::shutdown_both);
                sock->close();
            }
            catch (const std::exception &)
            {
            }
        }
        m_sessionSockets.clear();
    }

    std::lock_guard lock(m_sessionMutex);
    for (auto &t : m_sessions)
    {
        if (t.joinable())
        {
            t.join();
        }
    }
    m_sessions.clear();
}

void JsonRpcServer::acceptLoop()
{
    // Non-blocking acceptor + poll() so stop() can interrupt the loop
    // (blocking accept() cannot be woken by close() from another thread).
    m_acceptor.non_blocking(true);

    while (m_running.load(std::memory_order_acquire))
    {
        try
        {
            auto sock = std::make_shared<asio::ip::tcp::socket>(m_ioCtx);
            asio::error_code ec;
            m_acceptor.accept(*sock, ec);
            if (ec)
            {
                if (asio::error::would_block == ec)
                {
                    // No pending connection — poll briefly, then re-check.
                    struct pollfd pfd{ m_acceptor.native_handle(), POLLIN, 0 };
                    ::poll(&pfd, 1, 200);
                    continue;
                }
                // Acceptor closed during stop() — exit the loop.
                break;
            }

            std::lock_guard lock(m_sessionMutex);
            // Both the session thread and m_sessionSockets share the same
            // socket object so stop() can close it to unblock the read loop.
            m_sessions.emplace_back([this, sock]
                                    { handleSession(sock); });
            m_sessionSockets.push_back(sock);
            // Detach is not used — threads are joined in stop().
            // Sessions with finished work are cleaned up opportunistically.
            if (m_sessions.size() > 32)
            {
                m_sessions.erase(
                    std::remove_if(m_sessions.begin(), m_sessions.end(),
                                   [](const std::thread &t)
                                   { return !t.joinable(); }),
                    m_sessions.end());
            }
        }
        catch (const std::exception &)
        {
            // Acceptor closed during stop() — exit the loop.
            break;
        }
    }
}

void JsonRpcServer::handleSession(std::shared_ptr<asio::ip::tcp::socket> sock)
{
    try
    {
        asio::streambuf buffer;
        while (m_running.load(std::memory_order_acquire))
        {
            std::size_t n = asio::read_until(*sock, buffer, '\n');
            if (0 == n)
            {
                break;
            }

            std::string line{
                asio::buffer_cast<const char *>(buffer.data()), n
            };
            buffer.consume(n);

            // Strip trailing \r\n.
            while (!line.empty()
                   && ('\n' == line.back() || '\r' == line.back()))
            {
                line.pop_back();
            }
            if (line.empty())
            {
                continue;
            }

            const std::string response = dispatchLine(line, m_registry);
            if (!response.empty())
            {
                asio::write(*sock, asio::buffer(response + "\n"));
            }
        }
    }
    catch (const std::exception &)
    {
        // Connection closed / reset — session ends.
    }

    try
    {
        sock->close();
    }
    catch (const std::exception &)
    {
    }
}

// ---------------------------------------------------------------------------
// makeMethodRegistry — the 17 control-plane methods bound to EngineServices
// (method names double as MCP tool names)
// ---------------------------------------------------------------------------
MethodRegistry makeMethodRegistry(EngineServices &services)
{
    MethodRegistry reg;

    // --- Queries ---
    reg["get_status"] = [&services](const nlohmann::json &)
    {
        return RpcResult{ services.status() };
    };
    reg["get_account"] = [&services](const nlohmann::json &)
    {
        return RpcResult{ services.account() };
    };
    reg["get_positions"] = [&services](const nlohmann::json &)
    {
        return RpcResult{ services.positions() };
    };
    reg["get_orders"] = [&services](const nlohmann::json &)
    {
        return RpcResult{ services.orders() };
    };
    reg["list_strategies"] = [&services](const nlohmann::json &)
    {
        return RpcResult{ services.strategies() };
    };
    reg["get_strategy_params"] = [&services](const nlohmann::json &params)
    {
        const auto id = params.value("strategy_id", "");
        auto result = services.getStrategyParams(id);
        if (result.is_null())
        {
            return RpcResult{ PulseError{
                ErrorCode::ControlInvalidRequest,
                "get_strategy_params: strategy not found: " + id } };
        }
        return RpcResult{ result };
    };
    reg["set_strategy_param"] = [&services](const nlohmann::json &params)
    {
        const auto id = params.value("strategy_id", "");
        const auto param = params.value("param", "");
        if (!params.contains("value") || !params["value"].is_number())
        {
            return RpcResult{ PulseError{
                ErrorCode::ControlInvalidRequest,
                "set_strategy_param: value (number) is required" } };
        }
        const double value = params["value"].get<double>();
        if (!services.setStrategyParam(id, param, value))
        {
            return RpcResult{ PulseError{
                ErrorCode::ControlInvalidRequest,
                "set_strategy_param: unknown strategy or param: " + id + "." + param } };
        }
        return RpcResult{ services.getStrategyParams(id) };
    };
    reg["get_risk"] = [&services](const nlohmann::json &)
    {
        return RpcResult{ services.risk() };
    };
    reg["get_signals"] = [&services](const nlohmann::json &)
    {
        return RpcResult{ services.signals() };
    };
    reg["sync_positions"] = [&services](const nlohmann::json &)
    {
        return RpcResult{ services.syncPositions() };
    };
    reg["modify_sl_tp"] = [&services](const nlohmann::json &params)
    {
        return RpcResult{ services.modifySlTp(params) };
    };
    reg["get_market"] = [&services](const nlohmann::json &params)
    {
        const auto symbol = params.value("symbol", "");
        if (symbol.empty())
        {
            return RpcResult{ PulseError{
                ErrorCode::ControlInvalidRequest,
                "get_market: symbol is required" } };
        }
        const int levels = params.value("book_levels", 5);
        const int klines = params.value("klines", 0);
        const auto market_type = params.value("market_type", "");
        return RpcResult{ services.market(symbol, levels, klines, market_type) };
    };
    reg["switch_direction"] = [&services](const nlohmann::json &params)
    {
        const auto direction = params.value("direction", "");
        if (direction.empty())
        {
            return RpcResult{ PulseError{
                ErrorCode::ControlInvalidRequest,
                "switch_direction: direction is required (spot/futures/cfd)" } };
        }
        return RpcResult{ services.switchDirection(direction) };
    };

    // --- Commands ---
    reg["open_order"] = [&services](const nlohmann::json &params)
    {
        return orderResultToRpc(services.openOrder(params));
    };
    reg["close_position"] = [&services](const nlohmann::json &params)
    {
        return orderResultToRpc(services.closePosition(params));
    };
    reg["cancel_order"] = [&services](const nlohmann::json &params)
    {
        const auto id = params.value("order_id", "");
        if (id.empty())
        {
            return RpcResult{ PulseError{
                ErrorCode::ControlInvalidRequest,
                "cancel_order: order_id is required" } };
        }
        if (!services.cancelOrder(id))
        {
            return RpcResult{ PulseError{
                ErrorCode::InvalidOrder,
                "cancel_order: order not found or cancel failed: " + id } };
        }
        return RpcResult{ nlohmann::json{ { "cancelled", true },
                                          { "order_id", id } } };
    };
    // Futures trigger orders (price_orders) — M23.
    reg["place_trigger_order"] = [&services](const nlohmann::json &params)
    {
        return RpcResult{ services.placeTriggerOrder(params) };
    };
    reg["list_trigger_orders"] = [&services](const nlohmann::json &params)
    {
        return RpcResult{ services.listTriggerOrders(params) };
    };
    reg["list_futures_orders"] = [&services](const nlohmann::json &params)
    {
        return RpcResult{ services.listFuturesOrders(params) };
    };
    reg["cancel_trigger_order"] = [&services](const nlohmann::json &params)
    {
        return RpcResult{ services.cancelTriggerOrder(params) };
    };
    reg["halt_trading"] = [&services](const nlohmann::json &)
    {
        services.haltTrading();
        return RpcResult{ services.risk() };
    };
    reg["resume_trading"] = [&services](const nlohmann::json &)
    {
        services.resumeTrading();
        return RpcResult{ services.risk() };
    };
    reg["pause_strategy"] = [&services](const nlohmann::json &params)
    {
        const auto id = params.value("strategy_id", "");
        if (!services.pauseStrategy(id))
        {
            return RpcResult{ PulseError{
                ErrorCode::ControlInvalidRequest,
                "pause_strategy: strategy not found: " + id } };
        }
        return RpcResult{ services.strategies() };
    };
    reg["resume_strategy"] = [&services](const nlohmann::json &params)
    {
        const auto id = params.value("strategy_id", "");
        if (!services.resumeStrategy(id))
        {
            return RpcResult{ PulseError{
                ErrorCode::ControlInvalidRequest,
                "resume_strategy: strategy not found: " + id } };
        }
        return RpcResult{ services.strategies() };
    };

    return reg;
}

} // namespace pulse::control
