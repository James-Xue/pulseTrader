// web_server.cpp — HTTP + WebSocket server implementation (Layer 9 WebUI)
//
// Implements the WebServer using uWebSockets for high-performance HTTP and
// WebSocket handling. The uWS::App runs on a dedicated background thread
// with all route handlers registered before the event loop starts.
//
// Route table:
//   1. GET /api/status   — JSON health check (status, uptime_ms, version)
//   2. GET /api/snapshot — Latest DashboardSnapshot as JSON
//   3. WS  /ws           — Real-time snapshot push via pub/sub topic "snapshot"
//   4. GET /*            — Static file serving from frontend_dir
//
// Security enforcement:
//   - Bearer token auth on /api/* and /ws (skipped if config.authToken is empty)
//   - Host header validation on all API/WS routes (prevents DNS rebinding)
//   - Path traversal prevention on static file serving (rejects ".." in paths)
//
// Lifecycle:
//   - start() spawns a background thread, constructs uWS::App there, registers
//     routes, binds the listen socket, and enters the event loop
//   - stop() closes the listen socket, causing the event loop to return, then
//     joins the background thread and cleans up the Impl

#include "webui/WebServer.hpp"

#include "App.h"           // uWebSockets main header (uWS::App)
#include "Loop.h"          // uWS::Loop for defer()
#include "libusockets.h"   // us_listen_socket_close, us_socket_local_port

#include "logging/Logger.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <condition_variable>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>

namespace pulse::webui
{

// ---------------------------------------------------------------------------
// Impl — PIMPL struct hiding uWebSockets internals from the public header
//
// Members:
//   1. app            — uWS::App instance (must be constructed on the event loop thread)
//   2. listen_socket  — Opaque listen socket handle (used for shutdown)
//   3. event_thread   — Background thread running the event loop
// ---------------------------------------------------------------------------

struct WebServer::Impl
{
    // NOTE: uWS::App must be constructed on the thread that will run app.run().
    // We use unique_ptr so it can be created/destroyed from the event thread.
    std::unique_ptr<uWS::App> app;

    // Listen socket token from app.listen() callback.
    // Used by us_listen_socket_close() to stop the event loop.
    us_listen_socket_t *listen_socket{ nullptr };

