// SPDX-License-Identifier: MPL-2.0
#include "hieda/notebook/notebook_session.hpp"
#include "platform_file.hpp"

#include <lmdb.h>

#include <array>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <mutex>
#include <random>
#include <sstream>
#include <system_error>
#include <vector>

namespace hieda::notebook {
namespace {

constexpr std::uint32_t formatVersion = 1;
constexpr std::uint32_t schemaVersion = 1;
constexpr std::size_t mapSize = 8ULL * 1024ULL * 1024ULL * 1024ULL;
constexpr std::string_view formatMagic = "HIEDA_NOTEBOOK";

constexpr std::array<std::string_view, 12> databaseNames{
    "metadata",
    "blocks",
    "blocks_by_type",
    "containment_by_parent",
    "containment_by_child",
    "references_by_source",
    "references_by_target",
    "properties_by_block",
    "property_index",
    "pages_by_title",
    "journal_by_date",
    "settings",
};

struct Manifest {
    NotebookId id;
    std::int64_t createdAtMicroseconds{0};
    std::uint64_t revision{0};
};

auto makeError(NotebookErrorCode code, const std::filesystem::path& path, std::string detail)
    -> NotebookError {
    return {code, path, std::move(detail)};
}

auto errorFromSystem(const std::filesystem::path& path, const std::error_code& error,
                     std::string_view operation) -> NotebookError {
    auto code = NotebookErrorCode::ioFailure;
    if (error == std::errc::permission_denied || error == std::errc::read_only_file_system) {
        code = NotebookErrorCode::permissionDenied;
    } else if (error == std::errc::no_such_file_or_directory) {
        code = NotebookErrorCode::pathNotFound;
    }
    return makeError(code, path, std::string(operation) + ": " + error.message());
}

auto errorFromPlatform(const std::filesystem::path& path, const platform::FileError& error,
                       std::string_view operation) -> NotebookError {
    auto code = NotebookErrorCode::ioFailure;
    switch (error.kind) {
    case platform::FileErrorKind::alreadyLocked:
        code = NotebookErrorCode::alreadyInUse;
        break;
    case platform::FileErrorKind::alreadyExists:
        code = NotebookErrorCode::pathExists;
        break;
    case platform::FileErrorKind::permissionDenied:
        code = NotebookErrorCode::permissionDenied;
        break;
    case platform::FileErrorKind::notFound:
        code = NotebookErrorCode::pathNotFound;
        break;
    case platform::FileErrorKind::other:
        break;
    }
    return makeError(code, path, std::string(operation) + ": " + error.systemError.message());
}

auto pathWithSuffix(const std::filesystem::path& path, std::string_view suffix)
    -> std::filesystem::path {
    auto result = path;
#ifdef _WIN32
    result += std::wstring(suffix.begin(), suffix.end());
#else
    result += suffix;
#endif
    return result;
}

auto lmdbPath(const std::filesystem::path& path) -> std::string {
#ifdef _WIN32
    const auto utf8 = path.u8string();
    return {reinterpret_cast<const char*>(utf8.data()), utf8.size()};
#else
    return path.native();
#endif
}

auto errorFromLmdb(const std::filesystem::path& path, int error, std::string_view operation)
    -> NotebookError {
    if (error == MDB_KEYEXIST || error == MDB_PANIC || error == MDB_TLS_FULL ||
        error == MDB_BAD_TXN || error == MDB_BAD_RSLOT || error == MDB_BAD_VALSIZE ||
        error == MDB_INCOMPATIBLE || error == MDB_BAD_DBI || error == MDB_DBS_FULL ||
        error == MDB_PAGE_FULL || error == MDB_CURSOR_FULL) {
        throw NotebookException(std::string(operation) + ": " + mdb_strerror(error));
    }
    auto code = NotebookErrorCode::ioFailure;
    if (error == MDB_INVALID || error == MDB_CORRUPTED || error == MDB_PAGE_NOTFOUND ||
        error == MDB_NOTFOUND) {
        code = NotebookErrorCode::invalidNotebook;
    } else if (error == MDB_VERSION_MISMATCH) {
        code = NotebookErrorCode::unsupportedVersion;
    } else if (error == EACCES || error == EPERM) {
        code = NotebookErrorCode::permissionDenied;
    } else if (error == ENOENT) {
        code = NotebookErrorCode::pathNotFound;
    }
    return makeError(code, path, std::string(operation) + ": " + mdb_strerror(error));
}

auto openLmdbEnvironment(const std::filesystem::path& path) -> Result<MDB_env*> {
    MDB_env* environment = nullptr;
    auto result = mdb_env_create(&environment);
    if (result == MDB_SUCCESS) {
        result = mdb_env_set_maxdbs(environment, 16);
    }
    if (result == MDB_SUCCESS) {
        result = mdb_env_set_mapsize(environment, mapSize);
    }
    if (result == MDB_SUCCESS) {
        const auto encodedPath = lmdbPath(path);
        result = mdb_env_open(environment, encodedPath.c_str(), MDB_NOSUBDIR, 0600);
    }
    if (result != MDB_SUCCESS) {
        if (environment != nullptr) {
            mdb_env_close(environment);
        }
        return Result<MDB_env*>::failure(errorFromLmdb(path, result, "open LMDB environment"));
    }
    return Result<MDB_env*>::success(environment);
}

void appendU16(std::vector<std::uint8_t>& output, std::uint16_t value) {
    output.push_back(static_cast<std::uint8_t>(value >> 8U));
    output.push_back(static_cast<std::uint8_t>(value));
}

void appendU32(std::vector<std::uint8_t>& output, std::uint32_t value) {
    for (int shift = 24; shift >= 0; shift -= 8) {
        output.push_back(static_cast<std::uint8_t>(value >> static_cast<unsigned>(shift)));
    }
}

void appendU64(std::vector<std::uint8_t>& output, std::uint64_t value) {
    for (int shift = 56; shift >= 0; shift -= 8) {
        output.push_back(static_cast<std::uint8_t>(value >> static_cast<unsigned>(shift)));
    }
}

void appendField(std::vector<std::uint8_t>& output, std::uint16_t tag, const std::uint8_t* data,
                 std::size_t size) {
    appendU16(output, tag);
    appendU32(output, static_cast<std::uint32_t>(size));
    output.insert(output.end(), data, data + size);
}

auto encodeManifest(const Manifest& manifest) -> std::vector<std::uint8_t> {
    std::vector<std::uint8_t> output;
    appendU16(output, 1);
    appendField(output, 1, reinterpret_cast<const std::uint8_t*>(formatMagic.data()),
                formatMagic.size());

    std::vector<std::uint8_t> number;
    appendU32(number, formatVersion);
    appendField(output, 2, number.data(), number.size());
    number.clear();
    appendU32(number, schemaVersion);
    appendField(output, 3, number.data(), number.size());
    appendField(output, 4, reinterpret_cast<const std::uint8_t*>(manifest.id.bytes.data()),
                manifest.id.bytes.size());
    number.clear();
    appendU64(number, static_cast<std::uint64_t>(manifest.createdAtMicroseconds));
    appendField(output, 5, number.data(), number.size());
    number.clear();
    appendU64(number, manifest.revision);
    appendField(output, 6, number.data(), number.size());
    return output;
}

auto readU16(const std::uint8_t* data) -> std::uint16_t {
    return static_cast<std::uint16_t>((static_cast<std::uint16_t>(data[0]) << 8U) | data[1]);
}

auto readU32(const std::uint8_t* data) -> std::uint32_t {
    std::uint32_t value = 0;
    for (int index = 0; index < 4; ++index) {
        value = (value << 8U) | data[index];
    }
    return value;
}

auto readU64(const std::uint8_t* data) -> std::uint64_t {
    std::uint64_t value = 0;
    for (int index = 0; index < 8; ++index) {
        value = (value << 8U) | data[index];
    }
    return value;
}

auto decodeManifest(const MDB_val& value, const std::filesystem::path& path) -> Result<Manifest> {
    const auto* bytes = static_cast<const std::uint8_t*>(value.mv_data);
    const auto size = value.mv_size;
    if (size < 2 || readU16(bytes) != 1) {
        return Result<Manifest>::failure(
            makeError(NotebookErrorCode::invalidNotebook, path, "invalid manifest record version"));
    }

    Manifest manifest;
    bool hasMagic = false;
    bool hasFormatVersion = false;
    bool hasSchemaVersion = false;
    bool hasId = false;
    std::size_t offset = 2;
    while (offset < size) {
        if (size - offset < 6) {
            return Result<Manifest>::failure(
                makeError(NotebookErrorCode::invalidNotebook, path, "truncated manifest field"));
        }
        const auto tag = readU16(bytes + offset);
        const auto length = static_cast<std::size_t>(readU32(bytes + offset + 2));
        offset += 6;
        if (length > size - offset) {
            return Result<Manifest>::failure(makeError(NotebookErrorCode::invalidNotebook, path,
                                                       "invalid manifest field length"));
        }
        const auto* field = bytes + offset;
        if (tag == 1 && length == formatMagic.size()) {
            hasMagic = std::memcmp(field, formatMagic.data(), length) == 0;
        } else if (tag == 2 && length == 4) {
            hasFormatVersion = true;
            if (readU32(field) != formatVersion) {
                return Result<Manifest>::failure(makeError(NotebookErrorCode::unsupportedVersion,
                                                           path,
                                                           "unsupported Notebook format version"));
            }
        } else if (tag == 3 && length == 4) {
            hasSchemaVersion = true;
            if (readU32(field) != schemaVersion) {
                return Result<Manifest>::failure(makeError(NotebookErrorCode::unsupportedVersion,
                                                           path,
                                                           "unsupported Notebook schema version"));
            }
        } else if (tag == 4 && length == manifest.id.bytes.size()) {
            std::memcpy(manifest.id.bytes.data(), field, length);
            hasId = true;
        } else if (tag == 5 && length == 8) {
            manifest.createdAtMicroseconds = static_cast<std::int64_t>(readU64(field));
        } else if (tag == 6 && length == 8) {
            manifest.revision = readU64(field);
        }
        offset += length;
    }

    if (!hasMagic || !hasFormatVersion || !hasSchemaVersion || !hasId) {
        return Result<Manifest>::failure(
            makeError(NotebookErrorCode::invalidNotebook, path, "Notebook manifest is incomplete"));
    }
    return Result<Manifest>::success(manifest);
}

auto generateId() -> NotebookId {
    std::random_device random;
    NotebookId id;
    for (auto& byte : id.bytes) {
        byte = static_cast<std::byte>(random() & 0xFFU);
    }
    id.bytes[6] = static_cast<std::byte>((std::to_integer<unsigned>(id.bytes[6]) & 0x0FU) | 0x40U);
    id.bytes[8] = static_cast<std::byte>((std::to_integer<unsigned>(id.bytes[8]) & 0x3FU) | 0x80U);
    return id;
}

auto createEnvironment(const std::filesystem::path& path, const Manifest& manifest)
    -> std::optional<NotebookError> {
    auto opened = openLmdbEnvironment(path);
    if (!opened) {
        return opened.error();
    }
    MDB_env* environment = opened.value();
    auto result = MDB_SUCCESS;
    const auto closeEnvironment = [&environment]() -> void {
        if (environment != nullptr) {
            mdb_env_close(environment);
            environment = nullptr;
        }
    };

    MDB_txn* transaction = nullptr;
    result = mdb_txn_begin(environment, nullptr, 0, &transaction);
    if (result != MDB_SUCCESS) {
        auto error = errorFromLmdb(path, result, "begin schema transaction");
        closeEnvironment();
        return error;
    }

    MDB_dbi metadata = 0;
    for (const auto name : databaseNames) {
        MDB_dbi database = 0;
        const std::string ownedName(name);
        result = mdb_dbi_open(transaction, ownedName.c_str(), MDB_CREATE, &database);
        if (result != MDB_SUCCESS) {
            mdb_txn_abort(transaction);
            auto error = errorFromLmdb(path, result, "create Notebook schema");
            closeEnvironment();
            return error;
        }
        if (name == "metadata") {
            metadata = database;
        }
    }

    auto encodedManifest = encodeManifest(manifest);
    constexpr std::string_view keyText = "manifest";
    MDB_val key{keyText.size(), const_cast<char*>(keyText.data())};
    MDB_val value{encodedManifest.size(), encodedManifest.data()};
    result = mdb_put(transaction, metadata, &key, &value, MDB_NOOVERWRITE);
    if (result != MDB_SUCCESS) {
        mdb_txn_abort(transaction);
        auto error = errorFromLmdb(path, result, "commit Notebook manifest");
        closeEnvironment();
        return error;
    }
    result = mdb_txn_commit(transaction);
    transaction = nullptr;
    if (result != MDB_SUCCESS) {
        auto error = errorFromLmdb(path, result, "commit Notebook manifest");
        closeEnvironment();
        return error;
    }

    result = mdb_env_sync(environment, 1);
    closeEnvironment();
    if (result != MDB_SUCCESS) {
        return errorFromLmdb(path, result, "flush new Notebook");
    }
    return std::nullopt;
}

void removeIfPresent(const std::filesystem::path& path) noexcept {
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
}

} // namespace

class NotebookSession::Impl {
  public:
    ~Impl() {
        closeUnlocked();
    }

