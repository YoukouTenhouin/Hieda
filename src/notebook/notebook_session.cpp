// SPDX-License-Identifier: MPL-2.0
#include "hieda/notebook/notebook_session.hpp"
#include "notebook_session_test_access.hpp"
#include "platform_file.hpp"

#include <lmdb.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <deque>
#include <exception>
#include <iomanip>
#include <limits>
#include <mutex>
#include <random>
#include <ranges>
#include <sstream>
#include <system_error>
#include <unordered_map>
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

enum class BlockType : std::uint8_t { journalPage = 1, journalEntry = 2 };

struct BlockRecord {
    BlockType type{BlockType::journalEntry};
    BlockMetadata metadata;
    std::optional<JournalDate> journalDate;
    std::string authoredText;
};

class JournalCommitAdapter {
  public:
    virtual ~JournalCommitAdapter() = default;
    virtual auto commit(MDB_txn*& transaction) -> int = 0;
};

class LmdbJournalCommitAdapter final : public JournalCommitAdapter {
  public:
    auto commit(MDB_txn*& transaction) -> int override {
        auto* committing = transaction;
        transaction = nullptr;
        return mdb_txn_commit(committing);
    }
};

#ifdef HIEDA_TESTING
class RejectNextJournalCommitAdapter final : public JournalCommitAdapter {
  public:
    auto commit(MDB_txn*& transaction) -> int override {
        if (shouldReject_) {
            shouldReject_ = false;
            mdb_txn_abort(transaction);
            transaction = nullptr;
            return EIO;
        }
        auto* committing = transaction;
        transaction = nullptr;
        return mdb_txn_commit(committing);
    }

  private:
    bool shouldReject_{true};
};
#endif

struct SubscriptionState {
    std::mutex mutex;
    std::size_t nextIdentifier{1};
    std::unordered_map<std::size_t, std::function<void()>> callbacks;
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

auto generateBlockId() -> BlockId {
    std::random_device random;
    BlockId id;
    for (auto& byte : id.bytes) {
        byte = static_cast<std::byte>(random() & 0xFFU);
    }
    id.bytes[6] = static_cast<std::byte>((std::to_integer<unsigned>(id.bytes[6]) & 0x0FU) | 0x40U);
    id.bytes[8] = static_cast<std::byte>((std::to_integer<unsigned>(id.bytes[8]) & 0x3FU) | 0x80U);
    return id;
}

auto currentTimestamp() -> BlockTimestamp {
    return std::chrono::time_point_cast<std::chrono::microseconds>(
        std::chrono::system_clock::now());
}

auto validJournalDate(JournalDate date) -> bool {
    if (date.year < 1 || date.year > 9999) {
        return false;
    }
    const auto value = std::chrono::year_month_day{
        std::chrono::year{date.year}, std::chrono::month{date.month}, std::chrono::day{date.day}};
    return value.ok();
}

auto validAuthoredText(std::string_view text) -> bool {
    if (text.size() > std::numeric_limits<std::uint32_t>::max()) {
        return false;
    }
    if (text.find('\r') != std::string_view::npos) {
        return false;
    }
    std::size_t index = 0;
    while (index < text.size()) {
        const auto first = static_cast<unsigned char>(text[index]);
        std::size_t continuationCount = 0;
        std::uint32_t codePoint = 0;
        if (first <= 0x7FU) {
            ++index;
            continue;
        }
        if ((first & 0xE0U) == 0xC0U) {
            continuationCount = 1;
            codePoint = first & 0x1FU;
        } else if ((first & 0xF0U) == 0xE0U) {
            continuationCount = 2;
            codePoint = first & 0x0FU;
        } else if ((first & 0xF8U) == 0xF0U) {
            continuationCount = 3;
            codePoint = first & 0x07U;
        } else {
            return false;
        }
        if (continuationCount > text.size() - index - 1) {
            return false;
        }
        for (std::size_t offset = 1; offset <= continuationCount; ++offset) {
            const auto next = static_cast<unsigned char>(text[index + offset]);
            if ((next & 0xC0U) != 0x80U) {
                return false;
            }
            codePoint = (codePoint << 6U) | (next & 0x3FU);
        }
        auto minimum = 0x10000U;
        if (continuationCount == 1) {
            minimum = 0x80U;
        } else if (continuationCount == 2) {
            minimum = 0x800U;
        }
        if (codePoint < minimum || codePoint > 0x10FFFFU ||
            (codePoint >= 0xD800U && codePoint <= 0xDFFFU)) {
            return false;
        }
        index += continuationCount + 1;
    }
    return true;
}

auto timestampMicroseconds(BlockTimestamp timestamp) -> std::int64_t {
    return timestamp.time_since_epoch().count();
}

auto encodeBlock(const BlockRecord& block) -> std::vector<std::uint8_t> {
    std::vector<std::uint8_t> output;
    appendU16(output, 1);
    const auto type = static_cast<std::uint8_t>(block.type);
    appendField(output, 1, &type, 1);
    std::vector<std::uint8_t> number;
    appendU64(number, static_cast<std::uint64_t>(timestampMicroseconds(block.metadata.createdAt)));
    appendField(output, 2, number.data(), number.size());
    number.clear();
    appendU64(number, static_cast<std::uint64_t>(timestampMicroseconds(block.metadata.updatedAt)));
    appendField(output, 3, number.data(), number.size());
    const std::uint8_t active = 1;
    appendField(output, 4, &active, 1);
    if (block.journalDate) {
        number.clear();
        const auto packed =
            static_cast<std::uint32_t>((block.journalDate->year * 10000) +
                                       (block.journalDate->month * 100) + block.journalDate->day);
        appendU32(number, packed);
        appendField(output, 5, number.data(), number.size());
    }
    if (block.type == BlockType::journalEntry) {
        appendField(output, 6, reinterpret_cast<const std::uint8_t*>(block.authoredText.data()),
                    block.authoredText.size());
    }
    return output;
}

auto decodeBlock(const MDB_val& value, const BlockId& blockIdentifier,
                 const std::filesystem::path& path) -> Result<BlockRecord> {
    const auto* bytes = static_cast<const std::uint8_t*>(value.mv_data);
    if (value.mv_size < 2 || readU16(bytes) != 1) {
        return Result<BlockRecord>::failure(
            makeError(NotebookErrorCode::invalidNotebook, path, "invalid Block record version"));
    }
    BlockRecord block;
    block.metadata.id = blockIdentifier;
    bool hasType = false;
    bool hasCreated = false;
    bool hasUpdated = false;
    bool active = false;
    std::size_t offset = 2;
    while (offset < value.mv_size) {
        if (value.mv_size - offset < 6) {
            return Result<BlockRecord>::failure(
                makeError(NotebookErrorCode::invalidNotebook, path, "truncated Block field"));
        }
        const auto tag = readU16(bytes + offset);
        const auto length = static_cast<std::size_t>(readU32(bytes + offset + 2));
        offset += 6;
        if (length > value.mv_size - offset) {
            return Result<BlockRecord>::failure(
                makeError(NotebookErrorCode::invalidNotebook, path, "invalid Block field length"));
        }
        const auto* field = bytes + offset;
        if (tag == 1 && length == 1 && (field[0] == 1 || field[0] == 2)) {
            block.type = static_cast<BlockType>(field[0]);
            hasType = true;
        } else if (tag == 2 && length == 8) {
            block.metadata.createdAt = BlockTimestamp{
                std::chrono::microseconds{static_cast<std::int64_t>(readU64(field))}};
            hasCreated = true;
        } else if (tag == 3 && length == 8) {
            block.metadata.updatedAt = BlockTimestamp{
                std::chrono::microseconds{static_cast<std::int64_t>(readU64(field))}};
            hasUpdated = true;
        } else if (tag == 4 && length == 1) {
            active = field[0] == 1;
        } else if (tag == 5 && length == 4) {
            const auto packed = readU32(field);
            block.journalDate = JournalDate{static_cast<std::int32_t>(packed / 10000U),
                                            static_cast<std::uint8_t>((packed / 100U) % 100U),
                                            static_cast<std::uint8_t>(packed % 100U)};
        } else if (tag == 6) {
            block.authoredText.assign(reinterpret_cast<const char*>(field), length);
        }
        offset += length;
    }
    if (!hasType || !hasCreated || !hasUpdated || !active ||
        (block.type == BlockType::journalPage && !block.journalDate)) {
        return Result<BlockRecord>::failure(
            makeError(NotebookErrorCode::invalidNotebook, path, "Block record is incomplete"));
    }
    return Result<BlockRecord>::success(std::move(block));
}

auto dateKey(JournalDate date) -> std::array<std::uint8_t, 4> {
    const auto packed =
        static_cast<std::uint32_t>((date.year * 10000) + (date.month * 100) + date.day);
    return {static_cast<std::uint8_t>(packed >> 24U), static_cast<std::uint8_t>(packed >> 16U),
            static_cast<std::uint8_t>(packed >> 8U), static_cast<std::uint8_t>(packed)};
}

auto blockKey(const BlockId& blockIdentifier) -> MDB_val {
    return {blockIdentifier.bytes.size(), const_cast<std::byte*>(blockIdentifier.bytes.data())};
}

template <typename Identifier> auto formatUuid(const Identifier& identifier) -> std::string {
    std::ostringstream output;
    output << std::hex << std::setfill('0');
    for (std::size_t index = 0; index < identifier.bytes.size(); ++index) {
        if (index == 4 || index == 6 || index == 8 || index == 10) {
            output << '-';
        }
        output << std::setw(2) << std::to_integer<unsigned>(identifier.bytes[index]);
    }
    return output.str();
}

void notifyCallbacks(const std::vector<std::function<void()>>& callbacks) noexcept {
    for (const auto& callback : callbacks) {
        try {
            callback();
        } catch (...) {
            // A committed command remains successful even if an observer fails.
            static_cast<void>(std::current_exception());
        }
    }
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

struct JournalDatabases {
    MDB_dbi metadata{0};
    MDB_dbi blocks{0};
    MDB_dbi blocksByType{0};
    MDB_dbi containmentByParent{0};
    MDB_dbi containmentByChild{0};
    MDB_dbi journalByDate{0};
};

struct ParentLink {
    BlockId parent;
    std::uint64_t rank{0};
};

struct LoadedOutline {
    BlockRecord page;
    std::vector<JournalEntry> entries;
};

struct JournalHistoryAction {
    std::uint64_t sequence{0};
    JournalDate date;
    std::optional<LoadedOutline> before;
    std::optional<LoadedOutline> after;
    std::size_t estimatedBytes{0};
};

struct JournalPageHistory {
    JournalDate date;
    std::deque<JournalHistoryAction> undo;
    std::deque<JournalHistoryAction> redo;
};

enum class OutlineEditKind : std::uint8_t { split, join, indent, outdent, up, down, erase };

auto openJournalDatabases(MDB_txn* transaction, const std::filesystem::path& path)
    -> Result<JournalDatabases> {
    JournalDatabases databases;
    const std::array<std::pair<const char*, MDB_dbi*>, 6> names{{
        {"metadata", &databases.metadata},
        {"blocks", &databases.blocks},
        {"blocks_by_type", &databases.blocksByType},
        {"containment_by_parent", &databases.containmentByParent},
        {"containment_by_child", &databases.containmentByChild},
        {"journal_by_date", &databases.journalByDate},
    }};
    for (const auto& [name, database] : names) {
        const auto result = mdb_dbi_open(transaction, name, 0, database);
        if (result != MDB_SUCCESS) {
            return Result<JournalDatabases>::failure(
                errorFromLmdb(path, result, "open Journal database"));
        }
    }
    return Result<JournalDatabases>::success(databases);
}

auto readBlock(MDB_txn* transaction, MDB_dbi database, const BlockId& blockIdentifier,
               const std::filesystem::path& path) -> Result<BlockRecord> {
    auto key = blockKey(blockIdentifier);
    MDB_val value{};
    const auto result = mdb_get(transaction, database, &key, &value);
    if (result == MDB_NOTFOUND) {
        return Result<BlockRecord>::failure(
            makeError(NotebookErrorCode::blockNotFound, path, "Block does not exist"));
    }
    if (result != MDB_SUCCESS) {
        return Result<BlockRecord>::failure(errorFromLmdb(path, result, "read Block"));
    }
    return decodeBlock(value, blockIdentifier, path);
}

auto writeBlock(MDB_txn* transaction, MDB_dbi database, const BlockRecord& block,
                const std::filesystem::path& path) -> std::optional<NotebookError> {
    auto key = blockKey(block.metadata.id);
    auto encoded = encodeBlock(block);
    MDB_val value{encoded.size(), encoded.data()};
    const auto result = mdb_put(transaction, database, &key, &value, 0);
    if (result != MDB_SUCCESS) {
        return errorFromLmdb(path, result, "write Block");
    }
    return std::nullopt;
}

auto incrementRevision(MDB_txn* transaction, MDB_dbi metadata, const std::filesystem::path& path)
    -> std::optional<NotebookError> {
    constexpr std::string_view keyText = "manifest";
    MDB_val key{keyText.size(), const_cast<char*>(keyText.data())};
    MDB_val value{};
    auto result = mdb_get(transaction, metadata, &key, &value);
    if (result != MDB_SUCCESS) {
        return errorFromLmdb(path, result, "read Notebook revision");
    }
    auto manifest = decodeManifest(value, path);
    if (!manifest) {
        return manifest.error();
    }
    auto updatedManifest = std::move(manifest).value();
    ++updatedManifest.revision;
    auto encoded = encodeManifest(updatedManifest);
    MDB_val updated{encoded.size(), encoded.data()};
    result = mdb_put(transaction, metadata, &key, &updated, 0);
    if (result != MDB_SUCCESS) {
        return errorFromLmdb(path, result, "write Notebook revision");
    }
    return std::nullopt;
}

auto typeIndexKey(BlockType type, const BlockId& blockIdentifier) -> std::array<std::uint8_t, 17> {
    std::array<std::uint8_t, 17> key{};
    key[0] = static_cast<std::uint8_t>(type);
    std::memcpy(key.data() + 1, blockIdentifier.bytes.data(), blockIdentifier.bytes.size());
    return key;
}

auto containmentParentKey(const BlockId& parent, std::uint64_t rank)
    -> std::array<std::uint8_t, 24> {
    std::array<std::uint8_t, 24> key{};
    std::memcpy(key.data(), parent.bytes.data(), parent.bytes.size());
    for (std::size_t index = 0; index < 8; ++index) {
        key[16 + index] = static_cast<std::uint8_t>(rank >> ((7U - index) * 8U));
    }
    return key;
}

auto rankFromParentKey(const MDB_val& key) -> std::uint64_t {
    const auto* bytes = static_cast<const std::uint8_t*>(key.mv_data);
    return readU64(bytes + 16);
}

auto writeTypeIndex(MDB_txn* transaction, MDB_dbi database, BlockType type,
                    const BlockId& blockIdentifier, const std::filesystem::path& path)
    -> std::optional<NotebookError> {
    auto encoded = typeIndexKey(type, blockIdentifier);
    MDB_val key{encoded.size(), encoded.data()};
    MDB_val value{0, nullptr};
    const auto result = mdb_put(transaction, database, &key, &value, MDB_NOOVERWRITE);
    if (result != MDB_SUCCESS) {
        return errorFromLmdb(path, result, "index Block type");
    }
    return std::nullopt;
}

} // namespace

class NotebookSubscription::Impl {
  public:
    Impl(std::weak_ptr<SubscriptionState> state, std::size_t identifier)
        : state_(std::move(state)), identifier_(identifier) {}

