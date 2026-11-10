// SingleInstanceGuard.cpp — exclusive file lock for single-instance
// enforcement. See SingleInstanceGuard.hpp for the design rationale.

#include "core/SingleInstanceGuard.hpp"

#if defined(_WIN32)
// <windows.h> defines min/max macros unless NOMINMAX is set, and pulls in
// winsock 1 unless WIN32_LEAN_AND_MEAN is set — both collide with headers the
// rest of the tree includes. Lowercase spellings are the SDK's own idiom.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
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

SingleInstanceGuard::SingleInstanceGuard(const std::string &lock_path)
{
    // Create the parent directory (e.g. data/) if it does not exist yet.
    std::error_code ec;
    const auto parent = std::filesystem::path(lock_path).parent_path();
    if (!parent.empty())
    {
        std::filesystem::create_directories(parent, ec);
    }

#if defined(_WIN32)
    // dwShareMode = 0 IS the lock on Windows: while this handle stays open,
    // every other CreateFileW on the same path fails with
    // ERROR_SHARING_VIOLATION, which reproduces flock(LOCK_EX|LOCK_NB).
    // The handle is non-inheritable by default, so a child process cannot
    // keep the lock alive after the owner exits.
    const std::filesystem::path native_path(lock_path);
    const HANDLE handle = ::CreateFileW(native_path.c_str(),
                                        GENERIC_READ | GENERIC_WRITE,
                                        0,
                                        nullptr,
                                        OPEN_ALWAYS,
                                        FILE_ATTRIBUTE_NORMAL,
                                        nullptr);
    if (INVALID_HANDLE_VALUE == handle)
    {
        // Cannot open the lock file (permissions, or a live instance holding
        // it) — fail closed: acquired() == false → the engine refuses to
        // start.
        return;
    }
    m_handle = reinterpret_cast<std::intptr_t>(handle);
#else
    const int fd = ::open(lock_path.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0644);
    if (fd < 0)
    {
        // Cannot open the lock file (permissions etc.) — fail closed:
        // acquired() == false → the engine refuses to start.
        return;
    }

    // LOCK_NB: do not block waiting for the other instance to finish.
    // flock() is tied to the open file description, so a second open() in
    // the same or another process fails while the first holds it.
    if (0 != ::flock(fd, LOCK_EX | LOCK_NB))
    {
        ::close(fd);
        return;
    }
    m_handle = fd;
#endif
}

SingleInstanceGuard::~SingleInstanceGuard()
{
    if (m_handle < 0)
    {
        return;
    }

#if defined(_WIN32)
    // Closing the handle releases the zero-share lock. Explicit for clarity.
    ::CloseHandle(reinterpret_cast<HANDLE>(m_handle));
#else
    // Released implicitly on close anyway; explicit for clarity.
    const int fd = static_cast<int>(m_handle);
    ::flock(fd, LOCK_UN);
    ::close(fd);
#endif
}

} // namespace pulse