    void closeUnlocked() noexcept {
        if (environment != nullptr) {
            mdb_env_close(environment);
            environment = nullptr;
        }
        lockFile.reset();
        dataLockFile.reset();
        info.reset();
    }

    auto acquireLock(const std::filesystem::path& path) -> std::optional<NotebookError> {
        auto acquired =
            platform::acquireExclusiveFileLock(pathWithSuffix(path, ".open-lock"), true);
        if (const auto* error = std::get_if<platform::FileError>(&acquired)) {
            if (error->kind == platform::FileErrorKind::alreadyLocked) {
                return makeError(NotebookErrorCode::alreadyInUse, path,
                                 "Notebook is already open in another session");
            }
            return errorFromPlatform(path, *error, "lock Notebook");
        }
        lockFile.emplace(std::get<platform::ExclusiveFileLock>(std::move(acquired)));
        return std::nullopt;
    }

    auto acquireDataLock(const std::filesystem::path& path) -> std::optional<NotebookError> {
        auto acquired = platform::acquireExclusiveFileLock(path, false);
        if (const auto* error = std::get_if<platform::FileError>(&acquired)) {
            if (error->kind == platform::FileErrorKind::alreadyLocked) {
                return makeError(NotebookErrorCode::alreadyInUse, path,
                                 "Notebook is already open through another path");
            }
            return errorFromPlatform(path, *error, "lock Notebook data file");
        }
        dataLockFile.emplace(std::get<platform::ExclusiveFileLock>(std::move(acquired)));
        return std::nullopt;
    }