    // Background thread running app.run().
    std::thread event_thread;
};

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

WebServer::WebServer(const WebUiConfig &config,
                     DashboardState &state,
                     const std::string &frontend_dir)
    : m_config{ config }
    , m_state{ state }
    , m_frontendDir{ frontend_dir }
    , m_wsServer{ std::make_unique<WsServer>(config) }
{
}

WebServer::~WebServer()
{
    stop();
}

// ---------------------------------------------------------------------------
// start — launch the HTTP server on a background thread
//
// Sequence:
//   1. Guard against double-start
//   2. Create the WsServer and publish function
//   3. Spawn a background thread that:
//      a. Constructs uWS::App (Loop::get() is thread-local)
//      b. Registers API routes (/api/status, /api/snapshot)
//      c. Registers WebSocket route (/ws) with auth and client limits
//      d. Registers static file catch-all (/*)
//      e. Binds the listen socket
//      f. Signals readiness via condition variable
//      g. Runs the event loop (blocks until stop)
//   4. Wait for the listen callback to fire
//   5. Return the bind result
// ---------------------------------------------------------------------------

[[nodiscard]] bool WebServer::start()
{
    // Guard: do not start if already running or if Impl exists.
    if (m_running)
    {
        return true;
    }

    m_impl = std::make_unique<Impl>();

    // Synchronization: atomic flag set by the listen callback.
    // We use an atomic rather than a condition_variable because the lambda
    // runs on a background thread that may outlive the start() call's stack.
    std::atomic<bool> listen_ready{ false };
    std::atomic<bool> bind_result{ false };

    // Record the start time for uptime calculation.
    m_startTime = std::chrono::steady_clock::now();

    // Capture references for the background thread.
    auto &state_ref = m_state;
    auto *ws = m_wsServer.get();

    m_impl->event_thread = std::thread([this, &listen_ready, &bind_result,
                                        &state_ref, ws]()
    {
        // a. Construct uWS::App on this thread.
        //    uWS::App internally calls Loop::get() which is thread-local.
        m_impl->app = std::make_unique<uWS::App>();

        // Check if construction succeeded.
        if (m_impl->app->constructorFailed())
        {
            bind_result.store(false, std::memory_order_release);
            listen_ready.store(true, std::memory_order_release);
            return;
        }

        // Store the loop pointer for the WsServer's publish function.
        uWS::Loop *loop = m_impl->app->getLoop();

        // Set up the publish function: defers app->publish() to this thread.
        // Capture raw pointer to app; safe because Impl outlives the server.
        uWS::App *app_ptr = m_impl->app.get();
        ws->setPublishFn([loop, app_ptr](const std::string &json)
        {
            loop->defer([app_ptr, json]()
            {
                app_ptr->publish("snapshot", json, uWS::OpCode::TEXT);
            });
        });

        // b. Register /api/status — server health check.
        m_impl->app->get("/api/status", [this](auto *res, auto *req)
        {
            // Auth check.
            if (!validateAuth(req->getHeader("authorization")))
            {
                res->writeStatus("401 Unauthorized")
                   ->writeHeader("Content-Type", "text/plain")
                   ->end("Unauthorized");
                return;
            }

            // Host validation.
            if (!validateHost(req->getHeader("host")))
            {
                res->writeStatus("403 Forbidden")
                   ->writeHeader("Content-Type", "text/plain")
                   ->end("Invalid host");
                return;
            }

            // Build the status JSON.
            const auto uptime = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - m_startTime).count();

            nlohmann::json j;
            j["status"] = "ok";
            j["uptime_ms"] = uptime;
            j["version"] = "0.1.0";

            res->writeHeader("Content-Type", "application/json")
               ->end(j.dump());
        });

        // c. Register /api/snapshot — latest dashboard snapshot.
        m_impl->app->get("/api/snapshot", [this, &state_ref](auto *res, auto *req)
        {
            // Auth check.
            if (!validateAuth(req->getHeader("authorization")))
            {
                res->writeStatus("401 Unauthorized")
                   ->writeHeader("Content-Type", "text/plain")
                   ->end("Unauthorized");
                return;
            }

            // Host validation.
            if (!validateHost(req->getHeader("host")))
            {
                res->writeStatus("403 Forbidden")
                   ->writeHeader("Content-Type", "text/plain")
                   ->end("Invalid host");
                return;
            }

            // Retrieve the latest snapshot.
            auto snap = state_ref.latest();
            if (!snap)
            {
                res->writeHeader("Content-Type", "application/json")
                   ->end("{\"error\":\"no snapshot available\"}");
                return;
            }

            nlohmann::json j = *snap;
            res->writeHeader("Content-Type", "application/json")
               ->end(j.dump());
        });

        // d. Register /ws — WebSocket upgrade for real-time snapshot push.
        //    Designated initializers must follow the struct declaration order:
        //    compression, maxPayloadLength, idleTimeout, ..., upgrade, open,
        //    message, dropped, drain, subscription, close.
        m_impl->app->ws<PerSocketData>("/ws", {
            // -- Settings (must come before handlers in declaration order) --
            .maxPayloadLength = 16 * 1024,
            .idleTimeout = 120,
            .sendPingsAutomatically = true,

            // -- Upgrade handler: validate auth, host, and client limit --
            .upgrade = [this, ws](auto *res, auto *req, auto *context)
            {
                // Auth via query parameter: ?token=AUTH_TOKEN
                if (!m_config.authToken.empty())
                {
                    const auto token = req->getQuery("token");
                    if (token != std::string_view{ m_config.authToken })
                    {
                        res->writeStatus("401 Unauthorized")
                           ->end("Unauthorized");
                        return;
                    }
                }

                // Host validation.
                if (!validateHost(req->getHeader("host")))
                {
                    res->writeStatus("403 Forbidden")
                       ->end("Invalid host");
                    return;
                }

                // Client limit check.
                if (ws->clientCount() >= ws->maxClients())
                {
                    res->writeStatus("503 Service Unavailable")
                       ->end("Too many clients");
                    return;
                }

                // Upgrade to WebSocket.
                const auto sec_key = req->getHeader("sec-websocket-key");
                const auto sec_protocol = req->getHeader("sec-websocket-protocol");
                const auto sec_extensions = req->getHeader("sec-websocket-extensions");

                res->template upgrade<PerSocketData>(
                    PerSocketData{},
                    sec_key,
                    sec_protocol,
                    sec_extensions,
                    context
                );
            },

            // -- Open handler: track client, subscribe to topic, send latest --
            .open = [this, ws](auto *ws_conn)
            {
                ws->onWsOpen();

                // Subscribe to the snapshot pub/sub topic.
                ws_conn->subscribe("snapshot");

                // Send the cached latest snapshot immediately on connect.
                const auto cached = ws->lastJson();
                if (!cached.empty())
                {
                    ws_conn->send(cached, uWS::OpCode::TEXT);
                }
            },

            // -- Message handler: server is push-only, client messages ignored --
            .message = [](auto * /*ws_conn*/, auto /*msg*/, auto /*op*/)
            {
            },

            // -- Close handler: untrack client --
            .close = [ws](auto * /*ws_conn*/, int /*code*/, auto /*msg*/)
            {
                ws->onWsClose();
            },
        });

        // e. Register /* — static file serving from m_frontendDir.
        //    This catch-all must be last to avoid shadowing API routes.
        m_impl->app->get("/*", [this](auto *res, auto *req)
        {
            std::string path{ req->getUrl() };
            serveStatic(res, path);
        });

        // f. Bind the listen socket.
        m_impl->app->listen(m_config.bindAddress, static_cast<int>(m_config.port),
            [this, &listen_ready, &bind_result]
            (us_listen_socket_t *token)
            {
                m_impl->listen_socket = token;

                if (token)
                {
                    // Try to get the actual port (useful for port 0).
                    // us_listen_socket_t has us_socket_t as its first member,
                    // so the cast is safe for us_socket_local_port().
                    const int bound_port = us_socket_local_port(
                        0, reinterpret_cast<us_socket_t *>(token));

                    if (0 < bound_port)
                    {
                        m_actualPort = static_cast<std::uint16_t>(bound_port);
                    }
                    else
                    {
                        m_actualPort = m_config.port;
                    }

                    m_running = true;
                    bind_result.store(true, std::memory_order_release);
                }
                else
                {
                    bind_result.store(false, std::memory_order_release);
                }

                // Signal the main thread.
                listen_ready.store(true, std::memory_order_release);
            });

        // g. Run the event loop (blocks until us_listen_socket_close).
        m_impl->app->run();

        // Event loop exited — clean up the app.
        m_impl->app.reset();
    });