    ~Impl() {
        if (const auto state = state_.lock()) {
            std::scoped_lock lock(state->mutex);
            state->callbacks.erase(identifier_);
        }
    }

  private:
    std::weak_ptr<SubscriptionState> state_;
    std::size_t identifier_;
};

class NotebookSession::Impl {
  public:
    Impl()
        : commitAdapter(std::make_unique<LmdbJournalCommitAdapter>()),
          subscriptions(std::make_shared<SubscriptionState>()) {}

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
        journalHistory.clear();
        historyBytes = 0;
        nextHistorySequence = 1;
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
        info = NotebookInfo{manifest.value().id, path, schemaVersion, manifest.value().revision};
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

    auto childrenOf(MDB_txn* transaction, const JournalDatabases& databases,
                    const BlockId& parent) const
        -> Result<std::vector<std::pair<BlockId, std::uint64_t>>> {
        const auto path = info.value_or(NotebookInfo{}).path;
        std::vector<std::pair<BlockId, std::uint64_t>> children;
        MDB_cursor* cursor = nullptr;
        auto result = mdb_cursor_open(transaction, databases.containmentByParent, &cursor);
        if (result != MDB_SUCCESS) {
            return Result<std::vector<std::pair<BlockId, std::uint64_t>>>::failure(
                errorFromLmdb(path, result, "open Containment children"));
        }
        auto start = containmentParentKey(parent, 0);
        MDB_val key{start.size(), start.data()};
        MDB_val value{};
        result = mdb_cursor_get(cursor, &key, &value, MDB_SET_RANGE);
        while (result == MDB_SUCCESS && key.mv_size == 24 &&
               std::memcmp(key.mv_data, parent.bytes.data(), parent.bytes.size()) == 0) {
            if (value.mv_size != BlockId{}.bytes.size()) {
                mdb_cursor_close(cursor);
                return Result<std::vector<std::pair<BlockId, std::uint64_t>>>::failure(makeError(
                    NotebookErrorCode::invalidNotebook, path, "invalid Containment child"));
            }
            BlockId child;
            std::memcpy(child.bytes.data(), value.mv_data, child.bytes.size());
            children.emplace_back(child, rankFromParentKey(key));
            result = mdb_cursor_get(cursor, &key, &value, MDB_NEXT);
        }
        mdb_cursor_close(cursor);
        if (result != MDB_SUCCESS && result != MDB_NOTFOUND) {
            return Result<std::vector<std::pair<BlockId, std::uint64_t>>>::failure(
                errorFromLmdb(path, result, "read Containment children"));
        }
        return Result<std::vector<std::pair<BlockId, std::uint64_t>>>::success(std::move(children));
    }

    auto parentOf(MDB_txn* transaction, const JournalDatabases& databases,
                  const BlockId& child) const -> Result<ParentLink> {
        const auto path = info.value_or(NotebookInfo{}).path;
        auto key = blockKey(child);
        MDB_val value{};
        const auto result = mdb_get(transaction, databases.containmentByChild, &key, &value);
        if (result == MDB_NOTFOUND) {
            return Result<ParentLink>::failure(makeError(NotebookErrorCode::invalidNotebook, path,
                                                         "contained Block has no parent"));
        }
        if (result != MDB_SUCCESS || value.mv_size != 24) {
            return Result<ParentLink>::failure(
                result == MDB_SUCCESS ? makeError(NotebookErrorCode::invalidNotebook, path,
                                                  "invalid Containment parent index")
                                      : errorFromLmdb(path, result, "read Containment parent"));
        }
        ParentLink link;
        std::memcpy(link.parent.bytes.data(), value.mv_data, link.parent.bytes.size());
        MDB_val encoded{value.mv_size, value.mv_data};
        link.rank = rankFromParentKey(encoded);
        return Result<ParentLink>::success(link);
    }

    auto loadOutline(MDB_txn* transaction, const JournalDatabases& databases,
                     BlockRecord page) const -> Result<LoadedOutline> {
        const auto path = info.value_or(NotebookInfo{}).path;
        LoadedOutline outline{std::move(page), {}};
        struct PendingEntry {
            BlockId id;
            std::optional<BlockId> parentEntry;
            std::uint64_t rank{0};
        };
        std::vector<PendingEntry> pending;
        auto roots = childrenOf(transaction, databases, outline.page.metadata.id);
        if (!roots) {
            return Result<LoadedOutline>::failure(roots.error());
        }
        for (const auto& [identifier, rank] : roots.value() | std::views::reverse) {
            pending.push_back({identifier, std::nullopt, rank});
        }
        std::vector<BlockId> visited;
        while (!pending.empty()) {
            const auto current = pending.back();
            pending.pop_back();
            if (std::ranges::find(visited, current.id) != visited.end()) {
                return Result<LoadedOutline>::failure(makeError(
                    NotebookErrorCode::invalidNotebook, path, "cyclic Journal Containment"));
            }
            visited.push_back(current.id);
            auto parent = parentOf(transaction, databases, current.id);
            const auto expectedParent = current.parentEntry.value_or(outline.page.metadata.id);
            if (!parent || parent.value().parent != expectedParent ||
                parent.value().rank != current.rank) {
                return Result<LoadedOutline>::failure(
                    parent ? makeError(NotebookErrorCode::invalidNotebook, path,
                                       "inconsistent Journal Containment indexes")
                           : parent.error());
            }
            auto block = readBlock(transaction, databases.blocks, current.id, path);
            if (!block || block.value().type != BlockType::journalEntry) {
                return Result<LoadedOutline>::failure(
                    block ? makeError(NotebookErrorCode::invalidNotebook, path,
                                      "Journal contains a non-Entry Block")
                          : block.error());
            }
            outline.entries.push_back(
                {block.value().metadata, block.value().authoredText, current.parentEntry});
            auto children = childrenOf(transaction, databases, current.id);
            if (!children) {
                return Result<LoadedOutline>::failure(children.error());
            }
            for (const auto& [identifier, rank] : children.value() | std::views::reverse) {
                pending.push_back({identifier, current.id, rank});
            }
        }
        return Result<LoadedOutline>::success(std::move(outline));
    }

    auto loadOutlineForEntry(MDB_txn* transaction, const JournalDatabases& databases,
                             BlockId entryId) const -> Result<LoadedOutline> {
        const auto path = info.value_or(NotebookInfo{}).path;
        auto entry = readBlock(transaction, databases.blocks, entryId, path);
        if (!entry || entry.value().type != BlockType::journalEntry) {
            return Result<LoadedOutline>::failure(
                entry ? makeError(NotebookErrorCode::blockNotFound, path,
                                  "Block is not a Journal Entry")
                      : entry.error());
        }
        std::vector<BlockId> visited{entryId};
        auto current = entryId;
        while (true) {
            auto parent = parentOf(transaction, databases, current);
            if (!parent) {
                return Result<LoadedOutline>::failure(parent.error());
            }
            if (std::ranges::find(visited, parent.value().parent) != visited.end()) {
                return Result<LoadedOutline>::failure(makeError(
                    NotebookErrorCode::invalidNotebook, path, "cyclic Journal Containment"));
            }
            visited.push_back(parent.value().parent);
            auto block = readBlock(transaction, databases.blocks, parent.value().parent, path);
            if (!block) {
                return Result<LoadedOutline>::failure(block.error());
            }
            if (block.value().type == BlockType::journalPage) {
                return loadOutline(transaction, databases, std::move(block).value());
            }
            if (block.value().type != BlockType::journalEntry) {
                return Result<LoadedOutline>::failure(makeError(NotebookErrorCode::invalidNotebook,
                                                                path, "invalid Journal ancestor"));
            }
            current = parent.value().parent;
        }
    }

