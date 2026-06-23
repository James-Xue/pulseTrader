#pragma once
// heartbeat_events.hpp — Event types for the heartbeat scheduler (Layer 5)
//
// Defines the event types that flow through the heartbeat system:
//   1. OnBeat         — Timer fired, triggers a new AI analysis cycle
//   2. OnAnalysisDone — AI cycle completed, carries the AnalysisResult
//   3. OnParamUpdate  — A strategy parameter was updated by ParamAdvisor
//
// These events are used for observability and optional downstream hooks.
// The TaskQueue processes callable tasks (not events directly); events
// are informational wrappers used in logging and callbacks.

#include "ai/AnalysisResult.hpp"

#include <cstdint>
#include <string>
#include <variant>

namespace pulse::heartbeat
{

// ---------------------------------------------------------------------------
// OnBeat — Timer fired, trigger a new AI analysis cycle
// ---------------------------------------------------------------------------
struct OnBeat
{
    /// Monotonically increasing beat counter for logging.
    std::uint64_t beat_number = 0;
};

// ---------------------------------------------------------------------------
// OnAnalysisDone — AI analysis cycle completed
//
// Carries the AnalysisResult so downstream consumers (logging, WebUI)
// can observe what the AI concluded without polling ParamAdvisor.
// ---------------------------------------------------------------------------
struct OnAnalysisDone
{
    ai::AnalysisResult result; ///< The completed analysis result.
    bool params_updated = false; ///< True if ParamAdvisor applied any deltas.
};

// ---------------------------------------------------------------------------
// OnParamUpdate — A strategy parameter was updated by ParamAdvisor
//
// Emitted for each individual parameter that was changed. Useful for
// WebUI real-time parameter display and audit logging.
// ---------------------------------------------------------------------------
struct OnParamUpdate
{
    std::string param_name; ///< Name of the parameter (e.g. "ema_fast_period").
    double old_value = 0.0; ///< Value before the update.
    double new_value = 0.0; ///< Value after the update.
};

// ---------------------------------------------------------------------------
// TaskPriority — Priority level for tasks in the TaskQueue
// ---------------------------------------------------------------------------
enum class TaskPriority : std::uint8_t
{
    Normal = 0, ///< Default priority for AI analysis tasks.
    High = 1,   ///< High priority for urgent tasks (e.g. manual trigger).
};

// ---------------------------------------------------------------------------
// HeartbeatEvent — Variant type for all heartbeat events
// ---------------------------------------------------------------------------
using HeartbeatEvent = std::variant<OnBeat, OnAnalysisDone, OnParamUpdate>;

} // namespace pulse::heartbeat