    // 4. Wait for the listen callback to fire (poll the atomic flag).
    //    The listen callback fires synchronously during app.listen(), which
    //    runs on the background thread shortly after it starts.
    while (!listen_ready.load(std::memory_order_acquire))
    {
        std::this_thread::sleep_for(std::chrono::milliseconds{ 1 });
    }

    return bind_result.load(std::memory_order_acquire);
}

// ---------------------------------------------------------------------------
// stop — close the listen socket and join the background thread
//
// Sequence:
//   1. Close the listen socket (causes app.run() to return)
//   2. Join the background thread (waits for clean shutdown)
//   3. Reset the Impl (destroys everything)
//   4. Update the running flag
// ---------------------------------------------------------------------------

void WebServer::stop()
{
    if (!m_impl)
    {
        return;
    }

    // 1. Close the listen socket from the calling thread.
    //    This begins the shutdown process.
    if (m_impl->listen_socket)
    {
        us_listen_socket_close(0, m_impl->listen_socket);
        m_impl->listen_socket = nullptr;
    }

    // 2. Defer a full close to the event loop thread. This wakes the loop
    //    immediately (via us_wakeup_loop) and closes all remaining sockets,
    //    which causes app.run() to return without waiting for the next
    //    timer tick (~1s granularity in uSockets).
    if (m_impl->app)
    {
        uWS::App *app_ptr = m_impl->app.get();
        uWS::Loop *loop = app_ptr->getLoop();
        if (loop)
        {
            loop->defer([app_ptr]()
            {
                app_ptr->close();
            });
        }
    }

    // 3. Join the background thread.
    if (m_impl->event_thread.joinable())
    {
        m_impl->event_thread.join();
    }

    // 3. Clean up.
    m_running = false;
    m_actualPort = 0;
    m_impl.reset();
}

// ---------------------------------------------------------------------------
// Getters
// ---------------------------------------------------------------------------

[[nodiscard]] bool WebServer::running() const
{
    return m_running;
}

[[nodiscard]] std::uint16_t WebServer::port() const
{
    return m_actualPort;
}

[[nodiscard]] const WsServer &WebServer::wsServer() const
{
    return *m_wsServer;
}

// ---------------------------------------------------------------------------
// validateAuth — check the Authorization header
//
// Rules:
//   1. If authToken is empty (dev mode), all requests pass
//   2. Header must be exactly "Bearer <authToken>"
//   3. Case-sensitive comparison on both "Bearer" prefix and token value
// ---------------------------------------------------------------------------

[[nodiscard]] bool WebServer::validateAuth(std::string_view auth_header) const
{
    // Dev mode: no auth required.
    if (m_config.authToken.empty())
    {
        return true;
    }

    // Must start with "Bearer " (7 characters).
    constexpr std::string_view prefix{ "Bearer " };
    if (auth_header.size() < prefix.size())
    {
        return false;
    }

    if (auth_header.substr(0, prefix.size()) != prefix)
    {
        return false;
    }

    // Compare the token portion.
    const auto token = auth_header.substr(prefix.size());
    return token == std::string_view{ m_config.authToken };
}