    auto eraseContainment(MDB_txn* transaction, const JournalDatabases& databases,
                          const JournalEntry& entry) const -> std::optional<NotebookError> {
        const auto path = info.value_or(NotebookInfo{}).path;
        auto parent = parentOf(transaction, databases, entry.metadata.id);
        if (!parent) {
            return parent.error();
        }
        auto parentKey = containmentParentKey(parent.value().parent, parent.value().rank);
        MDB_val encodedParent{parentKey.size(), parentKey.data()};
        auto result = mdb_del(transaction, databases.containmentByParent, &encodedParent, nullptr);
        if (result != MDB_SUCCESS) {
            return errorFromLmdb(path, result, "remove Containment ordering");
        }
        auto childKey = blockKey(entry.metadata.id);
        result = mdb_del(transaction, databases.containmentByChild, &childKey, nullptr);
        if (result != MDB_SUCCESS) {
            return errorFromLmdb(path, result, "remove Containment parent");
        }
        return std::nullopt;
    }

    auto rewriteContainment(MDB_txn* transaction, const JournalDatabases& databases,
                            const LoadedOutline& before, const LoadedOutline& after) const
        -> std::optional<NotebookError> {
        const auto path = info.value_or(NotebookInfo{}).path;
        for (const auto& entry : before.entries) {
            if (auto error = eraseContainment(transaction, databases, entry)) {
                return error;
            }
        }
        constexpr std::uint64_t rankGap = 1ULL << 32U;
        std::vector<std::pair<BlockId, std::uint64_t>> nextRanks;
        for (const auto& entry : after.entries) {
            const auto parent = entry.parentEntry.value_or(after.page.metadata.id);
            auto rank = rankGap;
            auto found = std::ranges::find_if(
                nextRanks, [&](const auto& item) -> bool { return item.first == parent; });
            if (found == nextRanks.end()) {
                nextRanks.emplace_back(parent, rankGap * 2U);
            } else {
                rank = found->second;
                found->second += rankGap;
            }
            auto parentBytes = containmentParentKey(parent, rank);
            MDB_val parentKey{parentBytes.size(), parentBytes.data()};
            MDB_val childValue{entry.metadata.id.bytes.size(),
                               const_cast<std::byte*>(entry.metadata.id.bytes.data())};
            auto result = mdb_put(transaction, databases.containmentByParent, &parentKey,
                                  &childValue, MDB_NOOVERWRITE);
            if (result != MDB_SUCCESS) {
                return errorFromLmdb(path, result, "write Containment ordering");
            }
            auto childKey = blockKey(entry.metadata.id);
            MDB_val parentValue{parentBytes.size(), parentBytes.data()};
            result = mdb_put(transaction, databases.containmentByChild, &childKey, &parentValue,
                             MDB_NOOVERWRITE);
            if (result != MDB_SUCCESS) {
                return errorFromLmdb(path, result, "write Containment parent");
            }
        }
        return std::nullopt;
    }

    static auto subtreeEnd(const std::vector<JournalEntry>& entries, std::size_t root)
        -> std::size_t {
        const auto rootId = entries[root].metadata.id;
        auto index = root + 1;
        for (; index < entries.size(); ++index) {
            auto parent = entries[index].parentEntry;
            bool descendant = false;
            while (parent) {
                if (*parent == rootId) {
                    descendant = true;
                    break;
                }
                const auto found =
                    std::ranges::find_if(entries, [&](const auto& candidate) -> bool {
                        return candidate.metadata.id == *parent;
                    });
                if (found == entries.end()) {
                    break;
                }
                parent = found->parentEntry;
            }
            if (!descendant) {
                break;
            }
        }
        return index;
    }

    auto touchContainer(MDB_txn* transaction, const JournalDatabases& databases,
                        LoadedOutline& outline, std::optional<BlockId> parent,
                        BlockTimestamp now) const -> std::optional<NotebookError> {
        if (!parent) {
            outline.page.metadata.updatedAt = now;
            return writeBlock(transaction, databases.blocks, outline.page,
                              info.value_or(NotebookInfo{}).path);
        }
        const auto found = std::ranges::find_if(outline.entries, [&](const auto& entry) -> bool {
            return entry.metadata.id == *parent;
        });
        if (found == outline.entries.end()) {
            return makeError(NotebookErrorCode::invalidNotebook, info.value_or(NotebookInfo{}).path,
                             "Journal parent is outside its Page");
        }
        auto loaded =
            readBlock(transaction, databases.blocks, *parent, info.value_or(NotebookInfo{}).path);
        if (!loaded) {
            return loaded.error();
        }
        auto block = std::move(loaded).value();
        block.metadata.updatedAt = now;
        found->metadata.updatedAt = now;
        return writeBlock(transaction, databases.blocks, block, info.value_or(NotebookInfo{}).path);
    }

    auto loadOutlineForDate(MDB_txn* transaction, const JournalDatabases& databases,
                            JournalDate date) const -> Result<std::optional<LoadedOutline>> {
        const auto path = info.value_or(NotebookInfo{}).path;
        const auto encodedDate = dateKey(date);
        MDB_val key{encodedDate.size(), const_cast<std::uint8_t*>(encodedDate.data())};
        MDB_val value{};
        const auto result = mdb_get(transaction, databases.journalByDate, &key, &value);
        if (result == MDB_NOTFOUND) {
            return Result<std::optional<LoadedOutline>>::success(std::nullopt);
        }
        if (result != MDB_SUCCESS || value.mv_size != BlockId{}.bytes.size()) {
            return Result<std::optional<LoadedOutline>>::failure(
                result == MDB_SUCCESS ? makeError(NotebookErrorCode::invalidNotebook, path,
                                                  "invalid Journal date index")
                                      : errorFromLmdb(path, result, "read Journal date index"));
        }
        BlockId pageId;
        std::memcpy(pageId.bytes.data(), value.mv_data, pageId.bytes.size());
        auto page = readBlock(transaction, databases.blocks, pageId, path);
        if (!page || page.value().type != BlockType::journalPage ||
            page.value().journalDate != date) {
            return Result<std::optional<LoadedOutline>>::failure(
                page ? makeError(NotebookErrorCode::invalidNotebook, path,
                                 "Journal date points to an invalid Page")
                     : page.error());
        }
        auto outline = loadOutline(transaction, databases, std::move(page).value());
        if (!outline) {
            return Result<std::optional<LoadedOutline>>::failure(outline.error());
        }
        return Result<std::optional<LoadedOutline>>::success(std::move(outline).value());
    }

    static auto estimateOutlineBytes(const std::optional<LoadedOutline>& outline) -> std::size_t {
        if (!outline) {
            return sizeof(std::optional<LoadedOutline>);
        }
        auto bytes = sizeof(LoadedOutline) + outline->page.authoredText.size();
        for (const auto& entry : outline->entries) {
            bytes += sizeof(JournalEntry) + entry.authoredText.size();
        }
        return bytes;
    }

    auto pageHistory(JournalDate date) -> JournalPageHistory& {
        const auto found = std::ranges::find_if(
            journalHistory, [&](const auto& history) -> bool { return history.date == date; });
        if (found != journalHistory.end()) {
            return *found;
        }
        return journalHistory.emplace_back(JournalPageHistory{date, {}, {}});
    }

    auto pageHistory(JournalDate date) const -> const JournalPageHistory* {
        const auto found = std::ranges::find_if(
            journalHistory, [&](const auto& history) -> bool { return history.date == date; });
        return found == journalHistory.end() ? nullptr : &*found;
    }

    auto historyActionCount() const -> std::size_t {
        std::size_t count = 0;
        for (const auto& history : journalHistory) {
            count += history.undo.size() + history.redo.size();
        }
        return count;
    }

    void enforceHistoryBudget() {
        constexpr std::size_t historyBudget = 32ULL * 1024ULL * 1024ULL;
        while (historyBytes > historyBudget && historyActionCount() > 1) {
            JournalPageHistory* oldestHistory = nullptr;
            bool oldestIsRedo = false;
            auto oldestSequence = std::numeric_limits<std::uint64_t>::max();
            for (auto& history : journalHistory) {
                if (!history.undo.empty() && history.undo.front().sequence < oldestSequence) {
                    oldestHistory = &history;
                    oldestIsRedo = false;
                    oldestSequence = history.undo.front().sequence;
                }
                if (!history.redo.empty() && history.redo.back().sequence < oldestSequence) {
                    oldestHistory = &history;
                    oldestIsRedo = true;
                    oldestSequence = history.redo.back().sequence;
                }
            }
            if (oldestHistory == nullptr) {
                break;
            }
            if (oldestIsRedo) {
                for (const auto& action : oldestHistory->redo) {
                    historyBytes -= action.estimatedBytes;
                }
                oldestHistory->redo.clear();
            } else {
                historyBytes -= oldestHistory->undo.front().estimatedBytes;
                oldestHistory->undo.pop_front();
            }
        }
    }

    void recordHistory(JournalDate date, std::optional<LoadedOutline> before,
                       std::optional<LoadedOutline> after) {
        auto& history = pageHistory(date);
        for (const auto& action : history.redo) {
            historyBytes -= action.estimatedBytes;
        }
        history.redo.clear();
        JournalHistoryAction action{nextHistorySequence++, date, std::move(before),
                                    std::move(after), 0};
        action.estimatedBytes = sizeof(JournalHistoryAction) + estimateOutlineBytes(action.before) +
                                estimateOutlineBytes(action.after);
        historyBytes += action.estimatedBytes;
        history.undo.push_back(std::move(action));
        enforceHistoryBudget();
    }

    auto removeTypeIndex(MDB_txn* transaction, MDB_dbi database, BlockType type,
                         const BlockId& id) const -> std::optional<NotebookError> {
        auto bytes = typeIndexKey(type, id);
        MDB_val key{bytes.size(), bytes.data()};
        const auto result = mdb_del(transaction, database, &key, nullptr);
        if (result != MDB_SUCCESS && result != MDB_NOTFOUND) {
            return errorFromLmdb(info.value_or(NotebookInfo{}).path, result,
                                 "remove Block type index");
        }
        return std::nullopt;
    }

    auto restoreOutline(MDB_txn* transaction, const JournalDatabases& databases, JournalDate date,
                        const std::optional<LoadedOutline>& target)
        -> std::optional<NotebookError> {
        const auto path = info.value_or(NotebookInfo{}).path;
        auto currentResult = loadOutlineForDate(transaction, databases, date);
        if (!currentResult) {
            return currentResult.error();
        }
        auto current = std::move(currentResult).value();
        if (current) {
            for (const auto& entry : current->entries) {
                if (auto error = eraseContainment(transaction, databases, entry)) {
                    return error;
                }
                auto key = blockKey(entry.metadata.id);
                auto result = mdb_del(transaction, databases.blocks, &key, nullptr);
                if (result != MDB_SUCCESS) {
                    return errorFromLmdb(path, result, "remove Journal Entry for history");
                }
                if (auto error = removeTypeIndex(transaction, databases.blocksByType,
                                                 BlockType::journalEntry, entry.metadata.id)) {
                    return error;
                }
            }
            auto pageKey = blockKey(current->page.metadata.id);
            auto result = mdb_del(transaction, databases.blocks, &pageKey, nullptr);
            if (result != MDB_SUCCESS) {
                return errorFromLmdb(path, result, "remove Journal Page for history");
            }
            if (auto error = removeTypeIndex(transaction, databases.blocksByType,
                                             BlockType::journalPage, current->page.metadata.id)) {
                return error;
            }
        }
        const auto encodedDate = dateKey(date);
        MDB_val dateKeyValue{encodedDate.size(), const_cast<std::uint8_t*>(encodedDate.data())};
        auto result = mdb_del(transaction, databases.journalByDate, &dateKeyValue, nullptr);
        if (result != MDB_SUCCESS && result != MDB_NOTFOUND) {
            return errorFromLmdb(path, result, "remove Journal date for history");
        }
        if (!target) {
            return std::nullopt;
        }
        if (auto error = writeBlock(transaction, databases.blocks, target->page, path)) {
            return error;
        }
        if (auto error = writeTypeIndex(transaction, databases.blocksByType, BlockType::journalPage,
                                        target->page.metadata.id, path)) {
            return error;
        }
        MDB_val pageValue{target->page.metadata.id.bytes.size(),
                          const_cast<std::byte*>(target->page.metadata.id.bytes.data())};
        result = mdb_put(transaction, databases.journalByDate, &dateKeyValue, &pageValue,
                         MDB_NOOVERWRITE);
        if (result != MDB_SUCCESS) {
            return errorFromLmdb(path, result, "restore Journal date for history");
        }
        for (const auto& entry : target->entries) {
            const BlockRecord block{BlockType::journalEntry, entry.metadata, std::nullopt,
                                    entry.authoredText};
            if (auto error = writeBlock(transaction, databases.blocks, block, path)) {
                return error;
            }
            if (auto error = writeTypeIndex(transaction, databases.blocksByType,
                                            BlockType::journalEntry, entry.metadata.id, path)) {
                return error;
            }
        }
        const LoadedOutline emptyBefore{target->page, {}};
        return rewriteContainment(transaction, databases, emptyBefore, *target);
    }

