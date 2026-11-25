#pragma once
// PaperAccountStore.hpp — Crash-safe JSON persistence for the paper ledger.
//
// Why not SQLite: paper mode must run on builds without PULSE_ENABLE_SQLITE
// (the Windows default), and the ledger is small (cash + a handful of
// positions). The store writes a versioned JSON snapshot with tmp + fsync +
// rename, keeping the previous good copy as `.bak`.
//
// Failure policy is intentionally FAIL-CLOSED: if both the main file and the
// backup are unreadable, load() reports Corrupt and the caller refuses to
// start. Silently re-funding an account would erase the evidence of whatever
// corrupted it.
//
// Thread safety: start() runs one writer thread; saveNow() may be called from
// the control socket for an explicit flush. Both serialize under the same
// mutex, and the writer deep-copies the ledger before serialising so the
// account's own lock is never held during disk I/O.

#include "execution/PaperAccount.hpp"

#include <nlohmann/json.hpp>

#include <condition_variable>
#include <filesystem>
#include <mutex>
#include <thread>

namespace pulse::execution
{

/// Result of the startup load.
enum class PaperStoreLoad
{
    FirstRun, ///< No state file yet — seed a fresh account.
    Loaded,   ///< Restored from the main file or its backup.
    Corrupt,  ///< Both copies unreadable — the caller must abort.
};

class PaperAccountStore
{
  public:
    /// Parameters:
    ///   1. path                — state file (e.g. data/paper-state.json)
    ///   2. snapshot_interval_ms — writer cadence; clamped by the validator
    PaperAccountStore(std::filesystem::path path, int snapshot_interval_ms);

    /// Stops the writer thread and performs one final save.
    ~PaperAccountStore();

    PaperAccountStore(const PaperAccountStore &) = delete;
    PaperAccountStore &operator=(const PaperAccountStore &) = delete;

    /// Read the state file (or its backup) into `account`.
    [[nodiscard]] PaperStoreLoad load(PaperAccount &account) const;

    /// Start the periodic snapshot writer. The account must outlive the store.
    void start(PaperAccount &account);

    /// Stop the writer (idempotent).
    void stop();

    /// Stop the writer and forget the account, so the destructor performs no
    /// further save. Call after the shutdown-time saveNow() and before the
    /// logger is shut down (a destructor save would log into a dead logger).
    void detach();

    /// Write one snapshot immediately (also used by the shutdown path).
    void saveNow(const PaperAccount &account);

    [[nodiscard]] const std::filesystem::path &path() const noexcept;

    /// Schema version written into every file. Bump with a migration.
    static constexpr int kSchemaVersion = 1;

  private:
    /// Writer loop: waits `interval_ms` or until stop is requested.
    void run(std::stop_token st);

    /// tmp -> fsync -> rotate(path, bak) -> rename(tmp, path).
    [[nodiscard]] bool writeAtomic(const nlohmann::json &payload);

    std::filesystem::path m_path;
    std::filesystem::path m_tmp;
    std::filesystem::path m_bak;
    int m_intervalMs;

    std::mutex m_waitMutex;
    std::condition_variable_any m_waitCv;
    std::jthread m_thread;
    PaperAccount *m_account{ nullptr };
    std::mutex m_writeMutex; ///< Serializes file replacement.
};

} // namespace pulse::execution
