// SingleInstanceGuard.cpp — flock-based single-instance enforcement.
// See SingleInstanceGuard.hpp for the design rationale.

#include "core/SingleInstanceGuard.hpp"

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

#include <filesystem>
#include <system_error>

namespace pulse
{
namespace
{

#ifdef _WIN32
/// Convert a UTF-8 lock path to a wide string for CreateFileW. The default
/// narrow-to-wide conversion interprets bytes as the ANSI code page, which
/// corrupts non-ASCII paths — convert explicitly instead.
std::wstring toWidePath(const std::string &path)
{
    if (path.empty())
    {
        return {};
    }
    const int len = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
    std::wstring wide(static_cast<std::size_t>(len), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, wide.data(), len);
    return wide;
}
#endif

} // namespace

SingleInstanceGuard::SingleInstanceGuard(const std::string &lock_path)
{
    // Create the parent directory (e.g. data/) if it does not exist yet.
    std::error_code ec;
    const auto parent = std::filesystem::path(lock_path).parent_path();
    if (!parent.empty())
    {
        std::filesystem::create_directories(parent, ec);
    }

#ifdef _WIN32
    // Open (creating if needed) the lock file with full sharing so the second
    // instance can open the same file and still fail on the lock, mirroring
    // the POSIX flock semantics where open() succeeds but flock() fails.
    // FILE_SHARE_DELETE additionally mirrors POSIX unlink semantics: removing
    // the lock file stays legal while the handle is open.
    HANDLE h = CreateFileW(toWidePath(lock_path).c_str(),
                           GENERIC_READ | GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr,
                           OPEN_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL,
                           nullptr);
    if (INVALID_HANDLE_VALUE == h)
    {
        // Cannot open the lock file (permissions etc.) — fail closed:
        // acquired() == false → the engine refuses to start.
        return;
    }

    // LOCKFILE_FAIL_IMMEDIATELY: do not block waiting for the other instance
    // to finish. LockFileEx locks are released by the OS when the owning
    // process dies (even a crash), so there is no stale-lock problem — the
    // same guarantee flock provides on POSIX.
    OVERLAPPED overlapped{};
    if (0 == LockFileEx(h,
                        LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY,
                        0,
                        MAXDWORD,
                        MAXDWORD,
                        &overlapped))
    {
        CloseHandle(h);
        m_handle = nullptr;
    }
    else
    {
        m_handle = h;
    }
#else
    m_fd = ::open(lock_path.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0644);
    if (m_fd < 0)
    {
        // Cannot open the lock file (permissions etc.) — fail closed:
        // acquired() == false → the engine refuses to start.
        return;
    }

    // LOCK_NB: do not block waiting for the other instance to finish.
    // flock() is tied to the open file description, so a second open() in
    // the same or another process fails while the first holds it.
    if (0 != ::flock(m_fd, LOCK_EX | LOCK_NB))
    {
        ::close(m_fd);
        m_fd = -1;
    }
#endif
}

SingleInstanceGuard::~SingleInstanceGuard()
{
#ifdef _WIN32
    if (m_handle != nullptr)
    {
        // Released implicitly on close anyway; explicit for clarity.
        CloseHandle(static_cast<HANDLE>(m_handle));
        m_handle = nullptr;
    }
#else
    if (m_fd >= 0)
    {
        // Released implicitly on close anyway; explicit for clarity.
        ::flock(m_fd, LOCK_UN);
        ::close(m_fd);
    }
#endif
}

} // namespace pulse
