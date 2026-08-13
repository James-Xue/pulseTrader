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
    if (!connected())
    {
        return PulseError{ ErrorCode::ControlEngineUnreachable,
                           "not connected to the trading engine control socket" };
    }

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
