// SPDX-License-Identifier: MPL-2.0
#include "platform_file.hpp"

#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

#include <cerrno>
#include <utility>

namespace hieda::notebook::platform {
namespace {

auto
makeFileError(int error) -> FileError
{
    auto kind = FileErrorKind::other;
    if (error == EWOULDBLOCK || error == EAGAIN) {
        kind = FileErrorKind::alreadyLocked;
    } else if (error == EEXIST) {
        kind = FileErrorKind::alreadyExists;
    } else if (error == EACCES || error == EPERM) {
        kind = FileErrorKind::permissionDenied;
    } else if (error == ENOENT) {
        kind = FileErrorKind::notFound;
    }
    return {kind, std::error_code(error, std::generic_category())};
}

} // namespace

class ExclusiveFileLock::Impl final {
  public:
    explicit Impl(int descriptor) : descriptor_(descriptor) {}

    ~Impl()
    {
        static_cast<void>(flock(descriptor_, LOCK_UN));
        static_cast<void>(::close(descriptor_));
    }

  private:
    int descriptor_;
};

ExclusiveFileLock::ExclusiveFileLock(std::unique_ptr<Impl> impl)
    : impl_(std::move(impl))
{
}
ExclusiveFileLock::~ExclusiveFileLock() = default;
ExclusiveFileLock::ExclusiveFileLock(ExclusiveFileLock&&) noexcept = default;
auto ExclusiveFileLock::operator=(ExclusiveFileLock&&) noexcept
    -> ExclusiveFileLock& = default;

auto
acquireExclusiveFileLock(const std::filesystem::path& path, bool create)
    -> std::variant<ExclusiveFileLock, FileError>
{
    auto flags = O_RDWR | O_CLOEXEC;
    if (create) {
        flags |= O_CREAT;
    }
    const auto descriptor = ::open(path.c_str(), flags, 0600);
    if (descriptor < 0) {
        return makeFileError(errno);
    }
    if (flock(descriptor, LOCK_EX | LOCK_NB) != 0) {
        const auto error = errno;
        static_cast<void>(::close(descriptor));
        return makeFileError(error);
    }
    return ExclusiveFileLock(
        std::make_unique<ExclusiveFileLock::Impl>(descriptor));
}

auto
publishNewFile(const std::filesystem::path& temporaryPath,
               const std::filesystem::path& destinationPath)
    -> std::optional<FileError>
{
    if (::link(temporaryPath.c_str(), destinationPath.c_str()) != 0) {
        return makeFileError(errno);
    }
    std::error_code ignored;
    std::filesystem::remove(temporaryPath, ignored);
    return std::nullopt;
}

auto
syncParentDirectory(const std::filesystem::path& path)
    -> std::optional<FileError>
{
    const auto descriptor =
        ::open(path.parent_path().c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (descriptor < 0) {
        return makeFileError(errno);
    }
    if (::fsync(descriptor) != 0) {
        const auto error = errno;
        static_cast<void>(::close(descriptor));
        return makeFileError(error);
    }
    static_cast<void>(::close(descriptor));
    return std::nullopt;
}

} // namespace hieda::notebook::platform
