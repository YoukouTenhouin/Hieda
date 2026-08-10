// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <system_error>
#include <variant>

namespace hieda::notebook::platform {

enum class FileErrorKind : std::uint8_t {
    alreadyLocked,
    alreadyExists,
    permissionDenied,
    notFound,
    other,
};

struct FileError {
    FileErrorKind kind;
    std::error_code systemError;
};

class ExclusiveFileLock final {
  public:
    ~ExclusiveFileLock();

    ExclusiveFileLock(ExclusiveFileLock&&) noexcept;
    auto operator=(ExclusiveFileLock&&) noexcept -> ExclusiveFileLock&;

    ExclusiveFileLock(const ExclusiveFileLock&) = delete;
    auto operator=(const ExclusiveFileLock&) -> ExclusiveFileLock& = delete;

  private:
    class Impl;

    explicit ExclusiveFileLock(std::unique_ptr<Impl> impl);

    std::unique_ptr<Impl> impl_;

    friend auto acquireExclusiveFileLock(const std::filesystem::path& path,
                                         bool create)
        -> std::variant<ExclusiveFileLock, FileError>;
};

[[nodiscard]] auto acquireExclusiveFileLock(const std::filesystem::path& path,
                                            bool create)
    -> std::variant<ExclusiveFileLock, FileError>;
[[nodiscard]] auto publishNewFile(const std::filesystem::path& temporaryPath,
                                  const std::filesystem::path& destinationPath)
    -> std::optional<FileError>;
[[nodiscard]] auto syncParentDirectory(const std::filesystem::path& path)
    -> std::optional<FileError>;

} // namespace hieda::notebook::platform
