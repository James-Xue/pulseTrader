// paper_account_store.cpp — Crash-safe JSON persistence for the paper ledger.

#include "execution/PaperAccountStore.hpp"

#include "logging/Logger.hpp"

#include <fstream>
#include <string>

#ifndef _WIN32
#include <fcntl.h>
#include <unistd.h>
#endif

namespace pulse::execution
{

namespace
{
constexpr const char *kLogModule = "paper";
} // namespace

PaperAccountStore::PaperAccountStore(std::filesystem::path path,
                                     int snapshot_interval_ms)
    : m_path{ std::move(path) }
    , m_intervalMs{ snapshot_interval_ms }
{
    m_tmp = m_path;
    m_tmp += ".tmp";
    m_bak = m_path;
    m_bak += ".bak";
}

PaperAccountStore::~PaperAccountStore()
{
    stop();
    if (nullptr != m_account)
    {
        saveNow(*m_account);
    }
}

const std::filesystem::path &PaperAccountStore::path() const noexcept
{
    return m_path;
}

PaperStoreLoad PaperAccountStore::load(PaperAccount &account) const
{
    // 1. Try the main file.
    // 2. Fall back to the .bak copy written by the previous rotation.
    // 3. Distinguish "never written" (FirstRun) from "both copies exist but
    //    are unreadable" (Corrupt) so a corrupted ledger is never silently
    //    replaced with a freshly funded account.
    const auto try_read = [](const std::filesystem::path &p,
                             nlohmann::json &out) -> bool {
        std::ifstream in(p);
        if (!in)
        {
            return false;
        }
        try
        {
            in >> out;
        }
        catch (const std::exception &)
        {
            return false;
        }
        return out.is_object() && out.contains("schema_version")
            && out.contains("account");
    };

    nlohmann::json j;
    bool main_exists = std::filesystem::exists(m_path);
    if (try_read(m_path, j) && j.at("schema_version").get<int>() == kSchemaVersion)
    {
        if (account.restore(j.at("account")))
        {
            PULSE_LOG_INFO(kLogModule, "restored paper ledger from {}",
                           m_path.string());
            return PaperStoreLoad::Loaded;
        }
    }

    if (try_read(m_bak, j) && j.at("schema_version").get<int>() == kSchemaVersion)
    {
        if (account.restore(j.at("account")))
        {
            PULSE_LOG_WARN(kLogModule,
                           "main state unreadable; restored from {}",
                           m_bak.string());
            return PaperStoreLoad::Loaded;
        }
    }

    if (!main_exists && !std::filesystem::exists(m_bak))
    {
        return PaperStoreLoad::FirstRun;
    }

    PULSE_LOG_ERROR(kLogModule,
                    "both {} and its backup are unreadable — refusing to "
                    "re-fund the account",
                    m_path.string());
    return PaperStoreLoad::Corrupt;
}

void PaperAccountStore::start(PaperAccount &account)
{
    if (m_thread.joinable())
    {
        return;
    }
    m_account = &account;
    m_thread = std::jthread([this](std::stop_token st) { run(st); });
}

void PaperAccountStore::stop()
{
    if (!m_thread.joinable())
    {
        return;
    }
    m_thread.request_stop();
    m_waitCv.notify_all();
    m_thread.join();
}

void PaperAccountStore::detach()
{
    stop();
    m_account = nullptr;
}

void PaperAccountStore::run(std::stop_token st)
{
    while (!st.stop_requested())
    {
        {
            std::unique_lock<std::mutex> lock(m_waitMutex);
            m_waitCv.wait_for(lock, st,
                              std::chrono::milliseconds(m_intervalMs),
                              [&st] { return st.stop_requested(); });
        }
        if (st.stop_requested())
        {
            break;
        }
        if (nullptr != m_account)
        {
            saveNow(*m_account);
        }
    }
}

void PaperAccountStore::saveNow(const PaperAccount &account)
{
    nlohmann::json payload;
    payload["schema_version"] = kSchemaVersion;
    payload["account"] = account.toJson();

    // Serialise outside the account's lock (toJson copies under it) and keep
    // file replacement serialized separately.
    if (!writeAtomic(payload))
    {
        PULSE_LOG_ERROR(kLogModule, "failed to write paper state to {}",
                        m_path.string());
    }
}

bool PaperAccountStore::writeAtomic(const nlohmann::json &payload)
{
    std::lock_guard<std::mutex> lock(m_writeMutex);

    std::error_code ec;
    if (m_path.has_parent_path())
    {
        std::filesystem::create_directories(m_path.parent_path(), ec);
    }

    // 1. Write the new copy.
    {
        std::ofstream out(m_tmp, std::ios::binary | std::ios::trunc);
        if (!out)
        {
            return false;
        }
        const std::string text = payload.dump(2);
        out.write(text.data(), static_cast<std::streamsize>(text.size()));
        out.flush();
        if (!out)
        {
            return false;
        }
    }

#ifndef _WIN32
    // 2. Best-effort durability before the rename makes the file visible.
    if (const int fd = ::open(m_tmp.c_str(), O_RDONLY); fd >= 0)
    {
        ::fsync(fd);
        ::close(fd);
    }
#endif

    // 3. Rotate the previous good copy to .bak, then publish the new one.
    //    rename-before-remove (never remove the only copy first — that was a
    //    data-loss bug in the UeHao reference implementation).
    if (std::filesystem::exists(m_path, ec))
    {
        std::filesystem::remove(m_bak, ec); // stale backup, safe to drop
        std::filesystem::rename(m_path, m_bak, ec);
        if (ec)
        {
            PULSE_LOG_WARN(kLogModule, "could not rotate {} to {}: {}",
                           m_path.string(), m_bak.string(), ec.message());
        }
    }

    std::filesystem::rename(m_tmp, m_path, ec);
    if (ec)
    {
        // Publish failed — try to put the previous copy back so a crash here
        // does not leave the account without a readable state file.
        std::error_code restore_ec;
        std::filesystem::rename(m_bak, m_path, restore_ec);
        return false;
    }

    return true;
}

} // namespace pulse::execution
