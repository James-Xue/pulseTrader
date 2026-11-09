// JsonRpcServer.cpp — see JsonRpcServer.hpp

#ifndef ASIO_STANDALONE
#define ASIO_STANDALONE
#endif

#include "control/JsonRpcServer.hpp"

#include "control/EngineServices.hpp"
#include "control/DecisionIntake.hpp"
#include "logging/Logger.hpp"

#include <asio/read_until.hpp>
#include <asio/ssl.hpp>
#include <asio/streambuf.hpp>
#include <asio/write.hpp>

#include <openssl/crypto.h>
#include <openssl/sha.h>

#include <poll.h>

#include <array>
#include <chrono>
#include <iostream>

namespace pulse::control
{

/// Owns the SSL context. Defined here so JsonRpcServer.hpp stays free of
/// OpenSSL and asio::ssl.
struct JsonRpcServer::TlsState
{
    asio::ssl::context context{ asio::ssl::context::tls_server };
};

namespace
{
/// The concrete TLS stream type, used for the shared_ptr<void> bridge between
/// acceptLoop() and handleSessionTls().
using TlsStream = asio::ssl::stream<asio::ip::tcp::socket>;

/// Handshake budget. A blocking sync handshake would hang stop() forever on a
/// client that connects and then says nothing.
constexpr std::chrono::seconds kTlsHandshakeBudget{ 5 };
} // namespace

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

void JsonRpcServer::setSecurity(Security security)
{
    m_security = std::move(security);
}

void JsonRpcServer::initTokenDigest()
{
    // 1. An empty token means "authentication disabled", which is the default
    //    and must stay byte-identical to the historical behaviour.
    m_tokenRequired = !m_security.token.empty();
    if (!m_tokenRequired)
    {
        return;
    }

    // 2. Store only the digest. Digesting both sides first also hides the
    //    token length, which a direct std::string comparison would leak.
    SHA256(reinterpret_cast<const unsigned char *>(m_security.token.data()),
           m_security.token.size(), m_tokenDigest.data());
}

bool JsonRpcServer::initTls()
{
    try
    {
        m_tls = std::make_unique<TlsState>();
        auto &context = m_tls->context;

        // 1. TLS 1.2+ only; the deprecated protocol switches keep old clients
        //    from negotiating an obsolete version.
        context.set_options(asio::ssl::context::default_workarounds
                            | asio::ssl::context::no_sslv2
                            | asio::ssl::context::no_sslv3
                            | asio::ssl::context::no_tlsv1
                            | asio::ssl::context::no_tlsv1_1);

        // 2. Certificate chain and matching key.
        context.use_certificate_chain_file(m_security.certPath);
        context.use_private_key_file(m_security.keyPath, asio::ssl::context::pem);

        // 3. Optional mTLS. Off in v1; when on, a client certificate is
        //    required and verified against the configured CA bundle.
        if (m_security.requireClientCert)
        {
            context.load_verify_file(m_security.caPath);
            context.set_verify_mode(asio::ssl::verify_peer
                                    | asio::ssl::verify_fail_if_no_peer_cert);
        }
        return true;
    }
    catch (const std::exception &e)
    {
        PULSE_LOG_ERROR("control", "TLS context setup failed — {}", e.what());
        m_tls.reset();
        return false;
    }
}

bool JsonRpcServer::start()
{
    // Security is prepared before binding: a bad certificate should fail the
    // start, not the first handshake.
    initTokenDigest();
    if (m_security.tlsEnabled && !initTls())
    {
        return false;
    }

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

    PULSE_LOG_INFO("control",
                   "JSON-RPC control socket listening on {}:{} (tls={}, "
                   "token_auth={}, mtls={})",
                   m_bindAddress, m_port, m_security.tlsEnabled,
                   m_tokenRequired, m_security.requireClientCert);
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

    // Close active sessions to unblock their read loops. Each closer knows
    // whether it owns a plain socket or an SSL stream.
    {
        std::lock_guard lock(m_sessionMutex);
        for (auto &closer : m_sessionClosers)
        {
            try
            {
                closer();
            }
            catch (const std::exception &)
            {
            }
        }
        m_sessionClosers.clear();
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

            // The SSL stream takes ownership of the accepted socket, so the
            // closer has to reference whichever object still owns the handle.
            if (m_security.tlsEnabled)
            {
                auto stream = std::make_shared<TlsStream>(std::move(*sock),
                                                          m_tls->context);
                m_sessionClosers.push_back([stream]()
                {
                    asio::error_code ignored;
                    stream->lowest_layer().shutdown(
                        asio::ip::tcp::socket::shutdown_both, ignored);
                    stream->lowest_layer().close(ignored);
                });
                m_sessions.emplace_back(
                    [this, stream]() { handleSessionTls(stream); });
            }
            else
            {
                m_sessionClosers.push_back([sock]()
                {
                    asio::error_code ignored;
                    // shutdown() wakes a blocking recv; close() alone is not
                    // guaranteed to.
                    sock->shutdown(asio::ip::tcp::socket::shutdown_both, ignored);
                    sock->close(ignored);
                });
                m_sessions.emplace_back(
                    [this, sock]() { handleSessionPlain(sock); });
            }
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

template <typename Stream>
bool JsonRpcServer::authenticate(Stream &stream)
{
    // 1. Auth disabled: the connection proceeds exactly as before.
    if (!m_tokenRequired)
    {
        return true;
    }

    // 2. Exactly one line may be spent on the handshake. Any first method
    //    other than `auth` is a failure, so a client that skips authentication
    //    cannot reach the registry.
    asio::streambuf buffer;
    const std::size_t n = asio::read_until(stream, buffer, '\n');
    std::string line{ asio::buffer_cast<const char *>(buffer.data()), n };
    buffer.consume(n);
    while (!line.empty() && ('\n' == line.back() || '\r' == line.back()))
    {
        line.pop_back();
    }

    nlohmann::json id = nullptr;
    bool authenticated = false;

    const nlohmann::json request = nlohmann::json::parse(line, nullptr, false);
    if (!request.is_discarded() && request.is_object())
    {
        if (request.contains("id"))
        {
            id = request["id"];
        }

        const bool isAuth = "auth" == request.value("method", std::string{});
        if (isAuth && request.contains("params") && request["params"].is_object())
        {
            const nlohmann::json token = request["params"].value(
                "token", nlohmann::json{});
            if (token.is_string())
            {
                // Digest the presented token, then compare fixed-length
                // digests in constant time.
                const std::string presented = token.get<std::string>();
                std::array<unsigned char, 32> presentedDigest{};
                SHA256(reinterpret_cast<const unsigned char *>(presented.data()),
                       presented.size(), presentedDigest.data());
                authenticated = (0 == CRYPTO_memcmp(m_tokenDigest.data(),
                                                    presentedDigest.data(), 32));
            }
        }
    }

    // 3. Reply then close on failure. One attempt per connection.
    nlohmann::json reply;
    reply["jsonrpc"] = "2.0";
    reply["id"] = id;
    if (authenticated)
    {
        reply["result"] = { { "authenticated", true } };
    }
    else
    {
        reply["error"] = {
            { "code", -32603 },
            { "message", "authentication failed" },
            { "data", { { "code", static_cast<int>(ErrorCode::ControlAuthFailed) } } },
        };
    }
    asio::write(stream, asio::buffer(reply.dump() + "\n"));
    return authenticated;
}

template <typename Stream>
void JsonRpcServer::runLoop(Stream &stream)
{
    // The line loop is identical for plaintext and TLS: asio::ssl::stream
    // satisfies the same sync-read/sync-write concept as tcp::socket.
    asio::streambuf buffer;
    while (m_running.load(std::memory_order_acquire))
    {
        const std::size_t n = asio::read_until(stream, buffer, '\n');
        if (0 == n)
        {
            break;
        }

        std::string line{ asio::buffer_cast<const char *>(buffer.data()), n };
        buffer.consume(n);

        while (!line.empty() && ('\n' == line.back() || '\r' == line.back()))
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
            asio::write(stream, asio::buffer(response + "\n"));
        }
    }
}

void JsonRpcServer::handleSessionPlain(std::shared_ptr<asio::ip::tcp::socket> sock)
{
    try
    {
        if (authenticate(*sock))
        {
            runLoop(*sock);
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

void JsonRpcServer::handleSessionTls(std::shared_ptr<void> handle)
{
    auto stream = std::static_pointer_cast<TlsStream>(handle);

    try
    {
        // 1. Non-blocking handshake with a deadline. A blocking handshake would
        //    pin the session thread (and stop()) on a silent client.
        stream->lowest_layer().non_blocking(true);
        const auto deadline = std::chrono::steady_clock::now()
                              + kTlsHandshakeBudget;
        for (;;)
        {
            asio::error_code ec;
            stream->handshake(asio::ssl::stream_base::server, ec);
            if (!ec)
            {
                break;
            }
            if (asio::error::would_block != ec && asio::error::try_again != ec)
            {
                return;
            }
            if (!m_running.load(std::memory_order_acquire)
                || std::chrono::steady_clock::now() > deadline)
            {
                return;
            }
            struct pollfd pfd{ stream->lowest_layer().native_handle(), POLLIN, 0 };
            ::poll(&pfd, 1, 100);
        }
        stream->lowest_layer().non_blocking(false);

        // 2. Token gate, then the shared loop.
        if (authenticate(*stream))
        {
            runLoop(*stream);
        }
    }
    catch (const std::exception &)
    {
        // Handshake or read failure — session ends.
    }

    asio::error_code ignored;
    stream->lowest_layer().close(ignored);
}

// ---------------------------------------------------------------------------
// makeMethodRegistry — the 18 control-plane methods bound to EngineServices
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
    reg["get_account"] = [&services](const nlohmann::json &params)
    {
        // Opt-in: see EngineServices::account(). Defaulting to false keeps
        // the polled account snapshot free of pricing REST calls.
        return RpcResult{ services.account(
            params.value("include_spot_balances", false)) };
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
    reg["grid_start"] = [&services](const nlohmann::json &params)
    {
        return RpcResult{ services.gridStart(params) };
    };
    reg["grid_status"] = [&services](const nlohmann::json &)
    {
        return RpcResult{ services.gridStatus() };
    };
    reg["grid_pause"] = [&services](const nlohmann::json &)
    {
        return RpcResult{ services.gridPause() };
    };
    reg["grid_stop"] = [&services](const nlohmann::json &)
    {
        return RpcResult{ services.gridStop() };
    };
    reg["grid_resume"] = [&services](const nlohmann::json &)
    {
        return RpcResult{ services.gridResume() };
    };
    reg["get_param_history"] = [&services](const nlohmann::json &)
    {
        return RpcResult{ services.paramHistory() };
    };
    reg["get_strategy_performance"] = [&services](const nlohmann::json &params)
    {
        const int hours = params.value("hours", 24);
        return RpcResult{ services.strategyPerformance(hours) };
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
    reg["set_strategy_trading"] = [&services](const nlohmann::json &params)
    {
        const auto id = params.value("strategy_id", "");
        if (!params.contains("enabled") || !params["enabled"].is_boolean())
        {
            return RpcResult{ PulseError{
                ErrorCode::ControlInvalidRequest,
                "set_strategy_trading: enabled (boolean) is required" } };
        }
        if (!services.setStrategyTrading(id, params["enabled"].get<bool>()))
        {
            return RpcResult{ PulseError{
                ErrorCode::ControlInvalidRequest,
                "set_strategy_trading: strategy not found: " + id } };
        }
        return RpcResult{ services.strategies() };
    };

    // Decision intake: the bridge from the decision repository to the engine.
    // Validation and dry-run arithmetic only — this path never places an order.
    reg["submit_decision"] = [](const nlohmann::json &params)
    {
        return validateDecision(params);
    };

    return reg;
}

} // namespace pulse::control
