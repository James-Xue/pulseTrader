#!/usr/bin/env bash
# P5 of the 2026-11-27 paper-team reconciliation audit: the no-SQLite job.
#
# The Linux/CI build can run without SQLite (the Windows default). This job
# configures a scratch build tree, compiles every target — including the
# reconciliation contract test, whose SQLite assertions must compile out
# cleanly under -Werror — and runs the full suite.
#
# Usage: scripts/ci/no-sqlite.sh [extra cmake args]
set -euo pipefail
cd "$(dirname "$0")/../.."

BUILD_DIR="${PULSE_NO_SQLITE_BUILD_DIR:-build_no_sqlite}"
cmake -S . -B "$BUILD_DIR" -DPULSE_ENABLE_SQLITE=OFF -DPULSE_USE_FETCHCONTENT=OFF "$@"
cmake --build "$BUILD_DIR" -j"$(nproc)"
ctest --test-dir "$BUILD_DIR" -j1 --output-on-failure
