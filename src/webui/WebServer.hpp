#pragma once
// web_server.hpp — HTTP + WebSocket server for WebUI dashboard (Layer 9 WebUI)
//
// Serves the single-page frontend and REST API, and delegates WebSocket
// upgrades to WsServer for real-time snapshot push.
//
// Routes:
//   1. GET /api/status    — Server health check (JSON: status, uptime, version)
//   2. GET /api/snapshot  — Latest DashboardSnapshot as JSON
//   3. GET /ws            — WebSocket upgrade (delegated to WsServer)
//   4. GET /              — Serves index.html from frontend_dir
//   5. GET /*             — Serves static files from frontend_dir
//
// Security:
//   - Bearer token auth on /api/* and /ws endpoints (skipped if authToken is empty)
//   - Host header validation to prevent DNS rebinding attacks
//   - Path traversal prevention on static file serving
//
// Architecture:
//   - PIMPL pattern hides uWebSockets headers from public interface
//   - uWS::App is constructed and run on a dedicated background thread
//   - WsServer is owned internally as a helper for client tracking + broadcast
//   - DashboardState snapshot callback is wired to WsServer::pushSnapshot()
//
// Thread model:
//   - start() is non-blocking; spawns a background thread for the event loop
//   - stop() closes the listen socket and joins the background thread
//   - API handlers run on the event loop thread
//   - pushSnapshot() runs on the DashboardState poll thread; defers to loop

#include "core/config.hpp"
#include "webui/DashboardState.hpp"
#include "webui/WsServer.hpp"

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace pulse::webui
{

// ---------------------------------------------------------------------------
// WebServer — HTTP + WebSocket server with PIMPL
//
// Usage:
//   WebServer server(config, dashboard_state, "frontend");
//   if (server.start()) {
//       // Server is running on a background thread
//       // ... application runs ...
//       server.stop();
//   }
// ---------------------------------------------------------------------------
class WebServer
{
  public:
    /// Construct the server with configuration, dashboard state, and frontend path.
    ///
    /// Does not start listening — call start() explicitly.
    ///
    /// Parameters:
    ///   1. config       — WebUI configuration (bind address, port, auth token, etc.)
    ///   2. state        — DashboardState providing snapshots for API + WebSocket
    ///   3. frontend_dir — Path to the directory containing static frontend files
    WebServer(const WebUiConfig &config,
              DashboardState &state,
              const std::string &frontend_dir = "frontend/dist");

    /// Destructor — stops the server if still running.
    ~WebServer();

    // Non-copyable, non-movable (owns background thread and uWS::App via PIMPL).
    WebServer(const WebServer &) = delete;
    WebServer &operator=(const WebServer &) = delete;
    WebServer(WebServer &&) = delete;
    WebServer &operator=(WebServer &&) = delete;

    /// Start the HTTP server on a background thread (non-blocking).
    ///
    /// Sequence:
    ///   1. Spawn a background thread
    ///   2. Construct uWS::App on that thread (Loop::get() is thread-local)
    ///   3. Register API routes, WebSocket handler, and static file catch-all
    ///   4. Bind to bindAddress:port
    ///   5. Wire DashboardState snapshot callback to WsServer::pushSnapshot()
    ///   6. Run the event loop (blocks the background thread)
    ///
    /// Returns true if the listen socket was successfully bound.
    [[nodiscard]] bool start();

    /// Stop the server and join the background thread (blocking).
    ///
    /// Sequence:
    ///   1. Close the listen socket via us_listen_socket_close()
    ///   2. This causes app.run() to return on the background thread
    ///   3. Join the background thread
    ///   4. Clean up Impl (destroys uWS::App)
    ///
    /// Safe to call multiple times or when not running.
    void stop();

    /// Returns true if the server is currently listening.
    ///
    /// Lock-free: reads an atomic flag.
    [[nodiscard]] bool running() const;

    /// Returns the actual listen port (useful when port 0 was requested).
    ///
    /// Returns 0 if the server is not running.
    [[nodiscard]] std::uint16_t port() const;

    /// Returns a const reference to the internal WsServer helper.
    ///
    /// Primarily for testing and diagnostics.
    [[nodiscard]] const WsServer &wsServer() const;

  private:
    /// Validate the Authorization header against config.authToken.
    ///
    /// Rules:
    ///   1. If authToken is empty (dev mode), all requests pass
    ///   2. Otherwise, header must be "Bearer <authToken>" (case-sensitive)
    [[nodiscard]] bool validateAuth(std::string_view auth_header) const;

    /// Validate the Host header to prevent DNS rebinding attacks.
    ///
    /// Accepts:
    ///   1. "bindAddress:port" (e.g. "127.0.0.1:8080")
    ///   2. "localhost:port"
    ///   3. "127.0.0.1:port"
    ///
    /// Returns false for empty or unrecognized host headers.
    [[nodiscard]] bool validateHost(std::string_view host_header) const;

    /// Serve a static file from m_frontendDir.
    ///
    /// Security:
    ///   1. Rejects paths containing ".." (directory traversal)
    ///   2. Maps "/" to "/index.html"
    ///   3. Returns 404 for missing files
    ///   4. Sets appropriate Content-Type from file extension
    ///
    /// The template parameter matches uWebSockets' HttpResponse<SSL> type.
    template <typename Res>
    void serveStatic(Res *res, const std::string &path);

    /// Determine the MIME type from a file path extension.
    ///
    /// Supported types: html, css, js, json, svg, png, ico.
    /// Returns "application/octet-stream" for unknown extensions.
    [[nodiscard]] static std::string mimeType(const std::string &path);

    // --- Configuration ---
    WebUiConfig m_config;

    // --- Dashboard state reference ---
    DashboardState &m_state;

    // --- Frontend directory path ---
    std::string m_frontendDir;

    // --- Runtime state ---
    bool m_running{ false };
    std::uint16_t m_actualPort{ 0 };
    std::chrono::steady_clock::time_point m_startTime;

    // --- Internal WsServer helper ---
    std::unique_ptr<WsServer> m_wsServer;

    // --- PIMPL: hides uWS::App, listen socket, and thread ---
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace pulse::webui
