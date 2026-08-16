// SingleInstanceGuard.cpp — flock-based single-instance enforcement.
// See SingleInstanceGuard.hpp for the design rationale.

#include "core/SingleInstanceGuard.hpp"

#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

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
}

SingleInstanceGuard::~SingleInstanceGuard()
{
    if (m_fd >= 0)
    {
        // Released implicitly on close anyway; explicit for clarity.
        ::flock(m_fd, LOCK_UN);
        ::close(m_fd);
    }
}

} // namespace pulse
