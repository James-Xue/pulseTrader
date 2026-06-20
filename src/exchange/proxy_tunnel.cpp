// proxy_tunnel.cpp — HTTP CONNECT tunneling for WebSocket through HTTP proxy

#ifndef ASIO_STANDALONE
#define ASIO_STANDALONE
#endif

#include "exchange/proxy_tunnel.hpp"

#include "logging/logger.hpp"

#include <asio/connect.hpp>
#include <asio/read_until.hpp>
#include <asio/streambuf.hpp>
#include <asio/write.hpp>

#include <poll.h>

#include <cstdlib>
#include <istream>
#include <sstream>

namespace pulse::exchange
{

using namespace pulse::logging;

// ---------------------------------------------------------------------------
// Free functions
// ---------------------------------------------------------------------------

WsUrlParts parse_ws_url(const std::string &url)
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

std::string detect_proxy_url(const ExchangeConfig &config)
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
    : proxy_url_(proxy_url)
    , target_host_(target_host)
    , target_port_(target_port)
    , target_path_(target_path)
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
    auto scheme_end = proxy_url_.find("://");
    std::string host_part = (std::string::npos != scheme_end) ? proxy_url_.substr(scheme_end + 3) : proxy_url_;
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
    local_port_ = 0;
    acceptor_ = std::make_unique<asio::ip::tcp::acceptor>(
        io_ctx_, asio::ip::tcp::endpoint(asio::ip::address::from_string("127.0.0.1"), 0));
    acceptor_->listen();
    local_port_ = acceptor_->local_endpoint().port();

    // 3. Start accept thread (poll-based so stop() can interrupt it)
    running_ = true;
    accept_thread_ = std::thread([this, proxy_host, proxy_port]()
    {
        while (running_)
        {
            try
            {
                // Use poll() with timeout so we can check running_ periodically
                struct pollfd pfd;
                pfd.fd = acceptor_->native_handle();
                pfd.events = POLLIN;
                pfd.revents = 0;

                const int ret = ::poll(&pfd, 1, 200); // 200ms timeout
                if (ret <= 0)
                {
                    continue; // timeout or error — check running_ again
                }

                asio::ip::tcp::socket local_sock(io_ctx_);
                acceptor_->accept(local_sock);

                // Bug fix #1: Track the handle_connection thread instead of
                // detaching it. This prevents use-after-free when stop() is
                // called while handle_connection is still running.
                // Join any previous connection thread first (we only handle
                // one connection at a time for a single WS client).
                if (connection_thread_.joinable())
                {
                    connection_thread_.join();
                }
                connection_thread_ = std::thread(
                    &ProxyTunnel::handle_connection, this, std::move(local_sock), proxy_host, proxy_port);
            }
            catch (const std::exception &)
            {
                if (!running_)
                {
                    break;
                }
            }
        }
    });

    return "wss://127.0.0.1:" + std::to_string(local_port_) + target_path_;
}

void ProxyTunnel::stop()
{
    running_ = false;
    if (acceptor_)
    {
        asio::error_code ec;
        acceptor_->close(ec);
    }
    if (accept_thread_.joinable())
    {
        accept_thread_.join();
    }

    // Close all relay sockets to unblock read_some() calls
    {
        std::lock_guard lock(relay_mutex_);
        for (auto &sock : relay_sockets_)
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
        std::lock_guard lock(relay_mutex_);
        for (auto &t : relay_threads_)
        {
            if (t.joinable())
            {
                t.join();
            }
        }
        relay_threads_.clear();
        relay_sockets_.clear();
    }

    // Bug fix #1: Join the connection thread (handle_connection)
    if (connection_thread_.joinable())
    {
        connection_thread_.join();
    }
}

std::uint16_t ProxyTunnel::local_port() const
{
    return local_port_;
}

// ---------------------------------------------------------------------------
// Private
// ---------------------------------------------------------------------------

void ProxyTunnel::handle_connection(asio::ip::tcp::socket local_sock,
                                    const std::string &proxy_host,
                                    const std::string &proxy_port)
{
    try
    {
        // 1. Connect to the HTTP proxy
        asio::ip::tcp::socket remote_sock(io_ctx_);
        asio::ip::tcp::resolver resolver(io_ctx_);
        auto endpoints = resolver.resolve(proxy_host, proxy_port);
        asio::connect(remote_sock, endpoints);

        // 2. Send HTTP CONNECT request to establish tunnel
        const std::string target = target_host_ + ":" + std::to_string(target_port_);
        const std::string connect_req = "CONNECT " + target + " HTTP/1.1\r\n"
                                        "Host: " + target + "\r\n"
                                        "Proxy-Connection: keep-alive\r\n\r\n";
        asio::write(remote_sock, asio::buffer(connect_req));

        // 3. Read proxy response — expect "HTTP/1.x 200 Connection established"
        asio::streambuf response_buf;
        asio::read_until(remote_sock, response_buf, "\r\n\r\n");
        std::istream response_stream(&response_buf);
        std::string status_line;
        std::getline(response_stream, status_line);

        if (std::string::npos == status_line.find("200"))
        {
            PULSE_LOG_ERROR("exchange", "WS proxy CONNECT failed: {}", status_line);
            return;
        }

        PULSE_LOG_DEBUG("exchange", "WS proxy tunnel established to {}", target);

        // 4. Bidirectional relay using blocking I/O in two threads
        auto local_ptr = std::make_shared<asio::ip::tcp::socket>(std::move(local_sock));
        auto remote_ptr = std::make_shared<asio::ip::tcp::socket>(std::move(remote_sock));

        // Bug fix #2: Register sockets and threads in a single locked section.
        // Previously there was a gap between socket registration and thread
        // registration where stop() could close sockets but not join threads.
        std::thread t1([local_ptr, remote_ptr]()
        {
            relay_data(*local_ptr, *remote_ptr);
        });

        std::thread t2([local_ptr, remote_ptr]()
        {
            relay_data(*remote_ptr, *local_ptr);
            // When remote→local ends, close both to unblock the other direction
            asio::error_code ec;
            local_ptr->shutdown(asio::ip::tcp::socket::shutdown_both, ec);
            remote_ptr->shutdown(asio::ip::tcp::socket::shutdown_both, ec);
        });

        {
            std::lock_guard lock(relay_mutex_);
            relay_sockets_.push_back(local_ptr);
            relay_sockets_.push_back(remote_ptr);
            relay_threads_.push_back(std::move(t1));
            relay_threads_.push_back(std::move(t2));
        }
    }
    catch (const std::exception &e)
    {
        PULSE_LOG_ERROR("exchange", "WS proxy tunnel error: {}", e.what());
    }
}

void ProxyTunnel::relay_data(asio::ip::tcp::socket &source, asio::ip::tcp::socket &sink)
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
