// gate_ws_channels.cpp — Gate.io WebSocket channel subscription registry (Layer 1 Exchange)
//
// Implementation of channel management, frame dispatch, and message builders.
// All public methods are thread-safe via std::shared_mutex.

#include "exchange/gate_ws_channels.hpp"

#include "logging/logger.hpp"

#include <chrono>

namespace pulse::exchange
{

// ---------------------------------------------------------------------------
// subscribe
// ---------------------------------------------------------------------------
void GateWsChannels::subscribe(const std::string &channel,
    const std::vector<std::string> &payload,
    ChannelCallback callback)
{
    // 1. Acquire exclusive write lock
    // 2. Insert or replace the channel entry
    std::unique_lock lock(mutex_);
    channels_[channel] = ChannelEntry{ payload, std::move(callback) };
    PULSE_LOG_DEBUG("exchange", "Channel subscribed: {}", channel);
}

// ---------------------------------------------------------------------------
// unsubscribe
// ---------------------------------------------------------------------------
void GateWsChannels::unsubscribe(const std::string &channel)
{
    // 1. Acquire exclusive write lock
    // 2. Erase the channel if it exists
    std::unique_lock lock(mutex_);
    const auto erased = channels_.erase(channel);
    if (0 != erased)
    {
        PULSE_LOG_DEBUG("exchange", "Channel unsubscribed: {}", channel);
    }
}

// ---------------------------------------------------------------------------
// dispatch
// ---------------------------------------------------------------------------
bool GateWsChannels::dispatch(const nlohmann::json &frame)
{
    // 1. Extract the "channel" field — required for routing
    // 2. Look up the matching subscription under shared read lock
    // 3. Invoke the callback with frame["result"] and the full frame
    if (!frame.contains("channel"))
    {
        PULSE_LOG_WARN("exchange", "WS frame missing 'channel' field");
        return false;
    }

    const auto channel_name = frame["channel"].get<std::string>();

    std::shared_lock lock(mutex_);
    const auto it = channels_.find(channel_name);
    if (channels_.end() == it)
    {
        return false;
    }

    // Extract the "result" field — may be absent (null)
    const auto &result = frame.contains("result") ? frame["result"] : nlohmann::json();
    it->second.callback(result, frame);
    return true;
}

// ---------------------------------------------------------------------------
// build_subscribe_msg
// ---------------------------------------------------------------------------
nlohmann::json GateWsChannels::build_subscribe_msg(const std::string &channel,
    const std::vector<std::string> &payload) const
{
    // Gate.io v4 subscribe format:
    //   {"time": <unix_seconds>, "channel": "<name>", "event": "subscribe", "payload": [...]}
    const auto ts =
        std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    return nlohmann::json{ { "time", ts }, { "channel", channel }, { "event", "subscribe" }, { "payload", payload } };
}

// ---------------------------------------------------------------------------
// build_unsubscribe_msg
// ---------------------------------------------------------------------------
nlohmann::json GateWsChannels::build_unsubscribe_msg(const std::string &channel,
    const std::vector<std::string> &payload) const
{
    // Gate.io v4 unsubscribe format:
    //   {"time": <unix_seconds>, "channel": "<name>", "event": "unsubscribe", "payload": [...]}
    const auto ts =
        std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    return nlohmann::json{ { "time", ts }, { "channel", channel }, { "event", "unsubscribe" }, { "payload", payload } };
}

// ---------------------------------------------------------------------------
// build_pong
// ---------------------------------------------------------------------------
nlohmann::json GateWsChannels::build_pong(const nlohmann::json &ping_frame)
{
    // Gate.io sends: {"time": <int>, "channel": "spot.ping"}
    // Client replies: {"time": <same_int>, "channel": "spot.pong"}
    const auto time_val = ping_frame.value("time", 0);
    return nlohmann::json{ { "time", time_val }, { "channel", "spot.pong" } };
}

// ---------------------------------------------------------------------------
// active_channels
// ---------------------------------------------------------------------------
std::vector<std::string> GateWsChannels::active_channels() const
{
    // 1. Acquire shared read lock
    // 2. Collect all channel names into a vector
    std::shared_lock lock(mutex_);
    std::vector<std::string> result;
    result.reserve(channels_.size());
    for (const auto &[name, entry] : channels_)
    {
        result.push_back(name);
    }
    return result;
}

// ---------------------------------------------------------------------------
// get_payload
// ---------------------------------------------------------------------------
std::vector<std::string> GateWsChannels::get_payload(const std::string &channel) const
{
    // 1. Acquire shared read lock
    // 2. Return the payload if the channel exists, empty vector otherwise
    std::shared_lock lock(mutex_);
    const auto it = channels_.find(channel);
    if (channels_.end() == it)
    {
        return {};
    }
    return it->second.payload;
}

} // namespace pulse::exchange
