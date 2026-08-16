#pragma once
// SingleInstanceGuard.hpp — flock-based guard that refuses a second engine
// instance on the same machine.
//
// Background (2026-08-16 incident): two engine processes ran at once, each
// placing orders independently — the exchange ended up with 6 short
// contracts while each engine only knew about its own 3. The control-socket
// bind alone cannot prevent this: the second process just logs a bind error
// and keeps trading without a control plane.
//
// This guard takes an exclusive flock on a lock file at engine startup.
// A second instance fails to acquire it and exits immediately. flock is
// released automatically by the kernel when the owning process dies (even a
// crash/SIGKILL), so there is no stale-lock problem.
//
// The guard applies to the `trade` subcommand only — `cli` and `mcp` are
// clients and must be able to run while the engine is up. Set the
// environment variable PULSE_ALLOW_MULTI_INSTANCES=1 to bypass (debugging /
// running a second engine with a different config on purpose).

#include <string>

namespace pulse
{

class SingleInstanceGuard
{
  public:
    /// Opens (creating parent directories) and exclusively locks
    /// `lock_path`. If another live process holds the lock, acquired()
    /// returns false and the lock file is left untouched.
    explicit SingleInstanceGuard(const std::string &lock_path);

    ~SingleInstanceGuard();

    SingleInstanceGuard(const SingleInstanceGuard &) = delete;
    SingleInstanceGuard &operator=(const SingleInstanceGuard &) = delete;

    /// True if this process holds the exclusive lock.
    [[nodiscard]] bool acquired() const noexcept
    {
        return m_fd >= 0;
    }

  private:
    int m_fd = -1;
};

} // namespace pulse