    auto applyHistory(JournalDate date, NotebookSession::JournalHistoryDirection direction)
        -> Result<JournalPage> {
        lastCommandCommitted = false;
        const auto redo = direction == NotebookSession::JournalHistoryDirection::redo;
        auto& history = pageHistory(date);
        auto& source = redo ? history.redo : history.undo;
        if (source.empty()) {
            return Result<JournalPage>::failure(makeError(
                redo ? NotebookErrorCode::redoUnavailable : NotebookErrorCode::undoUnavailable,
                info.value_or(NotebookInfo{}).path,
                redo ? "no Journal edit is available to redo"
                     : "no Journal edit is available to undo"));
        }
        const auto& action = source.back();
        const auto target = redo ? action.after : action.before;
        const auto path = info.value_or(NotebookInfo{}).path;
        MDB_txn* transaction = nullptr;
        auto result = mdb_txn_begin(environment, nullptr, 0, &transaction);
        if (result != MDB_SUCCESS) {
            return Result<JournalPage>::failure(
                errorFromLmdb(path, result, redo ? "begin Journal redo" : "begin Journal undo"));
        }
        const auto fail = [&](NotebookError error) -> Result<JournalPage> {
            mdb_txn_abort(transaction);
            return Result<JournalPage>::failure(std::move(error));
        };
        auto databases = openJournalDatabases(transaction, path);
        if (!databases) {
            return fail(databases.error());
        }
        if (auto error = restoreOutline(transaction, databases.value(), date, target)) {
            return fail(std::move(*error));
        }
        if (auto error = incrementRevision(transaction, databases.value().metadata, path)) {
            return fail(std::move(*error));
        }
        result = commitAdapter->commit(transaction);
        if (result != MDB_SUCCESS) {
            return Result<JournalPage>::failure(
                errorFromLmdb(path, result, redo ? "commit Journal redo" : "commit Journal undo"));
        }
        auto applied = std::move(source.back());
        source.pop_back();
        auto& destination = redo ? history.undo : history.redo;
        destination.push_back(std::move(applied));
        incrementCachedRevision();
        lastCommandCommitted = true;
        if (!target) {
            return Result<JournalPage>::success({date, std::nullopt, {}});
        }
        return Result<JournalPage>::success({date, target->page.metadata, target->entries});
    }

    auto readNestedJournalPage(JournalDate date) const -> Result<JournalPage> {
        const auto path = info.value_or(NotebookInfo{}).path;
        MDB_txn* transaction = nullptr;
        auto result = mdb_txn_begin(environment, nullptr, MDB_RDONLY, &transaction);
        if (result != MDB_SUCCESS) {
            return Result<JournalPage>::failure(errorFromLmdb(path, result, "begin Journal read"));
        }
        auto databases = openJournalDatabases(transaction, path);
        if (!databases) {
            mdb_txn_abort(transaction);
            return Result<JournalPage>::failure(databases.error());
        }
        const auto encodedDate = dateKey(date);
        MDB_val key{encodedDate.size(), const_cast<std::uint8_t*>(encodedDate.data())};
        MDB_val value{};
        result = mdb_get(transaction, databases.value().journalByDate, &key, &value);
        if (result == MDB_NOTFOUND) {
            mdb_txn_abort(transaction);
            return Result<JournalPage>::success({date, std::nullopt, {}});
        }
        if (result != MDB_SUCCESS || value.mv_size != BlockId{}.bytes.size()) {
            mdb_txn_abort(transaction);
            return Result<JournalPage>::failure(
                result == MDB_SUCCESS ? makeError(NotebookErrorCode::invalidNotebook, path,
                                                  "invalid Journal date index")
                                      : errorFromLmdb(path, result, "read Journal date index"));
        }
        BlockId pageId;
        std::memcpy(pageId.bytes.data(), value.mv_data, pageId.bytes.size());
        auto page = readBlock(transaction, databases.value().blocks, pageId, path);
        if (!page || page.value().type != BlockType::journalPage ||
            page.value().journalDate != date) {
            mdb_txn_abort(transaction);
            return Result<JournalPage>::failure(
                page ? makeError(NotebookErrorCode::invalidNotebook, path,
                                 "Journal date points to an invalid Page")
                     : page.error());
        }
        auto outline = loadOutline(transaction, databases.value(), std::move(page).value());
        mdb_txn_abort(transaction);
        if (!outline) {
            return Result<JournalPage>::failure(outline.error());
        }
        auto loaded = std::move(outline).value();
        return Result<JournalPage>::success(
            {date, loaded.page.metadata, std::move(loaded.entries)});
    }

    auto readJournalPage(JournalDate date) const -> Result<JournalPage> {
        const auto path = info.value_or(NotebookInfo{}).path;
        MDB_txn* transaction = nullptr;
        auto result = mdb_txn_begin(environment, nullptr, MDB_RDONLY, &transaction);
        if (result != MDB_SUCCESS) {
            return Result<JournalPage>::failure(errorFromLmdb(path, result, "begin Journal read"));
        }
        auto databases = openJournalDatabases(transaction, path);
        if (!databases) {
            mdb_txn_abort(transaction);
            return Result<JournalPage>::failure(databases.error());
        }

        const auto encodedDate = dateKey(date);
        MDB_val dateKeyValue{encodedDate.size(), const_cast<std::uint8_t*>(encodedDate.data())};
        MDB_val pageValue{};
        result = mdb_get(transaction, databases.value().journalByDate, &dateKeyValue, &pageValue);
        if (result == MDB_NOTFOUND) {
            mdb_txn_abort(transaction);
            return Result<JournalPage>::success(JournalPage{date, std::nullopt, {}});
        }
        if (result != MDB_SUCCESS || pageValue.mv_size != BlockId{}.bytes.size()) {
            mdb_txn_abort(transaction);
            return Result<JournalPage>::failure(
                result == MDB_SUCCESS ? makeError(NotebookErrorCode::invalidNotebook, path,
                                                  "invalid Journal date index")
                                      : errorFromLmdb(path, result, "read Journal date index"));
        }
        BlockId pageId;
        std::memcpy(pageId.bytes.data(), pageValue.mv_data, pageId.bytes.size());
        auto pageBlock = readBlock(transaction, databases.value().blocks, pageId, path);
        if (!pageBlock || pageBlock.value().type != BlockType::journalPage ||
            pageBlock.value().journalDate != date) {
            mdb_txn_abort(transaction);
            return Result<JournalPage>::failure(
                pageBlock ? makeError(NotebookErrorCode::invalidNotebook, path,
                                      "Journal date points to an invalid Page")
                          : pageBlock.error());
        }

        JournalPage page{date, pageBlock.value().metadata, {}};
        MDB_cursor* cursor = nullptr;
        result = mdb_cursor_open(transaction, databases.value().containmentByParent, &cursor);
        if (result != MDB_SUCCESS) {
            mdb_txn_abort(transaction);
            return Result<JournalPage>::failure(
                errorFromLmdb(path, result, "open Journal containment"));
        }
        auto start = containmentParentKey(pageId, 0);
        MDB_val containmentKey{start.size(), start.data()};
        MDB_val childValue{};
        result = mdb_cursor_get(cursor, &containmentKey, &childValue, MDB_SET_RANGE);
        while (result == MDB_SUCCESS) {
            if (containmentKey.mv_size != 24 ||
                std::memcmp(containmentKey.mv_data, pageId.bytes.data(), pageId.bytes.size()) !=
                    0) {
                break;
            }
            if (childValue.mv_size != BlockId{}.bytes.size()) {
                mdb_cursor_close(cursor);
                mdb_txn_abort(transaction);
                return Result<JournalPage>::failure(makeError(NotebookErrorCode::invalidNotebook,
                                                              path, "invalid Journal containment"));
            }
            BlockId entryId;
            std::memcpy(entryId.bytes.data(), childValue.mv_data, entryId.bytes.size());
            auto entryBlock = readBlock(transaction, databases.value().blocks, entryId, path);
            if (!entryBlock || entryBlock.value().type != BlockType::journalEntry) {
                mdb_cursor_close(cursor);
                mdb_txn_abort(transaction);
                return Result<JournalPage>::failure(
                    entryBlock ? makeError(NotebookErrorCode::invalidNotebook, path,
                                           "Journal Page contains a non-Entry Block")
                               : entryBlock.error());
            }
            page.entries.push_back(JournalEntry{entryBlock.value().metadata,
                                                entryBlock.value().authoredText, std::nullopt});
            result = mdb_cursor_get(cursor, &containmentKey, &childValue, MDB_NEXT);
        }
        mdb_cursor_close(cursor);
        mdb_txn_abort(transaction);
        if (result != MDB_SUCCESS && result != MDB_NOTFOUND) {
            return Result<JournalPage>::failure(
                errorFromLmdb(path, result, "read Journal containment"));
        }
        return Result<JournalPage>::success(std::move(page));
    }

