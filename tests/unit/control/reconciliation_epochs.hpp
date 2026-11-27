#pragma once
// reconciliation_epochs.hpp — frozen epoch contract for the 2026-11-27 paper-team reconciliation audit.
//
// Why this header exists:
// The audit (docs/audits/paper-team-reconciliation-2026-11-27.md §2) fixes four
// capital epochs, E0..E3, and the per-epoch trade sums that any fix must reproduce.
// Those boundaries were originally recovered by hand from log timestamps and from
// data/paper-state.json.bak-* files, which means every future test that re-derives
// them can drift in a different direction. This header is the single place where
// the boundaries live, so a test never re-types a magic timestamp.
//
// Why a fixed UTC+8 offset instead of a tz-database lookup:
// 1. Asia/Shanghai has had no daylight saving since 1991, so +08:00 is exact for
//    every boundary below.
// 2. The Windows/MSVC build deliberately does not ship IANA tzdata (one test is
//    skipped there for exactly that reason), so a tzdb-based helper would fail on
//    a supported platform.
// 3. A pure civil-date conversion also keeps this header usable in `constexpr`
//    contexts.
//
// Units: every timestamp here is UNIX nanoseconds. That matches `trades.timestamp`
// in SQLite and `fills.fill_time_ns` in the GT ledger. JSON `time_ms` fields are
// milliseconds and are converted at the call site, never here.

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace pulse::test::reconciliation
{

/// Asia/Shanghai is UTC+8 with no DST (see the rationale above).
inline constexpr int kCstUtcOffsetHours = 8;

/// Days since 1970-01-01 for a proleptic Gregorian civil date.
///
/// This is Howard Hinnant's `days_from_civil` algorithm, inlined so the header has
/// no dependency on std::chrono's calendar support (which older libstdc++ builds
/// in this project's matrix do not fully provide).
///
/// Steps:
/// 1. Shift the year so March is month 0 — this puts the leap day at the end of
///    the shifted year and removes one special case.
/// 2. Split into 400-year Gregorian eras; within an era the leap rule collapses to
///    integer divisions.
/// 3. Re-base the era/day count onto the UNIX epoch (1970-01-01 is day 719468).
constexpr std::int64_t daysFromCivil(int year, int month, int day) noexcept
{
    year -= (month <= 2) ? 1 : 0;
    const int era = (year >= 0 ? year : year - 399) / 400;
    const unsigned yearOfEra = static_cast<unsigned>(year - era * 400);                                  // [0, 399]
    const unsigned dayOfYear = static_cast<unsigned>((153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1); // [0, 365]
    const unsigned dayOfEra = yearOfEra * 365 + yearOfEra / 4 - yearOfEra / 100 + dayOfYear;             // [0, 146096]
    return static_cast<std::int64_t>(era) * 146097 + static_cast<std::int64_t>(dayOfEra) - 719468;
}

/// Convert an Asia/Shanghai wall-clock instant to UNIX nanoseconds.
constexpr std::int64_t cstToUnixNs(int year, int month, int day, int hour, int minute, int second) noexcept
{
    const std::int64_t days = daysFromCivil(year, month, day);
    const std::int64_t secondsSinceEpoch =
        days * 86400 + hour * 3600 + minute * 60 + second - kCstUtcOffsetHours * 3600;
    return secondsSinceEpoch * 1'000'000'000LL;
}

/// One capital epoch as defined by audit §2.
///
/// `endInclusive` is true only for E3: its end is the 12:53:00 CST snapshot, and the
/// evidence includes a trade at 12:51:05, so the window must stay closed at the
/// snapshot instant rather than hand the last row to a nonexistent E4.
struct Epoch
{
    std::string_view id;
    std::int64_t startNs;  ///< Inclusive start boundary (CST wall clock).
    std::int64_t endNs;    ///< End boundary; inclusive only when `endInclusive`.
    bool endInclusive;
    std::int64_t firstTradeId; ///< Lowest `trades.id` observed in the epoch.
    std::int64_t lastTradeId;  ///< Highest `trades.id` observed in the epoch.
    int expectedRows;
    double expectedPnl;
    double expectedFees;
    double seedUsdt; ///< CFD-leg seed in force during this epoch (spot/futures stay untraded).
};

/// The four epochs, in order. Boundaries are CST wall-clock instants; ids and sums
/// are reproduced from the read-only audit snapshot (2026-11-27 12:53 CST).
///
/// E1's first id is 65, not 64: id 64 is absent from the table (an INSERT lost to a
/// duplicate order_id), and the audit records that hole explicitly.
inline constexpr std::array<Epoch, 4> kEpochs{{
    {"E0", cstToUnixNs(2026, 11, 25, 15, 32, 15), cstToUnixNs(2026, 11, 26, 10, 30, 0), false, 1, 63, 63,
     -26.88, 1.86, 10000.0},
    {"E1", cstToUnixNs(2026, 11, 26, 10, 30, 0), cstToUnixNs(2026, 11, 27, 9, 23, 0), false, 65, 194, 130,
     -64.48, 3.90, 100.0},
    {"E2", cstToUnixNs(2026, 11, 27, 9, 23, 0), cstToUnixNs(2026, 11, 27, 9, 46, 0), false, 195, 196, 2,
     -0.16, 0.00, 100.0},
    {"E3", cstToUnixNs(2026, 11, 27, 9, 46, 0), cstToUnixNs(2026, 11, 27, 12, 53, 0), true, 197, 211, 15,
     -7.70, 0.42, 100.0},
}};

/// End-of-epoch account snapshot and the seed it must reconcile against.
///
/// `file` is relative to the fixture directory (tests/fixtures/reconciliation). The
/// identity every snapshot must satisfy is:
///     cash + locked_margin == seed + realized_pnl - fees_paid
struct SnapshotExpectation
{
    std::string_view epochId;
    std::string_view file;
    double seedUsdt;
    double cashUsdt;
    double lockedMarginUsdt;
    double realizedPnlUsdt;
    double feesPaidUsdt;
};

/// Four file-backed snapshots. The 13:39-13:49 "live" observation in audit §2 is
/// deliberately absent: it was never captured as a file, and this fixture set only
/// freezes evidence that can be re-read byte-for-byte.
inline constexpr std::array<SnapshotExpectation, 4> kSnapshots{{
    {"E0", "paper-state/paper-state.json.bak-20261126-100usd", 10000.0, 9965.35, 0.0, -32.79, 1.86},
    {"E1", "paper-state/paper-state.json.bak-20261127-pre-topup", 100.0, 21.01, 0.0, -74.55, 4.44},
    {"E2", "paper-state/paper-state.json.bak-20261127-pre-final-reset", 100.0, 99.78, 0.0, -0.16, 0.06},
    {"E3", "paper-state/paper-state.json", 100.0, 88.2425, 3.6375, -7.70, 0.42},
}};

/// Classify a UNIX-nanosecond timestamp into an epoch index, or -1 when it falls
/// outside every frozen window. Boundaries are half-open except E3's end.
constexpr int classifyEpoch(std::int64_t timestampNs) noexcept
{
    for (std::size_t index = 0; index < kEpochs.size(); ++index)
    {
        const Epoch &epoch = kEpochs[index];
        const bool afterStart = (epoch.startNs <= timestampNs);
        const bool beforeEnd = epoch.endInclusive ? (timestampNs <= epoch.endNs) : (timestampNs < epoch.endNs);
        if (afterStart && beforeEnd)
        {
            return static_cast<int>(index);
        }
    }
    return -1;
}

/// Tolerance used for every monetary comparison in the P0 contract. The audit's
/// acceptance rule is 0.005 USDT; tests that need exact evidence equality use a
/// tighter value locally.
inline constexpr double kAuditMoneyTolerance = 0.005;

} // namespace pulse::test::reconciliation
