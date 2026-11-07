// ControlClient.cpp — see ControlClient.hpp

#ifndef ASIO_STANDALONE
#define ASIO_STANDALONE
#endif

#include "control/ControlClient.hpp"

#include <asio/connect.hpp>
#include <asio/read_until.hpp>
#include <asio/streambuf.hpp>
#include <asio/write.hpp>

#include <chrono>

namespace pulse::control
{

ControlClient::~ControlClient()
{
    disconnect();
}

bool ControlClient::connect(const std::string &host, std::uint16_t port,
                            int timeout_ms)
{
    // Remember the endpoint so call() can transparently reconnect when the
    // engine restarts and the old socket starts failing with broken pipe.
    m_host = host;
    m_port = port;
    m_timeoutMs = timeout_ms;

    disconnect();

    m_ioCtx = std::make_unique<asio::io_context>();
    m_sock = std::make_unique<asio::ip::tcp::socket>(*m_ioCtx);

    try
    {
        asio::ip::tcp::resolver resolver(*m_ioCtx);
        auto endpoints = resolver.resolve(host, std::to_string(port));

        bool connected = false;
        asio::error_code ec;
        asio::steady_timer timer(*m_ioCtx);
        timer.expires_after(std::chrono::milliseconds(timeout_ms));
        timer.async_wait([&](const asio::error_code &)
                         {
                             if (!connected)
                             {
                                 m_sock->cancel(ec);
                             }
                         });

        asio::async_connect(*m_sock, endpoints,
                            [&](const asio::error_code &err,
                                const asio::ip::tcp::endpoint &)
                            {
                                connected = !err;
                                // Cancel the pending timeout so run()
                                // returns as soon as connect resolves.
                                timer.cancel();
                            });
        m_ioCtx->restart();
        m_ioCtx->run();
        if (!connected)
        {
            m_sock.reset();
            return false;
        }
    }
    catch (const std::exception &)
    {
        m_sock.reset();
        return false;
    }

    m_lineBuffer.clear();
    // A fresh socket has not authenticated yet, even if the previous one had.
    m_authenticated = false;
    return true;
}

void ControlClient::setToken(std::string token)
{
    m_token = std::move(token);
    m_authenticated = false;
}

bool ControlClient::authenticate()
{
    // 1. No token configured: nothing to negotiate. An engine that requires
    //    one answers the first real call with 9106, which the caller sees as
    //    ControlAuthFailed.
    if (m_token.empty())
    {
        m_authenticated = true;
        return true;
    }

    // 2. One auth round-trip per connection, before any other request.
    const nlohmann::json params{ { "token", m_token } };
    auto result = tryCallOnce("auth", params);
    if (!ok(result))
    {
        m_authenticated = false;
        return false;
    }
    m_authenticated = true;
    return true;
}

void ControlClient::disconnect()
{
    if (m_sock)
    {
        try
        {
            m_sock->close();
        }
        catch (const std::exception &)
        {
        }
        m_sock.reset();
    }
    if (m_ioCtx)
    {
        m_ioCtx.reset();
    }
}

bool ControlClient::connected() const
{
    return m_sock != nullptr && m_sock->is_open();
}

Result<nlohmann::json>
ControlClient::call(const std::string &method, const nlohmann::json &params)
{
    // Transport failures (ControlEngineUnreachable) trigger one
    // disconnect+reconnect+retry: the engine process may have been restarted
    // since the socket was established, and a stale socket fails every write
    // with a broken pipe that never heals on its own. Protocol errors (e.g.
    // JSON-RPC error responses) are engine business outcomes — never retried.
    for (int attempt = 0; attempt < 2; ++attempt)
    {
        if (!connected())
        {
            if (m_host.empty() || !connect(m_host, m_port, m_timeoutMs))
            {
                return PulseError{ ErrorCode::ControlEngineUnreachable,
                                   "not connected to the trading engine control socket" };
            }
        }

        // Authenticate once per connection before the first real request.
        if (!m_authenticated && !authenticate())
        {
            return PulseError{ ErrorCode::ControlAuthFailed,
                               "control socket authentication failed" };
        }

        auto result = tryCallOnce(method, params);
        if (ok(result) || error(result).code != ErrorCode::ControlEngineUnreachable)
        {
            return result;
        }
        disconnect(); // Stale socket — drop it and reconnect on the next attempt.
    }
    return PulseError{ ErrorCode::ControlEngineUnreachable,
                       "engine unreachable after reconnect retry" };
}

Result<nlohmann::json>
ControlClient::tryCallOnce(const std::string &method, const nlohmann::json &params)
{
    nlohmann::json request{
        { "jsonrpc", "2.0" },
        { "id", m_nextId++ },
        { "method", method },
        { "params", params },
    };

    try
    {
        asio::write(*m_sock, asio::buffer(request.dump() + "\n"));
    }
    catch (const std::exception &e)
    {
        return PulseError{ ErrorCode::ControlEngineUnreachable,
                           std::string("write failed: ") + e.what() };
    }

    try
    {
        asio::streambuf buffer;
        std::size_t n = asio::read_until(*m_sock, buffer, '\n');
        std::string line{ asio::buffer_cast<const char *>(buffer.data()), n };
        while (!line.empty() && ('\n' == line.back() || '\r' == line.back()))
        {
            line.pop_back();
        }

        auto resp = nlohmann::json::parse(line);
        if (resp.contains("error"))
        {
            const auto &err = resp["error"];
            // The control plane reports logical failures with -32603 plus a
            // PulseError in error.data.code. 9106 means the token was missing
            // or wrong — a distinct outcome from a generic protocol error.
            const int dataCode = err.contains("data") && err["data"].is_object()
                                     ? err["data"].value("code", 0)
                                     : 0;
            if (static_cast<int>(ErrorCode::ControlAuthFailed) == dataCode)
            {
                return PulseError{ ErrorCode::ControlAuthFailed,
                                   err.value("message",
                                             "control authentication failed") };
            }
            return PulseError{
                ErrorCode::ControlProtocolError,
                err.value("message", "JSON-RPC error")
                    + " (code=" + std::to_string(err.value("code", -1)) + ")"
            };
        }
        if (!resp.contains("result"))
        {
            return PulseError{ ErrorCode::ControlProtocolError,
                               "response missing result" };
        }
        return resp["result"];
    }
    catch (const std::exception &e)
    {
        return PulseError{ ErrorCode::ControlEngineUnreachable,
                           std::string("read failed: ") + e.what() };
    }
}

} // namespace pulse::control