    auto insertEntry(JournalDate date, std::optional<BlockId> afterEntry, std::string authoredText)
        -> Result<JournalPage> {
        lastCommandCommitted = false;
        const auto path = info.value_or(NotebookInfo{}).path;
        MDB_txn* transaction = nullptr;
        auto result = mdb_txn_begin(environment, nullptr, 0, &transaction);
        if (result != MDB_SUCCESS) {
            return Result<JournalPage>::failure(
                errorFromLmdb(path, result, "begin Journal update"));
        }
        const auto fail = [&](NotebookError error) -> Result<JournalPage> {
            mdb_txn_abort(transaction);
            return Result<JournalPage>::failure(std::move(error));
        };
        auto databases = openJournalDatabases(transaction, path);
        if (!databases) {
            return fail(databases.error());
        }
        const auto now = currentTimestamp();
        const auto encodedDate = dateKey(date);
        MDB_val dateIndexKey{encodedDate.size(), const_cast<std::uint8_t*>(encodedDate.data())};
        MDB_val pageValue{};
        result = mdb_get(transaction, databases.value().journalByDate, &dateIndexKey, &pageValue);
        BlockRecord pageBlock;
        if (result == MDB_NOTFOUND) {
            if (afterEntry) {
                return fail(makeError(NotebookErrorCode::invalidInsertionPoint, path,
                                      "insertion point is not on this Journal Page"));
            }
            pageBlock = BlockRecord{
                BlockType::journalPage, BlockMetadata{generateBlockId(), now, now}, date, {}};
            if (auto error = writeBlock(transaction, databases.value().blocks, pageBlock, path)) {
                return fail(std::move(*error));
            }
            if (auto error = writeTypeIndex(transaction, databases.value().blocksByType,
                                            pageBlock.type, pageBlock.metadata.id, path)) {
                return fail(std::move(*error));
            }
            MDB_val pageIdValue{pageBlock.metadata.id.bytes.size(),
                                pageBlock.metadata.id.bytes.data()};
            result = mdb_put(transaction, databases.value().journalByDate, &dateIndexKey,
                             &pageIdValue, MDB_NOOVERWRITE);
            if (result != MDB_SUCCESS) {
                return fail(errorFromLmdb(path, result, "index Journal date"));
            }
        } else {
            if (result != MDB_SUCCESS || pageValue.mv_size != BlockId{}.bytes.size()) {
                return fail(result == MDB_SUCCESS
                                ? makeError(NotebookErrorCode::invalidNotebook, path,
                                            "invalid Journal date index")
                                : errorFromLmdb(path, result, "read Journal date index"));
            }
            BlockId pageId;
            std::memcpy(pageId.bytes.data(), pageValue.mv_data, pageId.bytes.size());
            auto loaded = readBlock(transaction, databases.value().blocks, pageId, path);
            if (!loaded || loaded.value().type != BlockType::journalPage) {
                return fail(loaded ? makeError(NotebookErrorCode::invalidNotebook, path,
                                               "Journal date points to an invalid Page")
                                   : loaded.error());
            }
            pageBlock = loaded.value();
            pageBlock.metadata.updatedAt = now;
            if (auto error = writeBlock(transaction, databases.value().blocks, pageBlock, path)) {
                return fail(std::move(*error));
            }
        }

        std::vector<std::pair<BlockId, std::uint64_t>> siblings;
        MDB_cursor* cursor = nullptr;
        result = mdb_cursor_open(transaction, databases.value().containmentByParent, &cursor);
        if (result != MDB_SUCCESS) {
            return fail(errorFromLmdb(path, result, "open Journal containment"));
        }
        auto start = containmentParentKey(pageBlock.metadata.id, 0);
        MDB_val parentKey{start.size(), start.data()};
        MDB_val childValue{};
        result = mdb_cursor_get(cursor, &parentKey, &childValue, MDB_SET_RANGE);
        while (result == MDB_SUCCESS && parentKey.mv_size == 24 &&
               std::memcmp(parentKey.mv_data, pageBlock.metadata.id.bytes.data(),
                           pageBlock.metadata.id.bytes.size()) == 0) {
            if (childValue.mv_size != BlockId{}.bytes.size()) {
                mdb_cursor_close(cursor);
                return fail(makeError(NotebookErrorCode::invalidNotebook, path,
                                      "invalid Journal containment"));
            }
            BlockId child;
            std::memcpy(child.bytes.data(), childValue.mv_data, child.bytes.size());
            siblings.emplace_back(child, rankFromParentKey(parentKey));
            result = mdb_cursor_get(cursor, &parentKey, &childValue, MDB_NEXT);
        }
        mdb_cursor_close(cursor);
        if (result != MDB_SUCCESS && result != MDB_NOTFOUND) {
            return fail(errorFromLmdb(path, result, "read Journal containment"));
        }

        std::size_t insertionIndex = siblings.size();
        if (afterEntry) {
            const auto found = std::ranges::find_if(
                siblings, [&](const auto& item) -> bool { return item.first == *afterEntry; });
            if (found == siblings.end()) {
                return fail(makeError(NotebookErrorCode::invalidInsertionPoint, path,
                                      "insertion point is not on this Journal Page"));
            }
            insertionIndex = static_cast<std::size_t>(std::distance(siblings.begin(), found)) + 1;
        }
        constexpr std::uint64_t rankGap = 1ULL << 32U;
        std::uint64_t rank = rankGap;
        if (!siblings.empty()) {
            if (insertionIndex == siblings.size()) {
                rank = siblings.back().second + rankGap;
            } else {
                const auto lower = insertionIndex == 0 ? 0 : siblings[insertionIndex - 1].second;
                const auto upper = siblings[insertionIndex].second;
                rank = lower + ((upper - lower) / 2U);
            }
        }
        const auto needsRebalancing =
            (!siblings.empty() && insertionIndex == siblings.size() &&
             rank < siblings.back().second) ||
            (insertionIndex < siblings.size() &&
             rank == (insertionIndex == 0 ? 0 : siblings[insertionIndex - 1].second));
        if (needsRebalancing) {
            if (siblings.size() >= std::numeric_limits<std::uint64_t>::max() / rankGap) {
                return fail(makeError(NotebookErrorCode::ioFailure, path,
                                      "Journal ordering capacity is exhausted"));
            }
            for (const auto& [siblingId, oldRank] : siblings) {
                static_cast<void>(siblingId);
                auto oldKeyBytes = containmentParentKey(pageBlock.metadata.id, oldRank);
                MDB_val oldKey{oldKeyBytes.size(), oldKeyBytes.data()};
                result =
                    mdb_del(transaction, databases.value().containmentByParent, &oldKey, nullptr);
                if (result != MDB_SUCCESS) {
                    return fail(errorFromLmdb(path, result, "rebalance Journal ordering"));
                }
            }
            for (std::size_t index = 0; index < siblings.size(); ++index) {
                const auto rebalancedRank = (static_cast<std::uint64_t>(index) + 1U) * rankGap;
                siblings[index].second = rebalancedRank;
                auto parentBytes = containmentParentKey(pageBlock.metadata.id, rebalancedRank);
                MDB_val rebalancedParentKey{parentBytes.size(), parentBytes.data()};
                MDB_val siblingValue{siblings[index].first.bytes.size(),
                                     siblings[index].first.bytes.data()};
                result = mdb_put(transaction, databases.value().containmentByParent,
                                 &rebalancedParentKey, &siblingValue, MDB_NOOVERWRITE);
                if (result != MDB_SUCCESS) {
                    return fail(errorFromLmdb(path, result, "rebalance Journal ordering"));
                }
                auto siblingKey = blockKey(siblings[index].first);
                auto childIndex = containmentParentKey(pageBlock.metadata.id, rebalancedRank);
                MDB_val childIndexValue{childIndex.size(), childIndex.data()};
                result = mdb_put(transaction, databases.value().containmentByChild, &siblingKey,
                                 &childIndexValue, 0);
                if (result != MDB_SUCCESS) {
                    return fail(errorFromLmdb(path, result, "rebalance Journal parent index"));
                }
            }
            if (insertionIndex == siblings.size()) {
                rank = siblings.empty() ? rankGap : siblings.back().second + rankGap;
            } else {
                const auto lower = insertionIndex == 0 ? 0 : siblings[insertionIndex - 1].second;
                rank = lower + ((siblings[insertionIndex].second - lower) / 2U);
            }
        }

        BlockRecord entryBlock{BlockType::journalEntry, BlockMetadata{generateBlockId(), now, now},
                               std::nullopt, std::move(authoredText)};
        if (auto error = writeBlock(transaction, databases.value().blocks, entryBlock, path)) {
            return fail(std::move(*error));
        }
        if (auto error = writeTypeIndex(transaction, databases.value().blocksByType,
                                        entryBlock.type, entryBlock.metadata.id, path)) {
            return fail(std::move(*error));
        }
        auto encodedParent = containmentParentKey(pageBlock.metadata.id, rank);
        MDB_val newParentKey{encodedParent.size(), encodedParent.data()};
        MDB_val entryIdValue{entryBlock.metadata.id.bytes.size(),
                             entryBlock.metadata.id.bytes.data()};
        result = mdb_put(transaction, databases.value().containmentByParent, &newParentKey,
                         &entryIdValue, MDB_NOOVERWRITE);
        if (result != MDB_SUCCESS) {
            return fail(errorFromLmdb(path, result, "write Journal ordering"));
        }
        auto childKey = blockKey(entryBlock.metadata.id);
        std::array<std::uint8_t, 24> childIndex{};
        std::memcpy(childIndex.data(), pageBlock.metadata.id.bytes.data(),
                    pageBlock.metadata.id.bytes.size());
        for (std::size_t index = 0; index < 8; ++index) {
            childIndex[16 + index] = static_cast<std::uint8_t>(rank >> ((7U - index) * 8U));
        }
        MDB_val childIndexValue{childIndex.size(), childIndex.data()};
        result = mdb_put(transaction, databases.value().containmentByChild, &childKey,
                         &childIndexValue, MDB_NOOVERWRITE);
        if (result != MDB_SUCCESS) {
            return fail(errorFromLmdb(path, result, "write Journal parent index"));
        }

        JournalPage committedPage{date, pageBlock.metadata, {}};
        committedPage.entries.reserve(siblings.size() + 1);
        for (std::size_t index = 0; index <= siblings.size(); ++index) {
            if (index == insertionIndex) {
                committedPage.entries.push_back(
                    JournalEntry{entryBlock.metadata, entryBlock.authoredText, std::nullopt});
            }
            if (index < siblings.size()) {
                auto sibling =
                    readBlock(transaction, databases.value().blocks, siblings[index].first, path);
                if (!sibling || sibling.value().type != BlockType::journalEntry) {
                    return fail(sibling ? makeError(NotebookErrorCode::invalidNotebook, path,
                                                    "Journal Page contains a non-Entry Block")
                                        : sibling.error());
                }
                committedPage.entries.push_back(JournalEntry{
                    sibling.value().metadata, sibling.value().authoredText, std::nullopt});
            }
        }
        if (auto error = incrementRevision(transaction, databases.value().metadata, path)) {
            return fail(std::move(*error));
        }
        result = commitAdapter->commit(transaction);
        if (result != MDB_SUCCESS) {
            return Result<JournalPage>::failure(
                errorFromLmdb(path, result, "commit Journal Entry"));
        }
        incrementCachedRevision();
        lastCommandCommitted = true;
        return Result<JournalPage>::success(std::move(committedPage));
    }

    auto updateEntry(BlockId entryId, std::string authoredText) -> Result<JournalEntry> {
        lastCommandCommitted = false;
        const auto path = info.value_or(NotebookInfo{}).path;
        MDB_txn* transaction = nullptr;
        auto result = mdb_txn_begin(environment, nullptr, 0, &transaction);
        if (result != MDB_SUCCESS) {
            return Result<JournalEntry>::failure(errorFromLmdb(path, result, "begin Journal edit"));
        }
        const auto fail = [&](NotebookError error) -> Result<JournalEntry> {
            mdb_txn_abort(transaction);
            return Result<JournalEntry>::failure(std::move(error));
        };
        auto databases = openJournalDatabases(transaction, path);
        if (!databases) {
            return fail(databases.error());
        }
        auto outlineResult = loadOutlineForEntry(transaction, databases.value(), entryId);
        if (!outlineResult) {
            return fail(outlineResult.error());
        }
        const auto before = std::move(outlineResult).value();
        auto loaded = readBlock(transaction, databases.value().blocks, entryId, path);
        if (!loaded) {
            return fail(loaded.error());
        }
        if (loaded.value().type != BlockType::journalEntry) {
            return fail(
                makeError(NotebookErrorCode::blockNotFound, path, "Block is not a Journal Entry"));
        }
        auto entry = loaded.value();
        auto parentLink = parentOf(transaction, databases.value(), entryId);
        if (!parentLink) {
            return fail(parentLink.error());
        }
        auto parentBlock =
            readBlock(transaction, databases.value().blocks, parentLink.value().parent, path);
        if (!parentBlock) {
            return fail(parentBlock.error());
        }
        const auto parentEntry = parentBlock.value().type == BlockType::journalEntry
                                     ? std::optional<BlockId>{parentLink.value().parent}
                                     : std::nullopt;
        if (entry.authoredText == authoredText) {
            mdb_txn_abort(transaction);
            return Result<JournalEntry>::success(
                JournalEntry{entry.metadata, entry.authoredText, parentEntry});
        }
        entry.authoredText = std::move(authoredText);
        entry.metadata.updatedAt = currentTimestamp();
        if (auto error = writeBlock(transaction, databases.value().blocks, entry, path)) {
            return fail(std::move(*error));
        }
        if (auto error = incrementRevision(transaction, databases.value().metadata, path)) {
            return fail(std::move(*error));
        }
        result = commitAdapter->commit(transaction);
        if (result != MDB_SUCCESS) {
            return Result<JournalEntry>::failure(
                errorFromLmdb(path, result, "commit Journal edit"));
        }
        incrementCachedRevision();
        lastCommandCommitted = true;
        auto after = before;
        const auto changed = std::ranges::find_if(
            after.entries, [&](const auto& candidate) { return candidate.metadata.id == entryId; });
        changed->metadata = entry.metadata;
        changed->authoredText = entry.authoredText;
        recordHistory(before.page.journalDate.value_or(JournalDate{}), before, std::move(after));
        return Result<JournalEntry>::success(
            JournalEntry{entry.metadata, entry.authoredText, parentEntry});
    }

