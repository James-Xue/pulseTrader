// ws_server.cpp — WebSocket push server implementation (Layer 9 WebUI)
//
// Implements client tracking and snapshot broadcasting for the WebUI dashboard:
//   1. push_snapshot() serializes DashboardSnapshot to JSON (cached for reuse)
//   2. Client count is tracked atomically for the max-clients limit
//   3. Broadcasting uses a pluggable publish function (set by WebServer) that
//      defers to the uWebSockets event loop thread via Loop::defer()
//
// The actual WebSocket route registration (open/close handlers) is done by
// WebServer, which calls on_ws_open() and on_ws_close() from the handlers.

#include "webui/ws_server.hpp"

#include <nlohmann/json.hpp>

#include <utility>

namespace pulse::webui
{

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

WsServer::WsServer(const WebUiConfig &config)
    : config_{ config }
{
}

WsServer::~WsServer() = default;

// ---------------------------------------------------------------------------
// set_publish_fn — store the publish function for deferred broadcast
// ---------------------------------------------------------------------------

void WsServer::set_publish_fn(PublishFn fn)
{
    publish_fn_ = std::move(fn);
}

// ---------------------------------------------------------------------------
// push_snapshot — serialize and broadcast to all connected clients
//
// Algorithm:
//   1. Serialize the DashboardSnapshot to JSON via nlohmann ADL
//   2. Cache the JSON string in last_json_ (for new client on-connect delivery)
//   3. Snapshot the current client count
//   4. If the publish function is set and clients are connected, invoke it
//
// Thread safety:
//   - JSON serialization is on the caller's thread (DashboardState poll thread)
//   - The publish function wraps Loop::defer() + app->publish(), which is
//     thread-safe (defer enqueues under a mutex and wakes the loop)
//   - last_json_ is protected by json_mutex_ for concurrent reads
// ---------------------------------------------------------------------------

void WsServer::push_snapshot(std::shared_ptr<const DashboardSnapshot> snapshot)
{
    // 1. Serialize to JSON and cache.
    nlohmann::json j = *snapshot;
    std::string json_str = j.dump();

    {
        std::lock_guard lock{ json_mutex_ };
        last_json_ = json_str;
    }

    // 2. Check if we have clients and a valid publish function.
    const std::size_t count = client_count_.load(std::memory_order_acquire);
    if (0 == count || !publish_fn_)
    {
        return;
    }

    // 3. Invoke the publish function (defers to event loop thread).
    publish_fn_(json_str);
}

// ---------------------------------------------------------------------------
// on_ws_open — track a new WebSocket client
// ---------------------------------------------------------------------------

void WsServer::on_ws_open()
{
    client_count_.fetch_add(1, std::memory_order_acq_rel);
}

// ---------------------------------------------------------------------------
// on_ws_close — untrack a closing WebSocket client
// ---------------------------------------------------------------------------

void WsServer::on_ws_close()
{
    // Guard against underflow (should not happen, but defensive).
    const std::size_t prev = client_count_.load(std::memory_order_acquire);
    if (0 < prev)
    {
        client_count_.fetch_sub(1, std::memory_order_acq_rel);
    }
}

// ---------------------------------------------------------------------------
// Getters
// ---------------------------------------------------------------------------

[[nodiscard]] std::size_t WsServer::client_count() const
{
    return client_count_.load(std::memory_order_acquire);
}

[[nodiscard]] std::size_t WsServer::max_clients() const
{
    return static_cast<std::size_t>(config_.maxClients);
}

[[nodiscard]] std::string WsServer::last_json() const
{
    std::lock_guard lock{ json_mutex_ };
    return last_json_;
}

} // namespace pulse::webui
