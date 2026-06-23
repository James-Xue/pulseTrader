// wsServer.cpp — WebSocket push server implementation (Layer 9 WebUI)
//
// Implements client tracking and snapshot broadcasting for the WebUI dashboard:
//   1. pushSnapshot() serializes DashboardSnapshot to JSON (cached for reuse)
//   2. Client count is tracked atomically for the max-clients limit
//   3. Broadcasting uses a pluggable publish function (set by WebServer) that
//      defers to the uWebSockets event loop thread via Loop::defer()
//
// The actual WebSocket route registration (open/close handlers) is done by
// WebServer, which calls onWsOpen() and onWsClose() from the handlers.

#include "webui/WsServer.hpp"

#include <nlohmann/json.hpp>

#include <utility>

namespace pulse::webui
{

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

WsServer::WsServer(const WebUiConfig &config)
    : m_config{ config }
{
}

WsServer::~WsServer() = default;

// ---------------------------------------------------------------------------
// setPublishFn — store the publish function for deferred broadcast
// ---------------------------------------------------------------------------

void WsServer::setPublishFn(PublishFn fn)
{
    m_publishFn = std::move(fn);
}

// ---------------------------------------------------------------------------
// pushSnapshot — serialize and broadcast to all connected clients
//
// Algorithm:
//   1. Serialize the DashboardSnapshot to JSON via nlohmann ADL
//   2. Cache the JSON string in m_lastJson (for new client on-connect delivery)
//   3. Snapshot the current client count
//   4. If the publish function is set and clients are connected, invoke it
//
// Thread safety:
//   - JSON serialization is on the caller's thread (DashboardState poll thread)
//   - The publish function wraps Loop::defer() + app->publish(), which is
//     thread-safe (defer enqueues under a mutex and wakes the loop)
//   - m_lastJson is protected by m_jsonMutex for concurrent reads
// ---------------------------------------------------------------------------

void WsServer::pushSnapshot(std::shared_ptr<const DashboardSnapshot> snapshot)
{
    // 1. Serialize to JSON and cache.
    nlohmann::json j = *snapshot;
    std::string json_str = j.dump();

    {
        std::lock_guard lock{ m_jsonMutex };
        m_lastJson = json_str;
    }

    // 2. Check if we have clients and a valid publish function.
    const std::size_t count = m_clientCount.load(std::memory_order_acquire);
    if (0 == count || !m_publishFn)
    {
        return;
    }

    // 3. Invoke the publish function (defers to event loop thread).
    m_publishFn(json_str);
}

// ---------------------------------------------------------------------------
// onWsOpen — track a new WebSocket client
// ---------------------------------------------------------------------------

void WsServer::onWsOpen()
{
    m_clientCount.fetch_add(1, std::memory_order_acq_rel);
}

// ---------------------------------------------------------------------------
// onWsClose — untrack a closing WebSocket client
// ---------------------------------------------------------------------------

void WsServer::onWsClose()
{
    // Guard against underflow (should not happen, but defensive).
    const std::size_t prev = m_clientCount.load(std::memory_order_acquire);
    if (0 < prev)
    {
        m_clientCount.fetch_sub(1, std::memory_order_acq_rel);
    }
}

// ---------------------------------------------------------------------------
// Getters
// ---------------------------------------------------------------------------

[[nodiscard]] std::size_t WsServer::clientCount() const
{
    return m_clientCount.load(std::memory_order_acquire);
}

[[nodiscard]] std::size_t WsServer::maxClients() const
{
    return static_cast<std::size_t>(m_config.maxClients);
}

[[nodiscard]] std::string WsServer::lastJson() const
{
    std::lock_guard lock{ m_jsonMutex };
    return m_lastJson;
}

} // namespace pulse::webui