    auto insertNestedEntry(JournalDate date, std::optional<BlockId> afterEntry,
                           std::string authoredText) -> Result<JournalPage> {
        lastCommandCommitted = false;
        const auto path = info.value_or(NotebookInfo{}).path;
        MDB_txn* transaction = nullptr;
        auto result = mdb_txn_begin(environment, nullptr, 0, &transaction);
        if (result != MDB_SUCCESS) {
            return Result<JournalPage>::failure(
                errorFromLmdb(path, result, "begin Journal insertion"));
        }
        const auto fail = [&](NotebookError error) -> Result<JournalPage> {
            mdb_txn_abort(transaction);
            return Result<JournalPage>::failure(std::move(error));
        };
        auto databases = openJournalDatabases(transaction, path);
        if (!databases) {
            return fail(databases.error());
        }
        const auto now = currentTimestamp();
        const auto encodedDate = dateKey(date);
        MDB_val dateIndexKey{encodedDate.size(), const_cast<std::uint8_t*>(encodedDate.data())};
        MDB_val pageValue{};
        result = mdb_get(transaction, databases.value().journalByDate, &dateIndexKey, &pageValue);
        const auto pageWasCreated = result == MDB_NOTFOUND;
        BlockRecord page;
        if (result == MDB_NOTFOUND) {
            if (afterEntry) {
                return fail(makeError(NotebookErrorCode::invalidInsertionPoint, path,
                                      "insertion point is not on this Journal Page"));
            }
            page = {BlockType::journalPage, BlockMetadata{generateBlockId(), now, now}, date, {}};
            if (auto error = writeBlock(transaction, databases.value().blocks, page, path)) {
                return fail(std::move(*error));
            }
            if (auto error = writeTypeIndex(transaction, databases.value().blocksByType, page.type,
                                            page.metadata.id, path)) {
                return fail(std::move(*error));
            }
            MDB_val pageIdValue{page.metadata.id.bytes.size(), page.metadata.id.bytes.data()};
            result = mdb_put(transaction, databases.value().journalByDate, &dateIndexKey,
                             &pageIdValue, MDB_NOOVERWRITE);
            if (result != MDB_SUCCESS) {
                return fail(errorFromLmdb(path, result, "index Journal date"));
            }
        } else {
            if (result != MDB_SUCCESS || pageValue.mv_size != BlockId{}.bytes.size()) {
                return fail(result == MDB_SUCCESS
                                ? makeError(NotebookErrorCode::invalidNotebook, path,
                                            "invalid Journal date index")
                                : errorFromLmdb(path, result, "read Journal date index"));
            }
            BlockId pageId;
            std::memcpy(pageId.bytes.data(), pageValue.mv_data, pageId.bytes.size());
            auto loaded = readBlock(transaction, databases.value().blocks, pageId, path);
            if (!loaded || loaded.value().type != BlockType::journalPage ||
                loaded.value().journalDate != date) {
                return fail(loaded ? makeError(NotebookErrorCode::invalidNotebook, path,
                                               "Journal date points to an invalid Page")
                                   : loaded.error());
            }
            page = std::move(loaded).value();
        }
        auto loadedOutline = loadOutline(transaction, databases.value(), std::move(page));
        if (!loadedOutline) {
            return fail(loadedOutline.error());
        }
        auto outline = std::move(loadedOutline).value();
        const auto before = outline;
        std::optional<BlockId> parent;
        auto insertionIndex = outline.entries.size();
        if (afterEntry) {
            const auto found =
                std::ranges::find_if(outline.entries, [&](const auto& entry) -> bool {
                    return entry.metadata.id == *afterEntry;
                });
            if (found == outline.entries.end()) {
                return fail(makeError(NotebookErrorCode::invalidInsertionPoint, path,
                                      "insertion point is not on this Journal Page"));
            }
            const auto index =
                static_cast<std::size_t>(std::distance(outline.entries.begin(), found));
            parent = found->parentEntry;
            insertionIndex = subtreeEnd(outline.entries, index);
        }
        BlockRecord entry{BlockType::journalEntry, BlockMetadata{generateBlockId(), now, now},
                          std::nullopt, std::move(authoredText)};
        if (auto error = writeBlock(transaction, databases.value().blocks, entry, path)) {
            return fail(std::move(*error));
        }
        if (auto error = writeTypeIndex(transaction, databases.value().blocksByType, entry.type,
                                        entry.metadata.id, path)) {
            return fail(std::move(*error));
        }
        outline.entries.insert(outline.entries.begin() +
                                   static_cast<std::ptrdiff_t>(insertionIndex),
                               {entry.metadata, entry.authoredText, parent});
        if (auto error = touchContainer(transaction, databases.value(), outline, parent, now)) {
            return fail(std::move(*error));
        }
        if (auto error = rewriteContainment(transaction, databases.value(), before, outline)) {
            return fail(std::move(*error));
        }
        if (auto error = incrementRevision(transaction, databases.value().metadata, path)) {
            return fail(std::move(*error));
        }
        result = commitAdapter->commit(transaction);
        if (result != MDB_SUCCESS) {
            return Result<JournalPage>::failure(
                errorFromLmdb(path, result, "commit Journal insertion"));
        }
        incrementCachedRevision();
        lastCommandCommitted = true;
        recordHistory(date, pageWasCreated ? std::nullopt : std::optional<LoadedOutline>{before},
                      outline);
        return Result<JournalPage>::success(
            {date, outline.page.metadata, std::move(outline.entries)});
    }

