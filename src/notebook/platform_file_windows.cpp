// SPDX-License-Identifier: MPL-2.0
#include "platform_file.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <utility>

namespace hieda::notebook::platform {
namespace {

auto makeFileError(DWORD error) -> FileError {
    auto kind = FileErrorKind::other;
    if (error == ERROR_LOCK_VIOLATION || error == ERROR_SHARING_VIOLATION) {
        kind = FileErrorKind::alreadyLocked;
    } else if (error == ERROR_ALREADY_EXISTS || error == ERROR_FILE_EXISTS) {
        kind = FileErrorKind::alreadyExists;
    } else if (error == ERROR_ACCESS_DENIED || error == ERROR_WRITE_PROTECT) {
        kind = FileErrorKind::permissionDenied;
    } else if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) {
        kind = FileErrorKind::notFound;
    }
    return {kind, std::error_code(static_cast<int>(error), std::system_category())};
}

} // namespace

class ExclusiveFileLock::Impl final {
  public:
    explicit Impl(HANDLE handle) : handle_(handle) {}

    Impl(HANDLE handle, HANDLE semaphore) : handle_(handle), semaphore_(semaphore) {}

    ~Impl() {
        if (semaphore_ != nullptr) {
            static_cast<void>(ReleaseSemaphore(semaphore_, 1, nullptr));
            static_cast<void>(CloseHandle(semaphore_));
        } else {
            OVERLAPPED overlapped{};
            static_cast<void>(UnlockFileEx(handle_, 0, 1, 0, &overlapped));
        }
        static_cast<void>(CloseHandle(handle_));
    }

  private:
    HANDLE handle_;
    HANDLE semaphore_{nullptr};
};

ExclusiveFileLock::ExclusiveFileLock(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
ExclusiveFileLock::~ExclusiveFileLock() = default;
ExclusiveFileLock::ExclusiveFileLock(ExclusiveFileLock&&) noexcept = default;
auto ExclusiveFileLock::operator=(ExclusiveFileLock&&) noexcept -> ExclusiveFileLock& = default;

auto acquireExclusiveFileLock(const std::filesystem::path& path, bool create)
    -> std::variant<ExclusiveFileLock, FileError> {
    if (!create) {
        // LockFileEx would also block LMDB's second handle from reading its metadata. Use the
        // stable file identity so hard-linked paths still contend without locking the data stream.
        const auto handle = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES,
                                        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle == INVALID_HANDLE_VALUE) {
            return makeFileError(GetLastError());
        }

        BY_HANDLE_FILE_INFORMATION information{};
        if (GetFileInformationByHandle(handle, &information) == 0) {
            const auto error = GetLastError();
            static_cast<void>(CloseHandle(handle));
            return makeFileError(error);
        }

        const auto semaphoreName = L"Global\\Hieda.Notebook." +
                                   std::to_wstring(information.dwVolumeSerialNumber) + L"." +
                                   std::to_wstring(information.nFileIndexHigh) + L"." +
                                   std::to_wstring(information.nFileIndexLow);
        const auto semaphore = CreateSemaphoreW(nullptr, 1, 1, semaphoreName.c_str());
        if (semaphore == nullptr) {
            const auto error = GetLastError();
            static_cast<void>(CloseHandle(handle));
            return makeFileError(error);
        }

        const auto waitResult = WaitForSingleObject(semaphore, 0);
        if (waitResult != WAIT_OBJECT_0) {
            const auto error =
                waitResult == WAIT_TIMEOUT
                    ? ERROR_LOCK_VIOLATION
                    : (waitResult == WAIT_FAILED ? GetLastError() : ERROR_GEN_FAILURE);
            static_cast<void>(CloseHandle(semaphore));
            static_cast<void>(CloseHandle(handle));
            return makeFileError(error);
        }
        return ExclusiveFileLock(std::make_unique<ExclusiveFileLock::Impl>(handle, semaphore));
    }

    const auto handle = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                    OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return makeFileError(GetLastError());
    }

    OVERLAPPED overlapped{};
    if (LockFileEx(handle, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0,
                   &overlapped) == 0) {
        const auto error = GetLastError();
        static_cast<void>(CloseHandle(handle));
        return makeFileError(error);
    }
    return ExclusiveFileLock(std::make_unique<ExclusiveFileLock::Impl>(handle));
}

auto publishNewFile(const std::filesystem::path& temporaryPath,
                    const std::filesystem::path& destinationPath) -> std::optional<FileError> {
    if (MoveFileExW(temporaryPath.c_str(), destinationPath.c_str(), MOVEFILE_WRITE_THROUGH) == 0) {
        return makeFileError(GetLastError());
    }
    return std::nullopt;
}

auto syncParentDirectory(const std::filesystem::path&) -> std::optional<FileError> {
    // MoveFileExW with MOVEFILE_WRITE_THROUGH does not return until publication is complete.
    return std::nullopt;
}

} // namespace hieda::notebook::platform