    auto openEnvironment(const std::filesystem::path& path) -> Result<NotebookInfo> {
        auto opened = openLmdbEnvironment(path);
        if (!opened) {
            return Result<NotebookInfo>::failure(opened.error());
        }
        MDB_env* openedEnvironment = opened.value();
        auto result = MDB_SUCCESS;

        MDB_txn* transaction = nullptr;
        MDB_dbi metadata = 0;
        result = mdb_txn_begin(openedEnvironment, nullptr, MDB_RDONLY, &transaction);
        if (result == MDB_SUCCESS) {
            result = mdb_dbi_open(transaction, "metadata", 0, &metadata);
        }
        constexpr std::string_view keyText = "manifest";
        MDB_val key{keyText.size(), const_cast<char*>(keyText.data())};
        MDB_val value{};
        if (result == MDB_SUCCESS) {
            result = mdb_get(transaction, metadata, &key, &value);
        }
        if (result != MDB_SUCCESS) {
            if (transaction != nullptr) {
                mdb_txn_abort(transaction);
            }
            mdb_env_close(openedEnvironment);
            return Result<NotebookInfo>::failure(
                errorFromLmdb(path, result, "read Notebook manifest"));
        }

        auto manifest = decodeManifest(value, path);
        mdb_txn_abort(transaction);
        if (!manifest) {
            mdb_env_close(openedEnvironment);
            return Result<NotebookInfo>::failure(manifest.error());
        }

        environment = openedEnvironment;
        info = NotebookInfo{manifest.value().id, path, schemaVersion};
        return Result<NotebookInfo>::success(*info);
    }