    auto editOutline(BlockId entryId, OutlineEditKind edit, std::size_t cursorByteOffset = 0,
                     std::string editedText = {}) -> Result<JournalPage> {
        lastCommandCommitted = false;
        const auto path = info.value_or(NotebookInfo{}).path;
        MDB_txn* transaction = nullptr;
        auto result = mdb_txn_begin(environment, nullptr, 0, &transaction);
        if (result != MDB_SUCCESS) {
            return Result<JournalPage>::failure(
                errorFromLmdb(path, result, "begin Journal structural edit"));
        }
        const auto fail = [&](NotebookError error) -> Result<JournalPage> {
            mdb_txn_abort(transaction);
            return Result<JournalPage>::failure(std::move(error));
        };
        auto databases = openJournalDatabases(transaction, path);
        if (!databases) {
            return fail(databases.error());
        }
        auto loaded = loadOutlineForEntry(transaction, databases.value(), entryId);
        if (!loaded) {
            return fail(loaded.error());
        }
        auto outline = std::move(loaded).value();
        const auto before = outline;
        auto found = std::ranges::find_if(outline.entries, [&](const auto& entry) -> bool {
            return entry.metadata.id == entryId;
        });
        if (found == outline.entries.end()) {
            return fail(makeError(NotebookErrorCode::blockNotFound, path,
                                  "Journal Entry is not on its Page"));
        }
        auto index = static_cast<std::size_t>(std::distance(outline.entries.begin(), found));
        const auto originalParent = found->parentEntry;
        const auto hasChildren =
            std::ranges::any_of(outline.entries, [&](const auto& entry) -> bool {
                return entry.parentEntry == entryId;
            });
        const auto now = currentTimestamp();
        std::vector<std::optional<BlockId>> touchedContainers;
        auto touch = [&](std::optional<BlockId> parent) -> void {
            if (std::ranges::find(touchedContainers, parent) == touchedContainers.end()) {
                touchedContainers.push_back(parent);
            }
        };
        bool deleteOriginal = false;
        std::optional<BlockRecord> blockToWrite;
        std::optional<BlockRecord> blockToCreate;

        if (edit == OutlineEditKind::split) {
            if (cursorByteOffset > editedText.size() ||
                (cursorByteOffset < editedText.size() &&
                 (static_cast<unsigned char>(editedText[cursorByteOffset]) & 0xC0U) == 0x80U)) {
                return fail(makeError(NotebookErrorCode::invalidCursorPosition, path,
                                      "split cursor is not on a Unicode boundary"));
            }
            auto original = readBlock(transaction, databases.value().blocks, entryId, path);
            if (!original) {
                return fail(original.error());
            }
            auto updated = std::move(original).value();
            const auto suffix = editedText.substr(cursorByteOffset);
            updated.authoredText = editedText.substr(0, cursorByteOffset);
            updated.metadata.updatedAt = now;
            found->authoredText = updated.authoredText;
            found->metadata.updatedAt = now;
            blockToWrite = updated;
            BlockRecord created{BlockType::journalEntry, BlockMetadata{generateBlockId(), now, now},
                                std::nullopt, suffix};
            blockToCreate = created;
            const auto insertion = subtreeEnd(outline.entries, index);
            outline.entries.insert(outline.entries.begin() + static_cast<std::ptrdiff_t>(insertion),
                                   {created.metadata, created.authoredText, originalParent});
            touch(originalParent);
        } else if (edit == OutlineEditKind::join) {
            if (hasChildren) {
                return fail(makeError(NotebookErrorCode::blockHasChildren, path,
                                      "an Entry with children cannot be joined"));
            }
            if (index == 0) {
                return fail(makeError(NotebookErrorCode::invalidStructuralMove, path,
                                      "the first Entry has no previous visible Entry"));
            }
            auto& target = outline.entries[index - 1];
            const auto combined = target.authoredText + editedText;
            if (!validAuthoredText(combined)) {
                return fail(makeError(NotebookErrorCode::invalidAuthoredText, path,
                                      "joined Journal Entry text is too large"));
            }
            auto targetBlock =
                readBlock(transaction, databases.value().blocks, target.metadata.id, path);
            if (!targetBlock) {
                return fail(targetBlock.error());
            }
            auto updated = std::move(targetBlock).value();
            updated.authoredText = combined;
            updated.metadata.updatedAt = now;
            target.authoredText = combined;
            target.metadata.updatedAt = now;
            blockToWrite = updated;
            outline.entries.erase(outline.entries.begin() + static_cast<std::ptrdiff_t>(index));
            deleteOriginal = true;
            touch(originalParent);
        } else if (edit == OutlineEditKind::erase) {
            if (hasChildren) {
                return fail(makeError(NotebookErrorCode::blockHasChildren, path,
                                      "an Entry with children cannot be deleted"));
            }
            outline.entries.erase(outline.entries.begin() + static_cast<std::ptrdiff_t>(index));
            deleteOriginal = true;
            touch(originalParent);
        } else if (edit == OutlineEditKind::indent) {
            std::optional<std::size_t> previousSibling;
            for (std::size_t candidate = 0; candidate < index; ++candidate) {
                if (outline.entries[candidate].parentEntry == originalParent) {
                    previousSibling = candidate;
                }
            }
            if (!previousSibling) {
                return fail(makeError(NotebookErrorCode::invalidStructuralMove, path,
                                      "the first sibling cannot be indented"));
            }
            found->parentEntry = outline.entries[*previousSibling].metadata.id;
            touch(originalParent);
            touch(found->parentEntry);
        } else if (edit == OutlineEditKind::outdent) {
            if (!originalParent) {
                return fail(makeError(NotebookErrorCode::invalidStructuralMove, path,
                                      "a top-level Entry cannot be outdented"));
            }
            const auto parent =
                std::ranges::find_if(outline.entries, [&](const auto& entry) -> bool {
                    return entry.metadata.id == *originalParent;
                });
            if (parent == outline.entries.end()) {
                return fail(makeError(NotebookErrorCode::invalidNotebook, path,
                                      "Journal parent is outside its Page"));
            }
            const auto parentParent = parent->parentEntry;
            const auto end = subtreeEnd(outline.entries, index);
            std::vector<JournalEntry> moving(
                outline.entries.begin() + static_cast<std::ptrdiff_t>(index),
                outline.entries.begin() + static_cast<std::ptrdiff_t>(end));
            moving.front().parentEntry = parentParent;
            outline.entries.erase(outline.entries.begin() + static_cast<std::ptrdiff_t>(index),
                                  outline.entries.begin() + static_cast<std::ptrdiff_t>(end));
            const auto parentAfterErase = static_cast<std::size_t>(
                std::distance(outline.entries.begin(),
                              std::ranges::find_if(outline.entries, [&](const auto& entry) -> bool {
                                  return entry.metadata.id == *originalParent;
                              })));
            const auto insertion = subtreeEnd(outline.entries, parentAfterErase);
            outline.entries.insert(outline.entries.begin() + static_cast<std::ptrdiff_t>(insertion),
                                   moving.begin(), moving.end());
            touch(originalParent);
            touch(parentParent);
        } else {
            const auto end = subtreeEnd(outline.entries, index);
            if (edit == OutlineEditKind::up) {
                std::optional<std::size_t> previousSibling;
                for (std::size_t candidate = 0; candidate < index; ++candidate) {
                    if (outline.entries[candidate].parentEntry == originalParent) {
                        previousSibling = candidate;
                    }
                }
                if (!previousSibling) {
                    return fail(makeError(NotebookErrorCode::invalidStructuralMove, path,
                                          "the first sibling cannot move up"));
                }
                std::rotate(outline.entries.begin() + static_cast<std::ptrdiff_t>(*previousSibling),
                            outline.entries.begin() + static_cast<std::ptrdiff_t>(index),
                            outline.entries.begin() + static_cast<std::ptrdiff_t>(end));
            } else {
                if (end >= outline.entries.size() ||
                    outline.entries[end].parentEntry != originalParent) {
                    return fail(makeError(NotebookErrorCode::invalidStructuralMove, path,
                                          "the last sibling cannot move down"));
                }
                const auto nextEnd = subtreeEnd(outline.entries, end);
                std::rotate(outline.entries.begin() + static_cast<std::ptrdiff_t>(index),
                            outline.entries.begin() + static_cast<std::ptrdiff_t>(end),
                            outline.entries.begin() + static_cast<std::ptrdiff_t>(nextEnd));
            }
            touch(originalParent);
        }

        if (edit == OutlineEditKind::indent || edit == OutlineEditKind::outdent ||
            edit == OutlineEditKind::up || edit == OutlineEditKind::down) {
            auto moved = readBlock(transaction, databases.value().blocks, entryId, path);
            if (!moved) {
                return fail(moved.error());
            }
            auto updated = std::move(moved).value();
            updated.authoredText = editedText;
            updated.metadata.updatedAt = now;
            blockToWrite = updated;
            const auto movedEntry =
                std::ranges::find_if(outline.entries, [&](const auto& entry) -> bool {
                    return entry.metadata.id == entryId;
                });
            movedEntry->metadata.updatedAt = now;
            movedEntry->authoredText = editedText;
        }
        if (blockToWrite) {
            if (auto error =
                    writeBlock(transaction, databases.value().blocks, *blockToWrite, path)) {
                return fail(std::move(*error));
            }
        }
        if (blockToCreate) {
            if (auto error =
                    writeBlock(transaction, databases.value().blocks, *blockToCreate, path)) {
                return fail(std::move(*error));
            }
            if (auto error =
                    writeTypeIndex(transaction, databases.value().blocksByType, blockToCreate->type,
                                   blockToCreate->metadata.id, path)) {
                return fail(std::move(*error));
            }
        }
        for (const auto& container : touchedContainers) {
            if (auto error =
                    touchContainer(transaction, databases.value(), outline, container, now)) {
                return fail(std::move(*error));
            }
        }
        if (auto error = rewriteContainment(transaction, databases.value(), before, outline)) {
            return fail(std::move(*error));
        }
        if (deleteOriginal) {
            auto blockKeyValue = blockKey(entryId);
            result = mdb_del(transaction, databases.value().blocks, &blockKeyValue, nullptr);
            if (result != MDB_SUCCESS) {
                return fail(errorFromLmdb(path, result, "delete Journal Entry"));
            }
            auto typeKeyBytes = typeIndexKey(BlockType::journalEntry, entryId);
            MDB_val typeKey{typeKeyBytes.size(), typeKeyBytes.data()};
            result = mdb_del(transaction, databases.value().blocksByType, &typeKey, nullptr);
            if (result != MDB_SUCCESS) {
                return fail(errorFromLmdb(path, result, "delete Journal Entry type index"));
            }
        }
        if (auto error = incrementRevision(transaction, databases.value().metadata, path)) {
            return fail(std::move(*error));
        }
        result = commitAdapter->commit(transaction);
        if (result != MDB_SUCCESS) {
            return Result<JournalPage>::failure(
                errorFromLmdb(path, result, "commit Journal structural edit"));
        }
        incrementCachedRevision();
        lastCommandCommitted = true;
        recordHistory(outline.page.journalDate.value_or(JournalDate{}), before, outline);
        return Result<JournalPage>::success({outline.page.journalDate.value_or(JournalDate{}),
                                             outline.page.metadata, std::move(outline.entries)});
    }

    auto deleteSubtrees(const std::vector<BlockId>& entryIds) -> Result<JournalPage> {
        lastCommandCommitted = false;
        const auto path = info.value_or(NotebookInfo{}).path;
        if (entryIds.empty()) {
            return Result<JournalPage>::failure(
                makeError(NotebookErrorCode::invalidStructuralMove, path,
                          "at least one Journal subtree must be selected for deletion"));
        }
        MDB_txn* transaction = nullptr;
        auto result = mdb_txn_begin(environment, nullptr, 0, &transaction);
        if (result != MDB_SUCCESS) {
            return Result<JournalPage>::failure(
                errorFromLmdb(path, result, "begin Journal subtree deletion"));
        }
        const auto fail = [&](NotebookError error) -> Result<JournalPage> {
            mdb_txn_abort(transaction);
            return Result<JournalPage>::failure(std::move(error));
        };
        auto databases = openJournalDatabases(transaction, path);
        if (!databases) {
            return fail(databases.error());
        }
        auto loaded = loadOutlineForEntry(transaction, databases.value(), entryIds.front());
        if (!loaded) {
            return fail(loaded.error());
        }
        auto outline = std::move(loaded).value();
        const auto before = outline;
        const auto isRequested = [&](const BlockId& id) -> bool {
            return std::ranges::find(entryIds, id) != entryIds.end();
        };
        for (const auto& id : entryIds) {
            if (std::ranges::none_of(outline.entries, [&](const auto& entry) -> bool {
                    return entry.metadata.id == id;
                })) {
                return fail(makeError(NotebookErrorCode::blockNotFound, path,
                                      "selected Journal Entry is not on the same Page"));
            }
        }

        std::vector<BlockId> roots;
        for (const auto& entry : outline.entries) {
            if (!isRequested(entry.metadata.id)) {
                continue;
            }
            auto ancestor = entry.parentEntry;
            bool coveredByAncestor = false;
            while (ancestor) {
                if (isRequested(*ancestor)) {
                    coveredByAncestor = true;
                    break;
                }
                const auto found =
                    std::ranges::find_if(outline.entries, [&](const auto& candidate) -> bool {
                        return candidate.metadata.id == *ancestor;
                    });
                ancestor = found == outline.entries.end() ? std::nullopt : found->parentEntry;
            }
            if (!coveredByAncestor) {
                roots.push_back(entry.metadata.id);
            }
        }
        const auto isDeleted = [&](const JournalEntry& entry) -> bool {
            if (std::ranges::find(roots, entry.metadata.id) != roots.end()) {
                return true;
            }
            auto ancestor = entry.parentEntry;
            while (ancestor) {
                if (std::ranges::find(roots, *ancestor) != roots.end()) {
                    return true;
                }
                const auto found =
                    std::ranges::find_if(before.entries, [&](const auto& candidate) -> bool {
                        return candidate.metadata.id == *ancestor;
                    });
                ancestor = found == before.entries.end() ? std::nullopt : found->parentEntry;
            }
            return false;
        };

        const auto now = currentTimestamp();
        std::vector<std::optional<BlockId>> touchedContainers;
        for (const auto& entry : before.entries) {
            if (std::ranges::find(roots, entry.metadata.id) == roots.end()) {
                continue;
            }
            if (std::ranges::find(touchedContainers, entry.parentEntry) ==
                touchedContainers.end()) {
                touchedContainers.push_back(entry.parentEntry);
            }
        }
        std::erase_if(outline.entries, isDeleted);
        for (const auto& parent : touchedContainers) {
            if (auto error = touchContainer(transaction, databases.value(), outline, parent, now)) {
                return fail(std::move(*error));
            }
        }
        if (auto error = rewriteContainment(transaction, databases.value(), before, outline)) {
            return fail(std::move(*error));
        }
        for (const auto& entry : before.entries) {
            if (!isDeleted(entry)) {
                continue;
            }
            auto key = blockKey(entry.metadata.id);
            result = mdb_del(transaction, databases.value().blocks, &key, nullptr);
            if (result != MDB_SUCCESS) {
                return fail(errorFromLmdb(path, result, "delete selected Journal Entry"));
            }
            if (auto error = removeTypeIndex(transaction, databases.value().blocksByType,
                                             BlockType::journalEntry, entry.metadata.id)) {
                return fail(std::move(*error));
            }
        }
        if (auto error = incrementRevision(transaction, databases.value().metadata, path)) {
            return fail(std::move(*error));
        }
        result = commitAdapter->commit(transaction);
        if (result != MDB_SUCCESS) {
            return Result<JournalPage>::failure(
                errorFromLmdb(path, result, "commit Journal subtree deletion"));
        }
        incrementCachedRevision();
        lastCommandCommitted = true;
        recordHistory(outline.page.journalDate.value_or(JournalDate{}), before, outline);
        return Result<JournalPage>::success({outline.page.journalDate.value_or(JournalDate{}),
                                             outline.page.metadata, std::move(outline.entries)});
    }

    void incrementCachedRevision() {
        auto updated = info.value_or(NotebookInfo{});
        ++updated.revision;
        info = std::move(updated);
    }

    [[nodiscard]] auto committedCallbacks() const -> std::vector<std::function<void()>> {
        std::scoped_lock lock(subscriptions->mutex);
        std::vector<std::function<void()>> callbacks;
        callbacks.reserve(subscriptions->callbacks.size());
        for (const auto& item : subscriptions->callbacks) {
            callbacks.push_back(item.second);
        }
        return callbacks;
    }

