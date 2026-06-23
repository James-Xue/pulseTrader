// proxy_tunnel.cpp — HTTP CONNECT tunneling for WebSocket through HTTP proxy

#ifndef ASIO_STANDALONE
#define ASIO_STANDALONE
#endif

#include "exchange/ProxyTunnel.hpp"

#include "logging/Logger.hpp"

#include <asio/connect.hpp>
#include <asio/read_until.hpp>
#include <asio/streambuf.hpp>
#include <asio/write.hpp>

// Cross-platform select(): available on both POSIX and Windows (winsock2).
#ifdef _WIN32
#include <winsock2.h>
#else
#include <sys/select.h>
#endif

#include <cstdlib>
#include <istream>
#include <sstream>

namespace pulse::exchange
{

using namespace pulse::logging;

// ---------------------------------------------------------------------------
// Free functions
// ---------------------------------------------------------------------------

WsUrlParts parseWsUrl(const std::string &url)
{
    WsUrlParts result;
    result.port = 443;
    result.path = "/";

    auto scheme_end = url.find("://");
    std::string rest = (std::string::npos != scheme_end) ? url.substr(scheme_end + 3) : url;
    auto path_start = rest.find('/');
    std::string authority = (std::string::npos != path_start) ? rest.substr(0, path_start) : rest;
    result.path = (std::string::npos != path_start) ? rest.substr(path_start) : "/";

    auto colon_pos = authority.find(':');
    if (std::string::npos != colon_pos)
    {
        result.host = authority.substr(0, colon_pos);
        result.port = static_cast<std::uint16_t>(std::stoi(authority.substr(colon_pos + 1)));
    }
    else
    {
        result.host = authority;
    }

    return result;
}

std::string detectProxyUrl(const ExchangeConfig &config)
{
    if (!config.proxyUrl.empty())
    {
        return config.proxyUrl;
    }
    if (const char *proxy = std::getenv("HTTPS_PROXY"); proxy)
    {
        return proxy;
    }
    if (const char *proxy = std::getenv("HTTP_PROXY"); proxy)
    {
        return proxy;
    }
    return {};
}

// ---------------------------------------------------------------------------
// ProxyTunnel
// ---------------------------------------------------------------------------

ProxyTunnel::ProxyTunnel(const std::string &proxy_url,
                         const std::string &target_host,
                         std::uint16_t target_port,
                         const std::string &target_path)
    : m_proxyUrl(proxy_url)
    , m_targetHost(target_host)
    , m_targetPort(target_port)
    , m_targetPath(target_path)
{
}

ProxyTunnel::~ProxyTunnel()
{
    stop();
}

std::string ProxyTunnel::start()
{
    // 1. Parse proxy URL → host:port
    std::string proxy_host;
    std::string proxy_port = "8080";
    auto scheme_end = m_proxyUrl.find("://");
    std::string host_part = (std::string::npos != scheme_end) ? m_proxyUrl.substr(scheme_end + 3) : m_proxyUrl;
    auto colon_pos = host_part.find(':');
    if (std::string::npos != colon_pos)
    {
        proxy_host = host_part.substr(0, colon_pos);
        proxy_port = host_part.substr(colon_pos + 1);
    }
    else
    {
        proxy_host = host_part;
    }

    // 2. Bind local acceptor to a random available port
    m_localPort = 0;
    m_acceptor = std::make_unique<asio::ip::tcp::acceptor>(
        m_ioCtx, asio::ip::tcp::endpoint(asio::ip::address::from_string("127.0.0.1"), 0));
    m_acceptor->listen();
    m_localPort = m_acceptor->local_endpoint().port();

    // 3. Start accept thread (poll-based so stop() can interrupt it)
    m_running = true;
    m_acceptThread = std::thread([this, proxy_host, proxy_port]()
    {
        while (m_running)
        {
            try
            {
                // Use select() with timeout so we can check m_running periodically
                auto native_fd = m_acceptor->native_handle();

#ifndef _WIN32
                // POSIX: FD_SET overflows if fd >= FD_SETSIZE (typically 1024)
                if (static_cast<int>(native_fd) >= FD_SETSIZE)
                {
                    break; // fd too large for select — should not happen for a single acceptor
                }
#endif

                fd_set read_fds;
                FD_ZERO(&read_fds);
                FD_SET(native_fd, &read_fds);
                struct timeval tv;
                tv.tv_sec = 0;
                tv.tv_usec = 200000; // 200ms

#ifdef _WIN32
                // Windows: first arg to select() is ignored
                const int ret = ::select(0, &read_fds, nullptr, nullptr, &tv);
#else
                const int ret = ::select(static_cast<int>(native_fd) + 1, &read_fds, nullptr, nullptr, &tv);
#endif
                if (ret <= 0)
                {
                    continue; // timeout or error — check m_running again
                }

                asio::ip::tcp::socket local_sock(m_ioCtx);
                m_acceptor->accept(local_sock);

                // Bug fix #1: Track the handleConnection thread instead of
                // detaching it. This prevents use-after-free when stop() is
                // called while handleConnection is still running.
                // Join any previous connection thread first (we only handle
                // one connection at a time for a single WS client).
                if (m_connectionThread.joinable())
                {
                    m_connectionThread.join();
                }
                m_connectionThread = std::thread(
                    &ProxyTunnel::handleConnection, this, std::move(local_sock), proxy_host, proxy_port);
            }
            catch (const std::exception &)
            {
                if (!m_running)
                {
                    break;
                }
            }
        }
    });

    return "wss://127.0.0.1:" + std::to_string(m_localPort) + m_targetPath;
}

void ProxyTunnel::stop()
{
    m_running = false;
    if (m_acceptor)
    {
        asio::error_code ec;
        m_acceptor->close(ec);
    }
    if (m_acceptThread.joinable())
    {
        m_acceptThread.join();
    }

    // Close all relay sockets to unblock read_some() calls
    {
        std::lock_guard lock(m_relayMutex);
        for (auto &sock : m_relaySockets)
        {
            if (sock && sock->is_open())
            {
                asio::error_code ec;
                sock->shutdown(asio::ip::tcp::socket::shutdown_both, ec);
                sock->close(ec);
            }
        }
    }

    // Join all relay threads
    {
        std::lock_guard lock(m_relayMutex);
        for (auto &t : m_relayThreads)
        {
            if (t.joinable())
            {
                t.join();
            }
        }
        m_relayThreads.clear();
        m_relaySockets.clear();
    }

    // Close the connecting socket to unblock asio::connect() in handleConnection
    {
        std::lock_guard lock(m_relayMutex);
        if (m_connectingSock && m_connectingSock->is_open())
        {
            asio::error_code ec;
            m_connectingSock->close(ec);
        }
        m_connectingSock.reset();
    }

    // Bug fix #1: Join the connection thread (handleConnection)
    if (m_connectionThread.joinable())
    {
        m_connectionThread.join();
    }
}

std::uint16_t ProxyTunnel::localPort() const
{
    return m_localPort;
}

// ---------------------------------------------------------------------------
// Private
// ---------------------------------------------------------------------------

void ProxyTunnel::handleConnection(asio::ip::tcp::socket local_sock,
                                    const std::string &proxy_host,
                                    const std::string &proxy_port)
{
    try
    {
        // 1. Connect to the HTTP proxy
        // Track the socket so stop() can close it to unblock asio::connect()
        auto remote_ptr = std::make_shared<asio::ip::tcp::socket>(m_ioCtx);
        {
            std::lock_guard lock(m_relayMutex);
            m_connectingSock = remote_ptr;
        }

        asio::ip::tcp::resolver resolver(m_ioCtx);
        auto endpoints = resolver.resolve(proxy_host, proxy_port);
        asio::connect(*remote_ptr, endpoints);

        // 2. Send HTTP CONNECT request to establish tunnel
        const std::string target = m_targetHost + ":" + std::to_string(m_targetPort);
        const std::string connect_req = "CONNECT " + target + " HTTP/1.1\r\n"
                                        "Host: " + target + "\r\n"
                                        "Proxy-Connection: keep-alive\r\n\r\n";
        asio::write(*remote_ptr, asio::buffer(connect_req));

        // 3. Read proxy response — expect "HTTP/1.x 200 Connection established"
        asio::streambuf response_buf;
        asio::read_until(*remote_ptr, response_buf, "\r\n\r\n");
        std::istream response_stream(&response_buf);
        std::string status_line;
        std::getline(response_stream, status_line);

        if (std::string::npos == status_line.find("200"))
        {
            PULSE_LOG_ERROR("exchange", "WS proxy CONNECT failed: {}", status_line);
            return;
        }

        PULSE_LOG_DEBUG("exchange", "WS proxy tunnel established to {}", target);

        // Clear connecting socket — it's now tracked in m_relaySockets
        {
            std::lock_guard lock(m_relayMutex);
            m_connectingSock.reset();
        }

        // 4. Bidirectional relay using blocking I/O in two threads
        auto local_ptr = std::make_shared<asio::ip::tcp::socket>(std::move(local_sock));
        // remote_ptr already created above and used for connect/handshake

        // Bug fix #2: Register sockets and threads in a single locked section.
        // Previously there was a gap between socket registration and thread
        // registration where stop() could close sockets but not join threads.
        std::thread t1([local_ptr, remote_ptr]()
        {
            relayData(*local_ptr, *remote_ptr);
        });

        std::thread t2([local_ptr, remote_ptr]()
        {
            relayData(*remote_ptr, *local_ptr);
            // When remote→local ends, close both to unblock the other direction
            asio::error_code ec;
            local_ptr->shutdown(asio::ip::tcp::socket::shutdown_both, ec);
            remote_ptr->shutdown(asio::ip::tcp::socket::shutdown_both, ec);
        });

        {
            std::lock_guard lock(m_relayMutex);
            m_relaySockets.push_back(local_ptr);
            m_relaySockets.push_back(remote_ptr);
            m_relayThreads.push_back(std::move(t1));
            m_relayThreads.push_back(std::move(t2));
        }
    }
    catch (const std::exception &e)
    {
        PULSE_LOG_ERROR("exchange", "WS proxy tunnel error: {}", e.what());
    }
}

void ProxyTunnel::relayData(asio::ip::tcp::socket &source, asio::ip::tcp::socket &sink)
{
    try
    {
        char buf[8192];
        while (true)
        {
            asio::error_code ec;
            const std::size_t bytes = source.read_some(asio::buffer(buf), ec);
            if (ec)
            {
                break;
            }
            asio::write(sink, asio::buffer(buf, bytes), ec);
            if (ec)
            {
                break;
            }
        }
    }
    catch (...)
    {
    }
}

} // namespace pulse::exchange