    auto finishOpen(const std::filesystem::path& path) -> Result<NotebookInfo> {
        try {
            auto opened = openEnvironment(path);
            if (!opened) {
                closeUnlocked();
            }
            return opened;
        } catch (...) {
            closeUnlocked();
            throw;
        }
    }

    mutable std::mutex mutex;
    MDB_env* environment{nullptr};
    std::optional<platform::ExclusiveFileLock> lockFile;
    std::optional<platform::ExclusiveFileLock> dataLockFile;
    std::optional<NotebookInfo> info;
};

auto NotebookId::toString() const -> std::string {
    std::ostringstream output;
    output << std::hex << std::setfill('0');
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        if (index == 4 || index == 6 || index == 8 || index == 10) {
            output << '-';
        }
        output << std::setw(2) << std::to_integer<unsigned>(bytes[index]);
    }
    return output.str();
}

NotebookSession::NotebookSession() : impl_(std::make_unique<Impl>()) {}

NotebookSession::~NotebookSession() = default;

auto NotebookSession::create(const std::filesystem::path& inputPath) -> Result<NotebookInfo> {
    std::scoped_lock lock(impl_->mutex);
    if (impl_->info) {
        return Result<NotebookInfo>::failure(
            makeError(NotebookErrorCode::alreadyOpen, inputPath, "a Notebook is already open"));
    }

    std::error_code filesystemError;
    const auto path = std::filesystem::absolute(inputPath, filesystemError).lexically_normal();
    if (filesystemError || path.filename().empty()) {
        return Result<NotebookInfo>::failure(
            makeError(NotebookErrorCode::invalidPath, inputPath, "invalid Notebook path"));
    }
    if (!std::filesystem::is_directory(path.parent_path(), filesystemError) || filesystemError) {
        return Result<NotebookInfo>::failure(
            makeError(NotebookErrorCode::invalidPath, path, "parent directory does not exist"));
    }
    if (std::filesystem::exists(path, filesystemError)) {
        return Result<NotebookInfo>::failure(
            makeError(NotebookErrorCode::pathExists, path, "the selected path already exists"));
    }
    if (filesystemError) {
        return Result<NotebookInfo>::failure(
            errorFromSystem(path, filesystemError, "inspect Notebook path"));
    }

    if (auto lockError = impl_->acquireLock(path)) {
        return Result<NotebookInfo>::failure(std::move(*lockError));
    }
    if (std::filesystem::exists(path, filesystemError)) {
        impl_->closeUnlocked();
        return Result<NotebookInfo>::failure(
            makeError(NotebookErrorCode::pathExists, path, "the selected path already exists"));
    }

    const auto id = generateId();
    const auto temporaryPath = pathWithSuffix(path, ".tmp-" + id.toString());
    const auto temporaryLockPath = pathWithSuffix(temporaryPath, "-lock");
    const auto createdAt = std::chrono::duration_cast<std::chrono::microseconds>(
                               std::chrono::system_clock::now().time_since_epoch())
                               .count();
    std::optional<NotebookError> creationError;
    try {
        creationError = createEnvironment(temporaryPath, Manifest{id, createdAt, 0});
    } catch (...) {
        removeIfPresent(temporaryPath);
        removeIfPresent(temporaryLockPath);
        impl_->closeUnlocked();
        throw;
    }
    if (creationError) {
        removeIfPresent(temporaryPath);
        removeIfPresent(temporaryLockPath);
        impl_->closeUnlocked();
        return Result<NotebookInfo>::failure(std::move(*creationError));
    }

    if (const auto publishError = platform::publishNewFile(temporaryPath, path)) {
        removeIfPresent(temporaryPath);
        removeIfPresent(temporaryLockPath);
        impl_->closeUnlocked();
        return Result<NotebookInfo>::failure(
            errorFromPlatform(path, *publishError, "publish Notebook"));
    }
    removeIfPresent(temporaryPath);
    removeIfPresent(temporaryLockPath);

    if (auto lockError = impl_->acquireDataLock(path)) {
        impl_->closeUnlocked();
        return Result<NotebookInfo>::failure(std::move(*lockError));
    }

    if (const auto syncError = platform::syncParentDirectory(path)) {
        impl_->closeUnlocked();
        return Result<NotebookInfo>::failure(
            errorFromPlatform(path, *syncError, "flush Notebook parent directory"));
    }

    return impl_->finishOpen(path);
}