    mutable std::mutex mutex;
    MDB_env* environment{nullptr};
    std::optional<platform::ExclusiveFileLock> lockFile;
    std::optional<platform::ExclusiveFileLock> dataLockFile;
    std::optional<NotebookInfo> info;
    std::unique_ptr<JournalCommitAdapter> commitAdapter;
    std::shared_ptr<SubscriptionState> subscriptions;
    bool lastCommandCommitted{false};
    std::vector<JournalPageHistory> journalHistory;
    std::size_t historyBytes{0};
    std::uint64_t nextHistorySequence{1};
};

#ifdef HIEDA_TESTING
void NotebookSessionTestAccess::rejectNextJournalCommit(NotebookSession& session) {
    std::scoped_lock lock(session.impl_->mutex);
    session.impl_->commitAdapter = std::make_unique<RejectNextJournalCommitAdapter>();
}
#endif

auto NotebookId::toString() const -> std::string {
    return formatUuid(*this);
}

auto BlockId::toString() const -> std::string {
    return formatUuid(*this);
}

NotebookSubscription::NotebookSubscription() = default;
NotebookSubscription::~NotebookSubscription() = default;
NotebookSubscription::NotebookSubscription(NotebookSubscription&&) noexcept = default;
auto NotebookSubscription::operator=(NotebookSubscription&&) noexcept
    -> NotebookSubscription& = default;
NotebookSubscription::NotebookSubscription(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

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

auto NotebookSession::journalPage(JournalDate date) const -> Result<JournalPage> {
    std::scoped_lock lock(impl_->mutex);
    if (!impl_->info) {
        return Result<JournalPage>::failure(
            makeError(NotebookErrorCode::notebookNotOpen, {}, "a Notebook must be open"));
    }
    if (!validJournalDate(date)) {
        return Result<JournalPage>::failure(makeError(NotebookErrorCode::invalidJournalDate,
                                                      impl_->info->path, "invalid Journal date"));
    }
    return impl_->readNestedJournalPage(date);
}

auto NotebookSession::insertJournalEntry(JournalDate date, std::optional<BlockId> afterEntry,
                                         std::string authoredText) -> Result<JournalPage> {
    std::vector<std::function<void()>> callbacks;
    auto result = [&]() -> Result<JournalPage> {
        std::scoped_lock lock(impl_->mutex);
        if (!impl_->info) {
            return Result<JournalPage>::failure(
                makeError(NotebookErrorCode::notebookNotOpen, {}, "a Notebook must be open"));
        }
        if (!validJournalDate(date)) {
            return Result<JournalPage>::failure(makeError(
                NotebookErrorCode::invalidJournalDate, impl_->info->path, "invalid Journal date"));
        }
        if (!validAuthoredText(authoredText)) {
            return Result<JournalPage>::failure(
                makeError(NotebookErrorCode::invalidAuthoredText, impl_->info->path,
                          "Journal Entry text must be bounded Unicode text using LF line breaks"));
        }
        auto outcome = impl_->insertNestedEntry(date, afterEntry, std::move(authoredText));
        if (impl_->lastCommandCommitted) {
            callbacks = impl_->committedCallbacks();
        }
        return outcome;
    }();
    notifyCallbacks(callbacks);
    return result;
}

auto NotebookSession::updateJournalEntry(BlockId entryId, std::string authoredText)
    -> Result<JournalEntry> {
    std::vector<std::function<void()>> callbacks;
    auto result = [&]() -> Result<JournalEntry> {
        std::scoped_lock lock(impl_->mutex);
        if (!impl_->info) {
            return Result<JournalEntry>::failure(
                makeError(NotebookErrorCode::notebookNotOpen, {}, "a Notebook must be open"));
        }
        if (!validAuthoredText(authoredText)) {
            return Result<JournalEntry>::failure(
                makeError(NotebookErrorCode::invalidAuthoredText, impl_->info->path,
                          "Journal Entry text must be bounded Unicode text using LF line breaks"));
        }
        auto outcome = impl_->updateEntry(entryId, std::move(authoredText));
        if (impl_->lastCommandCommitted) {
            callbacks = impl_->committedCallbacks();
        }
        return outcome;
    }();
    notifyCallbacks(callbacks);
    return result;
}

auto NotebookSession::splitJournalEntry(BlockId entryId, std::string authoredText,
                                        std::size_t cursorByteOffset) -> Result<JournalPage> {
    std::vector<std::function<void()>> callbacks;
    auto result = [&]() -> Result<JournalPage> {
        std::scoped_lock lock(impl_->mutex);
        if (!impl_->info) {
            return Result<JournalPage>::failure(
                makeError(NotebookErrorCode::notebookNotOpen, {}, "a Notebook must be open"));
        }
        if (!validAuthoredText(authoredText)) {
            return Result<JournalPage>::failure(
                makeError(NotebookErrorCode::invalidAuthoredText, impl_->info->path,
                          "Journal Entry text must be bounded Unicode text using LF line breaks"));
        }
        auto outcome = impl_->editOutline(entryId, OutlineEditKind::split, cursorByteOffset,
                                          std::move(authoredText));
        if (impl_->lastCommandCommitted) {
            callbacks = impl_->committedCallbacks();
        }
        return outcome;
    }();
    notifyCallbacks(callbacks);
    return result;
}

auto NotebookSession::joinJournalEntry(BlockId entryId, std::string authoredText)
    -> Result<JournalPage> {
    std::vector<std::function<void()>> callbacks;
    auto result = [&]() -> Result<JournalPage> {
        std::scoped_lock lock(impl_->mutex);
        if (!impl_->info) {
            return Result<JournalPage>::failure(
                makeError(NotebookErrorCode::notebookNotOpen, {}, "a Notebook must be open"));
        }
        if (!validAuthoredText(authoredText)) {
            return Result<JournalPage>::failure(
                makeError(NotebookErrorCode::invalidAuthoredText, impl_->info->path,
                          "Journal Entry text must be bounded Unicode text using LF line breaks"));
        }
        auto outcome =
            impl_->editOutline(entryId, OutlineEditKind::join, 0, std::move(authoredText));
        if (impl_->lastCommandCommitted) {
            callbacks = impl_->committedCallbacks();
        }
        return outcome;
    }();
    notifyCallbacks(callbacks);
    return result;
}

auto NotebookSession::moveJournalEntry(BlockId entryId, JournalEntryMove movement,
                                       std::string authoredText) -> Result<JournalPage> {
    auto edit = OutlineEditKind::indent;
    switch (movement) {
    case JournalEntryMove::indent:
        edit = OutlineEditKind::indent;
        break;
    case JournalEntryMove::outdent:
        edit = OutlineEditKind::outdent;
        break;
    case JournalEntryMove::up:
        edit = OutlineEditKind::up;
        break;
    case JournalEntryMove::down:
        edit = OutlineEditKind::down;
        break;
    }
    std::vector<std::function<void()>> callbacks;
    auto result = [&]() -> Result<JournalPage> {
        std::scoped_lock lock(impl_->mutex);
        if (!impl_->info) {
            return Result<JournalPage>::failure(
                makeError(NotebookErrorCode::notebookNotOpen, {}, "a Notebook must be open"));
        }
        if (!validAuthoredText(authoredText)) {
            return Result<JournalPage>::failure(
                makeError(NotebookErrorCode::invalidAuthoredText, impl_->info->path,
                          "Journal Entry text must be bounded Unicode text using LF line breaks"));
        }
        auto outcome = impl_->editOutline(entryId, edit, 0, std::move(authoredText));
        if (impl_->lastCommandCommitted) {
            callbacks = impl_->committedCallbacks();
        }
        return outcome;
    }();
    notifyCallbacks(callbacks);
    return result;
}

auto NotebookSession::deleteJournalEntry(BlockId entryId) -> Result<JournalPage> {
    std::vector<std::function<void()>> callbacks;
    auto result = [&]() -> Result<JournalPage> {
        std::scoped_lock lock(impl_->mutex);
        if (!impl_->info) {
            return Result<JournalPage>::failure(
                makeError(NotebookErrorCode::notebookNotOpen, {}, "a Notebook must be open"));
        }
        auto outcome = impl_->editOutline(entryId, OutlineEditKind::erase);
        if (impl_->lastCommandCommitted) {
            callbacks = impl_->committedCallbacks();
        }
        return outcome;
    }();
    notifyCallbacks(callbacks);
    return result;
}

auto NotebookSession::deleteJournalSubtrees(std::vector<BlockId> entryIds) -> Result<JournalPage> {
    std::vector<std::function<void()>> callbacks;
    auto result = [&]() -> Result<JournalPage> {
        std::scoped_lock lock(impl_->mutex);
        if (!impl_->info) {
            return Result<JournalPage>::failure(
                makeError(NotebookErrorCode::notebookNotOpen, {}, "a Notebook must be open"));
        }
        auto outcome = impl_->deleteSubtrees(entryIds);
        if (impl_->lastCommandCommitted) {
            callbacks = impl_->committedCallbacks();
        }
        return outcome;
    }();
    notifyCallbacks(callbacks);
    return result;
}

auto NotebookSession::journalEditCapabilities(JournalDate date) const
    -> Result<JournalEditCapabilities> {
    std::scoped_lock lock(impl_->mutex);
    if (!impl_->info) {
        return Result<JournalEditCapabilities>::failure(
            makeError(NotebookErrorCode::notebookNotOpen, {}, "a Notebook must be open"));
    }
    if (!validJournalDate(date)) {
        return Result<JournalEditCapabilities>::failure(makeError(
            NotebookErrorCode::invalidJournalDate, impl_->info->path, "invalid Journal date"));
    }
    const auto& implementation = std::as_const(*impl_);
    const auto* history = implementation.pageHistory(date);
    return Result<JournalEditCapabilities>::success({history != nullptr && !history->undo.empty(),
                                                     history != nullptr && !history->redo.empty()});
}

auto NotebookSession::undoJournalEdit(JournalDate date) -> Result<JournalPage> {
    return applyJournalHistory(date, JournalHistoryDirection::undo);
}

auto NotebookSession::redoJournalEdit(JournalDate date) -> Result<JournalPage> {
    return applyJournalHistory(date, JournalHistoryDirection::redo);
}

auto NotebookSession::applyJournalHistory(JournalDate date, JournalHistoryDirection direction)
    -> Result<JournalPage> {
    std::vector<std::function<void()>> callbacks;
    auto result = [&]() -> Result<JournalPage> {
        std::scoped_lock lock(impl_->mutex);
        if (!impl_->info) {
            return Result<JournalPage>::failure(
                makeError(NotebookErrorCode::notebookNotOpen, {}, "a Notebook must be open"));
        }
        if (!validJournalDate(date)) {
            return Result<JournalPage>::failure(makeError(
                NotebookErrorCode::invalidJournalDate, impl_->info->path, "invalid Journal date"));
        }
        auto outcome = impl_->applyHistory(date, direction);
        if (impl_->lastCommandCommitted) {
            callbacks = impl_->committedCallbacks();
        }
        return outcome;
    }();
    notifyCallbacks(callbacks);
    return result;
}

auto NotebookSession::subscribeToChanges(std::function<void()> callback) -> NotebookSubscription {
    const auto state = impl_->subscriptions;
    std::scoped_lock lock(state->mutex);
    const auto identifier = state->nextIdentifier++;
    state->callbacks.emplace(identifier, std::move(callback));
    return NotebookSubscription(std::make_unique<NotebookSubscription::Impl>(state, identifier));
}

} // namespace hieda::notebook