// ---------------------------------------------------------------------------
// validateHost — check the Host header for DNS rebinding protection
//
// Accepts:
//   1. "bindAddress:port" (e.g. "127.0.0.1:8080")
//   2. "localhost:port"
//   3. "127.0.0.1:port"
//
// Returns false for empty or unrecognized host headers.
// ---------------------------------------------------------------------------

[[nodiscard]] bool WebServer::validateHost(std::string_view host_header) const
{
    if (host_header.empty())
    {
        return false;
    }

    const auto port_str = std::to_string(m_actualPort);

    // 1. Check bindAddress:port.
    const auto bind_host = m_config.bindAddress + ":" + port_str;
    if (host_header == std::string_view{ bind_host })
    {
        return true;
    }

    // 2. Check localhost:port.
    const auto localhost_host = "localhost:" + port_str;
    if (host_header == std::string_view{ localhost_host })
    {
        return true;
    }

    // 3. Check 127.0.0.1:port (if bindAddress is not already 127.0.0.1).
    if ("127.0.0.1" != m_config.bindAddress)
    {
        const auto loopback_host = std::string{ "127.0.0.1:" } + port_str;
        if (host_header == std::string_view{ loopback_host })
        {
            return true;
        }
    }

    return false;
}

// ---------------------------------------------------------------------------
// serveStatic — serve a file from the frontend directory
//
// Security:
//   1. Rejects paths containing ".." (directory traversal prevention)
//   2. Maps "/" and empty paths to "/index.html"
//   3. Returns 404 for missing files
//   4. Sets Content-Type from file extension
// ---------------------------------------------------------------------------

template <typename Res>
void WebServer::serveStatic(Res *res, const std::string &path)
{
    // 1. Security: reject directory traversal attempts.
    if (std::string::npos != path.find(".."))
    {
        res->writeStatus("403 Forbidden")
           ->writeHeader("Content-Type", "text/plain")
           ->end("Forbidden");
        return;
    }

    // 2. Map root to index.html.
    std::string file_path = path;
    if ("/" == file_path || file_path.empty())
    {
        file_path = "/index.html";
    }

    // 3. Construct the full filesystem path.
    const auto full_path = m_frontendDir + file_path;

    // 4. Read the file.
    std::ifstream file{ full_path, std::ios::binary };
    if (!file.is_open())
    {
        res->writeStatus("404 Not Found")
           ->writeHeader("Content-Type", "text/plain")
           ->end("Not Found");
        return;
    }

    // 5. Read file contents into a string.
    std::ostringstream oss;
    oss << file.rdbuf();
    std::string content = oss.str();

    // 6. Determine Content-Type and send the response.
    const auto ct = mimeType(full_path);
    res->writeHeader("Content-Type", ct)
       ->end(std::move(content));
}

// Explicit template instantiation for the HttpResponse types we use.
// uWebSockets uses HttpResponse<false> for non-SSL and HttpResponse<true> for SSL.
template void WebServer::serveStatic(uWS::HttpResponse<false> *, const std::string &);

// ---------------------------------------------------------------------------
// mimeType — determine Content-Type from file extension
//
// Supported types:
//   .html  → text/html
//   .css   → text/css
//   .js    → application/javascript
//   .json  → application/json
//   .svg   → image/svg+xml
//   .png   → image/png
//   .ico   → image/x-icon
//   .woff2 → font/woff2
//   .woff  → font/woff
//   other  → application/octet-stream
// ---------------------------------------------------------------------------

[[nodiscard]] std::string WebServer::mimeType(const std::string &path)
{
    const auto dot = path.rfind('.');
    if (std::string::npos == dot)
    {
        return "application/octet-stream";
    }

    const auto ext = path.substr(dot);

    if (".html" == ext)  { return "text/html"; }
    if (".css" == ext)   { return "text/css"; }
    if (".js" == ext)    { return "application/javascript"; }
    if (".json" == ext)  { return "application/json"; }
    if (".svg" == ext)   { return "image/svg+xml"; }
    if (".png" == ext)   { return "image/png"; }
    if (".ico" == ext)   { return "image/x-icon"; }
    if (".woff2" == ext) { return "font/woff2"; }
    if (".woff" == ext)  { return "font/woff"; }
    if (".map" == ext)   { return "application/json"; }
    if (".txt" == ext)   { return "text/plain"; }

    return "application/octet-stream";
}

} // namespace pulse::webui
