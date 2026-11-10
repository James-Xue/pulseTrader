#pragma once
// SingleInstanceGuard.hpp — OS-level exclusive-lock guard that refuses a
// second engine instance on the same machine.
//
// Background (2026-08-16 incident): two engine processes ran at once, each
// placing orders independently — the exchange ended up with 6 short
// contracts while each engine only knew about its own 3. The control-socket
// bind alone cannot prevent this: the second process just logs a bind error
// and keeps trading without a control plane.
//
// This guard takes an exclusive lock on a lock file at engine startup:
// flock(LOCK_EX|LOCK_NB) on POSIX, and on Windows a CreateFileW() whose
// dwShareMode is 0 — the Win32 equivalent, because a second open of a
// zero-share file fails with ERROR_SHARING_VIOLATION. A second instance fails
// to acquire it and exits immediately. Both locks are released automatically
// by the kernel when the owning process dies (even a crash/SIGKILL), so there
// is no stale-lock problem.
//
// The guard applies to the `trade` subcommand only — `cli` and `mcp` are
// clients and must be able to run while the engine is up. Set the
// environment variable PULSE_ALLOW_MULTI_INSTANCES=1 to bypass (debugging /
// running a second engine with a different config on purpose).

#include <cstdint>
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
        return m_handle >= 0;
    }

  private:
    /// Handle held while the lock is owned: a POSIX file descriptor or a
    /// Win32 HANDLE. Kept as intptr_t so this header stays free of
    /// <windows.h>; both platforms use -1 as the "not acquired" sentinel
    /// (INVALID_HANDLE_VALUE is defined as -1 too).
    std::intptr_t m_handle = -1;
};

} // namespace pulse
