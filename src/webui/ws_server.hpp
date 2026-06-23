#pragma once
// wsServer.hpp — WebSocket push server for WebUI real-time snapshots (Layer 9 WebUI)
//
// Provides push-based snapshot delivery to connected WebSocket clients:
//   1. DashboardState invokes the snapshot callback after each poll cycle
//   2. WebServer forwards the snapshot to WsServer::pushSnapshot()
//   3. WsServer serializes the snapshot to JSON (cached for reuse) and invokes
//      a publish function (set by WebServer) that broadcasts via pub/sub
//
// Thread model:
//   - pushSnapshot() may be called from any thread (typically the poll thread)
//   - JSON serialization happens on the caller's thread (cheap, ~50 us)
//   - The actual publish is deferred to the uWebSockets event loop thread
//     via a publish function that calls Loop::defer() internally
//   - Client add/remove is called from the event loop thread (WS open/close)
//
// Client limit:
//   - Enforced by WebServer's upgrade handler before the WS connection is accepted
//   - WsServer tracks the count for informational purposes
//
// PerSocketData:
//   - uWebSockets requires a per-socket data struct as a template parameter
//   - Currently empty; reserved for future subscription filtering

#include "core/config.hpp"
#include "webui/snapshot_types.hpp"

#include <atomic>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

namespace pulse::webui
{

// ---------------------------------------------------------------------------
// PerSocketData — per-WebSocket user data (template parameter for uWS::ws)
//
// Currently empty. Future extensions could include:
//   1. Per-client subscription filters (e.g. only certain symbols)
//   2. Per-client backpressure tracking
//   3. Connection metadata (IP, connect time)
// ---------------------------------------------------------------------------
struct PerSocketData
{
    // Reserved for future use.
};

// ---------------------------------------------------------------------------
// WsServer — WebSocket push server helper
//
// Manages connected client tracking and snapshot broadcasting. The actual
// uWS::App and route registration are owned by WebServer; WsServer is a
// helper that provides:
//   1. Thread-safe client add/remove (called from WS open/close handlers)
//   2. JSON serialization with caching (serialize once, send to all)
//   3. A pluggable publish function (set by WebServer) for deferred broadcast
//
// Usage:
//   WsServer ws(config);
//   ws.setPublishFn([](const std::string &json) {
//       loop->defer([&app, json]() { app->publish("snapshot", json, TEXT); });
//   });
//   ws.pushSnapshot(snapshot);  // Called from DashboardState callback
// ---------------------------------------------------------------------------
class WsServer
{
  public:
    /// Publish function signature: receives serialized JSON, broadcasts to clients.
    ///
    /// Must be thread-safe — typically wraps Loop::defer() + app->publish().
    using PublishFn = std::function<void(const std::string &json)>;

    /// Construct with WebUI configuration (reads maxClients).
    explicit WsServer(const WebUiConfig &config);

    /// Destructor.
    ~WsServer();

    // Non-copyable, non-movable (holds mutex and atomic state).
    WsServer(const WsServer &) = delete;
    WsServer &operator=(const WsServer &) = delete;
    WsServer(WsServer &&) = delete;
    WsServer &operator=(WsServer &&) = delete;

    /// Set the publish function used to broadcast JSON to connected clients.
    ///
    /// Must be set before pushSnapshot() is called. WebServer sets this
    /// during start() with a lambda that captures the uWS::App pointer and
    /// defers the publish to the event loop thread.
    void setPublishFn(PublishFn fn);

    /// Push a snapshot to all connected WebSocket clients.
    ///
    /// Algorithm:
    ///   1. Serialize the snapshot to JSON (cached in m_lastJson)
    ///   2. Snapshot the current client count
    ///   3. If m_publishFn is set and clients > 0, invoke the publish function
    ///      which defers the actual broadcast to the event loop thread
    ///
    /// Thread-safe: may be called from any thread.
    void pushSnapshot(std::shared_ptr<const DashboardSnapshot> snapshot);

    /// Called when a new WebSocket connection is opened.
    ///
    /// Increments the client counter. Must be called from the event loop thread.
    void onWsOpen();

    /// Called when a WebSocket connection is closed.
    ///
    /// Decrements the client counter. Must be called from the event loop thread.
    void onWsClose();

    /// Returns the current number of connected clients.
    ///
    /// Lock-free: reads an atomic counter.
    [[nodiscard]] std::size_t clientCount() const;

    /// Returns the maximum number of concurrent clients from config.
    [[nodiscard]] std::size_t maxClients() const;

    /// Returns the most recently serialized JSON snapshot.
    ///
    /// Thread-safe: reads a mutex-protected string.
    /// Returns empty string if no snapshot has been pushed yet.
    [[nodiscard]] std::string lastJson() const;

  private:
    // --- Configuration ---
    WebUiConfig m_config;

    // --- Client tracking ---
    std::atomic<std::size_t> m_clientCount{ 0 };

    // --- Cached JSON (serialized once per push, reused for all clients) ---
    mutable std::mutex m_jsonMutex;
    std::string m_lastJson;

    // --- Publish function (set by WebServer, thread-safe) ---
    PublishFn m_publishFn;
};

} // namespace pulse::webui