auto NotebookSession::open(const std::filesystem::path& inputPath) -> Result<NotebookInfo> {
    std::scoped_lock lock(impl_->mutex);
    if (impl_->info) {
        return Result<NotebookInfo>::failure(
            makeError(NotebookErrorCode::alreadyOpen, inputPath, "a Notebook is already open"));
    }

    std::error_code filesystemError;
    const auto path = std::filesystem::absolute(inputPath, filesystemError).lexically_normal();
    if (filesystemError || !std::filesystem::exists(path, filesystemError)) {
        return Result<NotebookInfo>::failure(
            makeError(NotebookErrorCode::pathNotFound, inputPath, "Notebook does not exist"));
    }
    if (!std::filesystem::is_regular_file(path, filesystemError) || filesystemError) {
        return Result<NotebookInfo>::failure(
            makeError(NotebookErrorCode::invalidPath, path, "Notebook path is not a regular file"));
    }
    if (auto lockError = impl_->acquireLock(path)) {
        return Result<NotebookInfo>::failure(std::move(*lockError));
    }
    if (auto lockError = impl_->acquireDataLock(path)) {
        impl_->closeUnlocked();
        return Result<NotebookInfo>::failure(std::move(*lockError));
    }
    return impl_->finishOpen(path);
}

void NotebookSession::close() noexcept {
    std::scoped_lock lock(impl_->mutex);
    impl_->closeUnlocked();
}

auto NotebookSession::isOpen() const noexcept -> bool {
    std::scoped_lock lock(impl_->mutex);
    return impl_->info.has_value();
}

auto NotebookSession::current() const -> std::optional<NotebookInfo> {
    std::scoped_lock lock(impl_->mutex);
    return impl_->info;
}

} // namespace hieda::notebook
