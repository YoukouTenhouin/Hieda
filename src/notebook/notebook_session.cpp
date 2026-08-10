// SPDX-License-Identifier: MPL-2.0
#include "hieda/notebook/notebook_session.hpp"
#include "authored_text_parser.hpp"
#include "notebook_session_test_access.hpp"
#include "platform_file.hpp"

#include <lmdb.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
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
constexpr std::uint32_t schemaVersion = 2;
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

enum class BlockType : std::uint8_t {
    page = 1,
    entry = 2,
    journalPage = page,
    journalEntry = entry,
    pageEntry = entry,
};

struct BlockRecord {
    BlockType type{BlockType::journalEntry};
    BlockMetadata metadata;
    std::optional<JournalDate> journalDate;
    std::string authoredText;
    std::string pageName;
    std::string displayTitle;
    std::optional<PageKind> pageKind;
};

class JournalCommitAdapter {
  public:
    virtual ~JournalCommitAdapter() = default;
    virtual auto commit(MDB_txn*& transaction) -> int = 0;
};

class LmdbJournalCommitAdapter final : public JournalCommitAdapter {
  public:
    auto
    commit(MDB_txn*& transaction) -> int override
    {
        auto* committing = transaction;
        transaction = nullptr;
        return mdb_txn_commit(committing);
    }
};

#ifdef HIEDA_TESTING
class RejectNextJournalCommitAdapter final : public JournalCommitAdapter {
  public:
    auto
    commit(MDB_txn*& transaction) -> int override
    {
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

auto
makeError(NotebookErrorCode code, const std::filesystem::path& path,
          std::string detail) -> NotebookError
{
    return {code, path, std::move(detail)};
}

auto
errorFromSystem(const std::filesystem::path& path, const std::error_code& error,
                std::string_view operation) -> NotebookError
{
    auto code = NotebookErrorCode::ioFailure;
    if (error == std::errc::permission_denied ||
        error == std::errc::read_only_file_system) {
        code = NotebookErrorCode::permissionDenied;
    } else if (error == std::errc::no_such_file_or_directory) {
        code = NotebookErrorCode::pathNotFound;
    }
    return makeError(code, path,
                     std::string(operation) + ": " + error.message());
}

auto
errorFromPlatform(const std::filesystem::path& path,
                  const platform::FileError& error, std::string_view operation)
    -> NotebookError
{
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
    return makeError(code, path,
                     std::string(operation) + ": " +
                         error.systemError.message());
}

auto
pathWithSuffix(const std::filesystem::path& path, std::string_view suffix)
    -> std::filesystem::path
{
    auto result = path;
#ifdef _WIN32
    result += std::wstring(suffix.begin(), suffix.end());
#else
    result += suffix;
#endif
    return result;
}

auto
lmdbPath(const std::filesystem::path& path) -> std::string
{
#ifdef _WIN32
    const auto utf8 = path.u8string();
    return {reinterpret_cast<const char*>(utf8.data()), utf8.size()};
#else
    return path.native();
#endif
}

auto
errorFromLmdb(const std::filesystem::path& path, int error,
              std::string_view operation) -> NotebookError
{
    if (error == MDB_KEYEXIST || error == MDB_PANIC || error == MDB_TLS_FULL ||
        error == MDB_BAD_TXN || error == MDB_BAD_RSLOT ||
        error == MDB_BAD_VALSIZE || error == MDB_INCOMPATIBLE ||
        error == MDB_BAD_DBI || error == MDB_DBS_FULL ||
        error == MDB_PAGE_FULL || error == MDB_CURSOR_FULL) {
        throw NotebookException(std::string(operation) + ": " +
                                mdb_strerror(error));
    }
    auto code = NotebookErrorCode::ioFailure;
    if (error == MDB_INVALID || error == MDB_CORRUPTED ||
        error == MDB_PAGE_NOTFOUND || error == MDB_NOTFOUND) {
        code = NotebookErrorCode::invalidNotebook;
    } else if (error == MDB_VERSION_MISMATCH) {
        code = NotebookErrorCode::unsupportedVersion;
    } else if (error == EACCES || error == EPERM) {
        code = NotebookErrorCode::permissionDenied;
    } else if (error == ENOENT) {
        code = NotebookErrorCode::pathNotFound;
    }
    return makeError(code, path,
                     std::string(operation) + ": " + mdb_strerror(error));
}

auto
openLmdbEnvironment(const std::filesystem::path& path) -> Result<MDB_env*>
{
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
        result =
            mdb_env_open(environment, encodedPath.c_str(), MDB_NOSUBDIR, 0600);
    }
    if (result != MDB_SUCCESS) {
        if (environment != nullptr) {
            mdb_env_close(environment);
        }
        return Result<MDB_env*>::failure(
            errorFromLmdb(path, result, "open LMDB environment"));
    }
    return Result<MDB_env*>::success(environment);
}

void
appendU16(std::vector<std::uint8_t>& output, std::uint16_t value)
{
    output.push_back(static_cast<std::uint8_t>(value >> 8U));
    output.push_back(static_cast<std::uint8_t>(value));
}

void
appendU32(std::vector<std::uint8_t>& output, std::uint32_t value)
{
    for (int shift = 24; shift >= 0; shift -= 8) {
        output.push_back(
            static_cast<std::uint8_t>(value >> static_cast<unsigned>(shift)));
    }
}

void
appendU64(std::vector<std::uint8_t>& output, std::uint64_t value)
{
    for (int shift = 56; shift >= 0; shift -= 8) {
        output.push_back(
            static_cast<std::uint8_t>(value >> static_cast<unsigned>(shift)));
    }
}

void
appendField(std::vector<std::uint8_t>& output, std::uint16_t tag,
            const std::uint8_t* data, std::size_t size)
{
    appendU16(output, tag);
    appendU32(output, static_cast<std::uint32_t>(size));
    output.insert(output.end(), data, data + size);
}

auto
encodeManifest(const Manifest& manifest) -> std::vector<std::uint8_t>
{
    std::vector<std::uint8_t> output;
    appendU16(output, 1);
    appendField(output, 1,
                reinterpret_cast<const std::uint8_t*>(formatMagic.data()),
                formatMagic.size());

    std::vector<std::uint8_t> number;
    appendU32(number, formatVersion);
    appendField(output, 2, number.data(), number.size());
    number.clear();
    appendU32(number, schemaVersion);
    appendField(output, 3, number.data(), number.size());
    appendField(output, 4,
                reinterpret_cast<const std::uint8_t*>(manifest.id.bytes.data()),
                manifest.id.bytes.size());
    number.clear();
    appendU64(number,
              static_cast<std::uint64_t>(manifest.createdAtMicroseconds));
    appendField(output, 5, number.data(), number.size());
    number.clear();
    appendU64(number, manifest.revision);
    appendField(output, 6, number.data(), number.size());
    return output;
}

auto
readU16(const std::uint8_t* data) -> std::uint16_t
{
    return static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(data[0]) << 8U) | data[1]);
}

auto
readU32(const std::uint8_t* data) -> std::uint32_t
{
    std::uint32_t value = 0;
    for (int index = 0; index < 4; ++index) {
        value = (value << 8U) | data[index];
    }
    return value;
}

auto
readU64(const std::uint8_t* data) -> std::uint64_t
{
    std::uint64_t value = 0;
    for (int index = 0; index < 8; ++index) {
        value = (value << 8U) | data[index];
    }
    return value;
}

auto
decodeManifest(const MDB_val& value, const std::filesystem::path& path)
    -> Result<Manifest>
{
    const auto* bytes = static_cast<const std::uint8_t*>(value.mv_data);
    const auto size = value.mv_size;
    if (size < 2 || readU16(bytes) != 1) {
        return Result<Manifest>::failure(
            makeError(NotebookErrorCode::invalidNotebook, path,
                      "invalid manifest record version"));
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
                makeError(NotebookErrorCode::invalidNotebook, path,
                          "truncated manifest field"));
        }
        const auto tag = readU16(bytes + offset);
        const auto length =
            static_cast<std::size_t>(readU32(bytes + offset + 2));
        offset += 6;
        if (length > size - offset) {
            return Result<Manifest>::failure(
                makeError(NotebookErrorCode::invalidNotebook, path,
                          "invalid manifest field length"));
        }
        const auto* field = bytes + offset;
        if (tag == 1 && length == formatMagic.size()) {
            hasMagic = std::memcmp(field, formatMagic.data(), length) == 0;
        } else if (tag == 2 && length == 4) {
            hasFormatVersion = true;
            if (readU32(field) != formatVersion) {
                return Result<Manifest>::failure(
                    makeError(NotebookErrorCode::unsupportedVersion, path,
                              "unsupported Notebook format version"));
            }
        } else if (tag == 3 && length == 4) {
            hasSchemaVersion = true;
            if (readU32(field) != schemaVersion) {
                return Result<Manifest>::failure(
                    makeError(NotebookErrorCode::unsupportedVersion, path,
                              "unsupported Notebook schema version"));
            }
        } else if (tag == 4 && length == manifest.id.bytes.size()) {
            std::memcpy(manifest.id.bytes.data(), field, length);
            hasId = true;
        } else if (tag == 5 && length == 8) {
            manifest.createdAtMicroseconds =
                static_cast<std::int64_t>(readU64(field));
        } else if (tag == 6 && length == 8) {
            manifest.revision = readU64(field);
        }
        offset += length;
    }

    if (!hasMagic || !hasFormatVersion || !hasSchemaVersion || !hasId) {
        return Result<Manifest>::failure(
            makeError(NotebookErrorCode::invalidNotebook, path,
                      "Notebook manifest is incomplete"));
    }
    return Result<Manifest>::success(manifest);
}

auto
generateId() -> NotebookId
{
    std::random_device random;
    NotebookId id;
    for (auto& byte : id.bytes) {
        byte = static_cast<std::byte>(random() & 0xFFU);
    }
    id.bytes[6] = static_cast<std::byte>(
        (std::to_integer<unsigned>(id.bytes[6]) & 0x0FU) | 0x40U);
    id.bytes[8] = static_cast<std::byte>(
        (std::to_integer<unsigned>(id.bytes[8]) & 0x3FU) | 0x80U);
    return id;
}

auto
generateBlockId() -> BlockId
{
    std::random_device random;
    BlockId id;
    for (auto& byte : id.bytes) {
        byte = static_cast<std::byte>(random() & 0xFFU);
    }
    id.bytes[6] = static_cast<std::byte>(
        (std::to_integer<unsigned>(id.bytes[6]) & 0x0FU) | 0x40U);
    id.bytes[8] = static_cast<std::byte>(
        (std::to_integer<unsigned>(id.bytes[8]) & 0x3FU) | 0x80U);
    return id;
}

auto
currentTimestamp() -> BlockTimestamp
{
    return std::chrono::time_point_cast<std::chrono::microseconds>(
        std::chrono::system_clock::now());
}

auto
validJournalDate(JournalDate date) -> bool
{
    if (date.year < 1 || date.year > 9999) {
        return false;
    }
    const auto value = std::chrono::year_month_day{
        std::chrono::year{date.year}, std::chrono::month{date.month},
        std::chrono::day{date.day}};
    return value.ok();
}

auto
validAuthoredText(std::string_view text) -> bool
{
    constexpr std::size_t maximumAuthoredTextBytes = 1024ULL * 1024ULL;
    if (text.size() > maximumAuthoredTextBytes) {
        return false;
    }
    std::size_t index = 0;
    while (index < text.size()) {
        const auto first = static_cast<unsigned char>(text[index]);
        std::size_t continuationCount = 0;
        std::uint32_t codePoint = 0;
        if (first <= 0x7FU) {
            if ((first <= 0x1FU && first != '\n') || first == 0x7FU) {
                return false;
            }
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
        if (codePoint >= 0x80U && codePoint <= 0x9FU) {
            return false;
        }
        index += continuationCount + 1;
    }
    return true;
}

auto
validPageTitle(std::string_view title) -> bool
{
    return !title.empty() && title.find('\n') == std::string_view::npos &&
           title.find('\r') == std::string_view::npos &&
           title.find('\0') == std::string_view::npos &&
           validAuthoredText(title);
}

auto
timestampMicroseconds(BlockTimestamp timestamp) -> std::int64_t
{
    return timestamp.time_since_epoch().count();
}

auto
encodeBlock(const BlockRecord& block) -> std::vector<std::uint8_t>
{
    std::vector<std::uint8_t> output;
    appendU16(output, 2);
    const auto type = static_cast<std::uint8_t>(block.type);
    appendField(output, 1, &type, 1);
    std::vector<std::uint8_t> number;
    appendU64(number, static_cast<std::uint64_t>(
                          timestampMicroseconds(block.metadata.createdAt)));
    appendField(output, 2, number.data(), number.size());
    number.clear();
    appendU64(number, static_cast<std::uint64_t>(
                          timestampMicroseconds(block.metadata.updatedAt)));
    appendField(output, 3, number.data(), number.size());
    if (block.pageKind) {
        const auto kind = static_cast<std::uint8_t>(
            static_cast<std::uint8_t>(*block.pageKind) + 1U);
        appendField(output, 4, &kind, 1);
    }
    if (block.journalDate) {
        number.clear();
        const auto packed = static_cast<std::uint32_t>(
            (block.journalDate->year * 10000) +
            (block.journalDate->month * 100) + block.journalDate->day);
        appendU32(number, packed);
        appendField(output, 5, number.data(), number.size());
    }
    if (block.type == BlockType::entry) {
        appendField(
            output, 6,
            reinterpret_cast<const std::uint8_t*>(block.authoredText.data()),
            block.authoredText.size());
    }
    if (block.type == BlockType::page && block.pageKind == PageKind::named) {
        appendField(
            output, 7,
            reinterpret_cast<const std::uint8_t*>(block.pageName.data()),
            block.pageName.size());
        appendField(
            output, 8,
            reinterpret_cast<const std::uint8_t*>(block.displayTitle.data()),
            block.displayTitle.size());
    }
    return output;
}

auto
decodeBlock(const MDB_val& value, const BlockId& blockIdentifier,
            const std::filesystem::path& path) -> Result<BlockRecord>
{
    const auto* bytes = static_cast<const std::uint8_t*>(value.mv_data);
    if (value.mv_size < 2 || readU16(bytes) != 2) {
        return Result<BlockRecord>::failure(
            makeError(NotebookErrorCode::invalidNotebook, path,
                      "invalid Block record version"));
    }
    BlockRecord block;
    block.metadata.id = blockIdentifier;
    bool hasType = false;
    bool hasCreated = false;
    bool hasUpdated = false;
    bool hasPageKind = false;
    std::size_t offset = 2;
    while (offset < value.mv_size) {
        if (value.mv_size - offset < 6) {
            return Result<BlockRecord>::failure(
                makeError(NotebookErrorCode::invalidNotebook, path,
                          "truncated Block field"));
        }
        const auto tag = readU16(bytes + offset);
        const auto length =
            static_cast<std::size_t>(readU32(bytes + offset + 2));
        offset += 6;
        if (length > value.mv_size - offset) {
            return Result<BlockRecord>::failure(
                makeError(NotebookErrorCode::invalidNotebook, path,
                          "invalid Block field length"));
        }
        const auto* field = bytes + offset;
        if (tag == 1 && length == 1 && field[0] >= 1 && field[0] <= 2) {
            block.type = static_cast<BlockType>(field[0]);
            hasType = true;
        } else if (tag == 2 && length == 8) {
            block.metadata.createdAt = BlockTimestamp{std::chrono::microseconds{
                static_cast<std::int64_t>(readU64(field))}};
            hasCreated = true;
        } else if (tag == 3 && length == 8) {
            block.metadata.updatedAt = BlockTimestamp{std::chrono::microseconds{
                static_cast<std::int64_t>(readU64(field))}};
            hasUpdated = true;
        } else if (tag == 4 && length == 1 && field[0] >= 1 && field[0] <= 2) {
            block.pageKind = static_cast<PageKind>(field[0] - 1U);
            hasPageKind = true;
        } else if (tag == 5 && length == 4) {
            const auto packed = readU32(field);
            block.journalDate =
                JournalDate{static_cast<std::int32_t>(packed / 10000U),
                            static_cast<std::uint8_t>((packed / 100U) % 100U),
                            static_cast<std::uint8_t>(packed % 100U)};
        } else if (tag == 6) {
            block.authoredText.assign(reinterpret_cast<const char*>(field),
                                      length);
        } else if (tag == 7) {
            block.pageName.assign(reinterpret_cast<const char*>(field), length);
        } else if (tag == 8) {
            block.displayTitle.assign(reinterpret_cast<const char*>(field),
                                      length);
        }
        offset += length;
    }
    const auto validPage =
        block.type == BlockType::page && hasPageKind &&
        ((block.pageKind == PageKind::journal && block.journalDate &&
          validJournalDate(block.journalDate.value_or(JournalDate{})) &&
          block.pageName.empty() && block.displayTitle.empty()) ||
         (block.pageKind == PageKind::named && !block.journalDate &&
          authored_text::validPageName(block.pageName) &&
          validPageTitle(block.displayTitle))) &&
        block.authoredText.empty();
    const auto validEntry = block.type == BlockType::entry && !hasPageKind &&
                            !block.journalDate && block.pageName.empty() &&
                            block.displayTitle.empty() &&
                            validAuthoredText(block.authoredText);
    if (!hasType || !hasCreated || !hasUpdated || (!validPage && !validEntry)) {
        return Result<BlockRecord>::failure(
            makeError(NotebookErrorCode::invalidNotebook, path,
                      "Block record is incomplete"));
    }
    return Result<BlockRecord>::success(std::move(block));
}

auto
dateKey(JournalDate date) -> std::array<std::uint8_t, 4>
{
    const auto packed = static_cast<std::uint32_t>(
        (date.year * 10000) + (date.month * 100) + date.day);
    return {static_cast<std::uint8_t>(packed >> 24U),
            static_cast<std::uint8_t>(packed >> 16U),
            static_cast<std::uint8_t>(packed >> 8U),
            static_cast<std::uint8_t>(packed)};
}

auto
blockKey(const BlockId& blockIdentifier) -> MDB_val
{
    return {blockIdentifier.bytes.size(),
            const_cast<std::byte*>(blockIdentifier.bytes.data())};
}

template <typename Identifier>
auto
formatUuid(const Identifier& identifier) -> std::string
{
    std::ostringstream output;
    output << std::hex << std::setfill('0');
    for (std::size_t index = 0; index < identifier.bytes.size(); ++index) {
        if (index == 4 || index == 6 || index == 8 || index == 10) {
            output << '-';
        }
        output << std::setw(2)
               << std::to_integer<unsigned>(identifier.bytes[index]);
    }
    return output.str();
}

void
notifyCallbacks(const std::vector<std::function<void()>>& callbacks) noexcept
{
    for (const auto& callback : callbacks) {
        try {
            callback();
        } catch (...) {
            // A committed command remains successful even if an observer fails.
            static_cast<void>(std::current_exception());
        }
    }
}

auto
createEnvironment(const std::filesystem::path& path, const Manifest& manifest)
    -> std::optional<NotebookError>
{
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
        result =
            mdb_dbi_open(transaction, ownedName.c_str(), MDB_CREATE, &database);
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

void
removeIfPresent(const std::filesystem::path& path) noexcept
{
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
    MDB_dbi pagesByName{0};
    MDB_dbi referencesBySource{0};
    MDB_dbi referencesByTarget{0};
};

enum class SemanticReferenceTargetIndexKind : std::uint8_t {
    resolvedPage = 'R',
    unresolvedPage = 'U',
    resolvedBlock = 'B',
    missingBlock = 'M',
};

struct LinkedReferencesCursor {
    std::uint64_t revision{0};
    std::size_t offset{0};

    [[nodiscard]] static auto
    decode(std::string_view text) -> std::optional<LinkedReferencesCursor>
    {
        const auto separator = text.find(':');
        LinkedReferencesCursor cursor;
        if (separator == std::string_view::npos ||
            std::from_chars(text.data(), text.data() + separator,
                            cursor.revision)
                    .ec != std::errc{} ||
            std::from_chars(text.data() + separator + 1,
                            text.data() + text.size(), cursor.offset)
                    .ec != std::errc{}) {
            return std::nullopt;
        }
        return cursor;
    }

    [[nodiscard]] auto
    encode() const -> std::string
    {
        return std::to_string(revision) + ":" + std::to_string(offset);
    }
};

auto
resolvedPageLinkTargetPrefix(const BlockId& pageId) -> std::vector<std::uint8_t>
{
    std::vector<std::uint8_t> prefix{static_cast<std::uint8_t>(
        SemanticReferenceTargetIndexKind::resolvedPage)};
    const auto* bytes =
        reinterpret_cast<const std::uint8_t*>(pageId.bytes.data());
    prefix.insert(prefix.end(), bytes, bytes + pageId.bytes.size());
    return prefix;
}

auto
unresolvedPageLinkTargetPrefix(std::string_view pageName)
    -> std::vector<std::uint8_t>
{
    std::vector<std::uint8_t> prefix{static_cast<std::uint8_t>(
        SemanticReferenceTargetIndexKind::unresolvedPage)};
    appendU16(prefix, static_cast<std::uint16_t>(pageName.size()));
    prefix.insert(prefix.end(), pageName.begin(), pageName.end());
    return prefix;
}

auto
blockReferenceTargetPrefix(const BlockId& blockId, bool resolved)
    -> std::vector<std::uint8_t>
{
    std::vector<std::uint8_t> prefix{static_cast<std::uint8_t>(
        resolved ? SemanticReferenceTargetIndexKind::resolvedBlock
                 : SemanticReferenceTargetIndexKind::missingBlock)};
    const auto* bytes =
        reinterpret_cast<const std::uint8_t*>(blockId.bytes.data());
    prefix.insert(prefix.end(), bytes, bytes + blockId.bytes.size());
    return prefix;
}

struct ParentLink {
    BlockId parent;
    std::uint64_t rank{0};
};

struct OutlineEntryRecord {
    BlockMetadata metadata;
    std::string authoredText;
    std::optional<BlockId> parentEntry;
};

struct LoadedOutline {
    BlockRecord page;
    std::vector<OutlineEntryRecord> entries;
};

auto
publicJournalEntries(const std::vector<OutlineEntryRecord>& outlineEntries)
    -> std::vector<Entry>
{
    std::vector<Entry> entries;
    entries.reserve(outlineEntries.size());
    for (const auto& entry : outlineEntries) {
        entries.push_back(
            {entry.metadata, entry.authoredText, entry.parentEntry});
    }
    return entries;
}

auto
publicOutline(const LoadedOutline& outline) -> OutlinePage
{
    OutlinePage result;
    if (outline.page.pageKind == PageKind::journal) {
        result.kind = PageKind::journal;
        result.journalDate = outline.page.journalDate;
    } else {
        result.kind = PageKind::named;
        result.name = outline.page.pageName;
        result.displayTitle = outline.page.displayTitle;
    }
    result.metadata = outline.page.metadata;
    result.entries = publicJournalEntries(outline.entries);
    return result;
}

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

struct MechanicalTextChange {
    BlockId sourceId;
    std::string before;
    std::string after;
};

struct PageHistoryAction {
    std::uint64_t sequence{0};
    std::optional<Page> before;
    std::optional<Page> after;
    std::vector<MechanicalTextChange> mechanicalTextChanges;
    std::size_t estimatedBytes{0};
};

struct PageHistory {
    BlockId pageId;
    std::deque<PageHistoryAction> undo;
    std::deque<PageHistoryAction> redo;
};

struct CrossPageHistoryAction {
    std::uint64_t sequence{0};
    LoadedOutline beforeSource;
    std::optional<LoadedOutline> beforeDestination;
    LoadedOutline afterSource;
    LoadedOutline afterDestination;
    std::size_t estimatedBytes{0};
};

enum class OutlineEditKind : std::uint8_t {
    split,
    join,
    indent,
    outdent,
    up,
    down,
    erase
};

auto
openJournalDatabases(MDB_txn* transaction, const std::filesystem::path& path,
                     bool createSemanticReferenceIndexes = false)
    -> Result<JournalDatabases>
{
    JournalDatabases databases;
    const std::array<std::pair<const char*, MDB_dbi*>, 9> names{{
        {"metadata", &databases.metadata},
        {"blocks", &databases.blocks},
        {"blocks_by_type", &databases.blocksByType},
        {"containment_by_parent", &databases.containmentByParent},
        {"containment_by_child", &databases.containmentByChild},
        {"journal_by_date", &databases.journalByDate},
        {"pages_by_title", &databases.pagesByName},
        {"references_by_source", &databases.referencesBySource},
        {"references_by_target", &databases.referencesByTarget},
    }};
    for (std::size_t index = 0; index < names.size(); ++index) {
        const auto& [name, database] = names[index];
        const auto flags =
            createSemanticReferenceIndexes && index >= names.size() - 2
                ? static_cast<unsigned int>(MDB_CREATE)
                : 0U;
        const auto result = mdb_dbi_open(transaction, name, flags, database);
        if (result != MDB_SUCCESS) {
            return Result<JournalDatabases>::failure(
                errorFromLmdb(path, result, "open Journal database"));
        }
    }
    return Result<JournalDatabases>::success(databases);
}

auto
readBlock(MDB_txn* transaction, MDB_dbi database,
          const BlockId& blockIdentifier, const std::filesystem::path& path)
    -> Result<BlockRecord>
{
    auto key = blockKey(blockIdentifier);
    MDB_val value{};
    const auto result = mdb_get(transaction, database, &key, &value);
    if (result == MDB_NOTFOUND) {
        return Result<BlockRecord>::failure(makeError(
            NotebookErrorCode::blockNotFound, path, "Block does not exist"));
    }
    if (result != MDB_SUCCESS) {
        return Result<BlockRecord>::failure(
            errorFromLmdb(path, result, "read Block"));
    }
    return decodeBlock(value, blockIdentifier, path);
}

auto
writeBlock(MDB_txn* transaction, MDB_dbi database, const BlockRecord& block,
           const std::filesystem::path& path) -> std::optional<NotebookError>
{
    auto key = blockKey(block.metadata.id);
    auto encoded = encodeBlock(block);
    MDB_val value{encoded.size(), encoded.data()};
    const auto result = mdb_put(transaction, database, &key, &value, 0);
    if (result != MDB_SUCCESS) {
        return errorFromLmdb(path, result, "write Block");
    }
    return std::nullopt;
}

auto
writeIncrementedRevision(MDB_txn* transaction, MDB_dbi metadata,
                         const std::filesystem::path& path)
    -> std::optional<NotebookError>
{
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

auto
typeIndexKey(BlockType type, const BlockId& blockIdentifier)
    -> std::array<std::uint8_t, 17>
{
    std::array<std::uint8_t, 17> key{};
    key[0] = static_cast<std::uint8_t>(type);
    std::memcpy(key.data() + 1, blockIdentifier.bytes.data(),
                blockIdentifier.bytes.size());
    return key;
}

auto
containmentParentKey(const BlockId& parent, std::uint64_t rank)
    -> std::array<std::uint8_t, 24>
{
    std::array<std::uint8_t, 24> key{};
    std::memcpy(key.data(), parent.bytes.data(), parent.bytes.size());
    for (std::size_t index = 0; index < 8; ++index) {
        key[16 + index] =
            static_cast<std::uint8_t>(rank >> ((7U - index) * 8U));
    }
    return key;
}

auto
rankFromParentKey(const MDB_val& key) -> std::uint64_t
{
    const auto* bytes = static_cast<const std::uint8_t*>(key.mv_data);
    return readU64(bytes + 16);
}

auto
writeTypeIndex(MDB_txn* transaction, MDB_dbi database, BlockType type,
               const BlockId& blockIdentifier,
               const std::filesystem::path& path)
    -> std::optional<NotebookError>
{
    auto encoded = typeIndexKey(type, blockIdentifier);
    MDB_val key{encoded.size(), encoded.data()};
    MDB_val value{0, nullptr};
    const auto result =
        mdb_put(transaction, database, &key, &value, MDB_NOOVERWRITE);
    if (result != MDB_SUCCESS) {
        return errorFromLmdb(path, result, "index Block type");
    }
    return std::nullopt;
}

auto
rebuildSemanticReferenceIndexes(MDB_txn* transaction,
                                const JournalDatabases& databases,
                                const std::filesystem::path& path)
    -> std::optional<NotebookError>
{
    auto result = mdb_drop(transaction, databases.referencesBySource, 0);
    if (result == MDB_SUCCESS) {
        result = mdb_drop(transaction, databases.referencesByTarget, 0);
    }
    if (result != MDB_SUCCESS) {
        return errorFromLmdb(path, result, "clear Page Link indexes");
    }

    MDB_cursor* cursor = nullptr;
    result = mdb_cursor_open(transaction, databases.blocksByType, &cursor);
    if (result != MDB_SUCCESS) {
        return errorFromLmdb(path, result, "open Page Link source scan");
    }
    BlockId lowerId{};
    auto lower = typeIndexKey(BlockType::entry, lowerId);
    MDB_val key{lower.size(), lower.data()};
    MDB_val value{};
    result = mdb_cursor_get(cursor, &key, &value, MDB_SET_RANGE);
    while (result == MDB_SUCCESS && key.mv_size == lower.size() &&
           static_cast<const std::uint8_t*>(key.mv_data)[0] ==
               static_cast<std::uint8_t>(BlockType::entry)) {
        BlockId sourceId;
        std::memcpy(sourceId.bytes.data(),
                    static_cast<const std::uint8_t*>(key.mv_data) + 1,
                    sourceId.bytes.size());
        auto source = readBlock(transaction, databases.blocks, sourceId, path);
        if (!source) {
            mdb_cursor_close(cursor);
            return source.error();
        }
        const auto links =
            authored_text::pageLinks(source.value().authoredText);
        if (!links.empty()) {
            std::vector<std::uint8_t> encoded;
            appendU16(encoded, 1);
            appendU32(encoded, static_cast<std::uint32_t>(links.size()));
            for (const auto& link : links) {
                appendU32(encoded,
                          static_cast<std::uint32_t>(link.sourceByteOffset));
                appendU32(encoded,
                          static_cast<std::uint32_t>(link.sourceByteLength));
                appendU16(encoded,
                          static_cast<std::uint16_t>(link.pageName.size()));
                encoded.insert(encoded.end(), link.pageName.begin(),
                               link.pageName.end());

                MDB_val nameKey{link.pageName.size(),
                                const_cast<char*>(link.pageName.data())};
                MDB_val pageValue{};
                const auto lookup = mdb_get(transaction, databases.pagesByName,
                                            &nameKey, &pageValue);
                const auto resolved =
                    lookup == MDB_SUCCESS &&
                    pageValue.mv_size == sourceId.bytes.size();
                encoded.push_back(resolved ? 1U : 0U);
                if (resolved) {
                    const auto* target =
                        static_cast<const std::uint8_t*>(pageValue.mv_data);
                    encoded.insert(encoded.end(), target,
                                   target + sourceId.bytes.size());
                } else if (lookup != MDB_NOTFOUND) {
                    mdb_cursor_close(cursor);
                    return lookup == MDB_SUCCESS
                               ? makeError(NotebookErrorCode::invalidNotebook,
                                           path,
                                           "Page name index contains an "
                                           "invalid identity")
                               : errorFromLmdb(path, lookup,
                                               "resolve Page Link");
                }
            }
            auto sourceKey = blockKey(sourceId);
            MDB_val sourceValue{encoded.size(), encoded.data()};
            const auto put = mdb_put(transaction, databases.referencesBySource,
                                     &sourceKey, &sourceValue, 0);
            if (put != MDB_SUCCESS) {
                mdb_cursor_close(cursor);
                return errorFromLmdb(path, put, "write Page Link source index");
            }
        }
        for (const auto& reference :
             authored_text::blockReferences(source.value().authoredText)) {
            const auto targetId = reference.targetId;
            auto target =
                readBlock(transaction, databases.blocks, targetId, path);
            const auto resolved = static_cast<bool>(target);
            if (!resolved &&
                target.error().code != NotebookErrorCode::blockNotFound) {
                mdb_cursor_close(cursor);
                return target.error();
            }
            auto reverseKey = blockReferenceTargetPrefix(targetId, resolved);
            const auto* sourceBytes =
                reinterpret_cast<const std::uint8_t*>(sourceId.bytes.data());
            reverseKey.insert(reverseKey.end(), sourceBytes,
                              sourceBytes + sourceId.bytes.size());
            MDB_val reverseKeyValue{reverseKey.size(), reverseKey.data()};
            MDB_val empty{0, nullptr};
            const auto put = mdb_put(transaction, databases.referencesByTarget,
                                     &reverseKeyValue, &empty, 0);
            if (put != MDB_SUCCESS) {
                mdb_cursor_close(cursor);
                return errorFromLmdb(path, put,
                                     "write Block Reference reverse index");
            }
        }
        result = mdb_cursor_get(cursor, &key, &value, MDB_NEXT);
    }
    mdb_cursor_close(cursor);
    if (result != MDB_SUCCESS && result != MDB_NOTFOUND) {
        return errorFromLmdb(path, result, "scan Page Link sources");
    }

    MDB_cursor* sourceCursor = nullptr;
    result = mdb_cursor_open(transaction, databases.referencesBySource,
                             &sourceCursor);
    if (result != MDB_SUCCESS) {
        return errorFromLmdb(path, result,
                             "open Page Link reverse indexing scan");
    }
    MDB_val sourceKey{};
    MDB_val sourceValue{};
    result = mdb_cursor_get(sourceCursor, &sourceKey, &sourceValue, MDB_FIRST);
    while (result == MDB_SUCCESS) {
        const auto* bytes =
            static_cast<const std::uint8_t*>(sourceValue.mv_data);
        if (sourceKey.mv_size != BlockId{}.bytes.size() ||
            sourceValue.mv_size < 6 || readU16(bytes) != 1) {
            mdb_cursor_close(sourceCursor);
            return makeError(NotebookErrorCode::invalidNotebook, path,
                             "Page Link source index is invalid");
        }
        const auto count = readU32(bytes + 2);
        std::size_t offset = 6;
        for (std::uint32_t index = 0; index < count; ++index) {
            if (sourceValue.mv_size - offset < 11) {
                mdb_cursor_close(sourceCursor);
                return makeError(NotebookErrorCode::invalidNotebook, path,
                                 "Page Link occurrence index is truncated");
            }
            const auto nameLength = readU16(bytes + offset + 8);
            offset += 10;
            if (sourceValue.mv_size - offset <
                static_cast<std::size_t>(nameLength) + 1) {
                mdb_cursor_close(sourceCursor);
                return makeError(NotebookErrorCode::invalidNotebook, path,
                                 "Page Link occurrence name is truncated");
            }
            std::vector<std::uint8_t> reverseKey;
            const auto resolved = bytes[offset + nameLength] != 0;
            reverseKey.push_back(static_cast<std::uint8_t>(
                resolved ? SemanticReferenceTargetIndexKind::resolvedPage
                         : SemanticReferenceTargetIndexKind::unresolvedPage));
            if (resolved) {
                if (sourceValue.mv_size - offset <
                    static_cast<std::size_t>(nameLength) + 1 +
                        BlockId{}.bytes.size()) {
                    mdb_cursor_close(sourceCursor);
                    return makeError(NotebookErrorCode::invalidNotebook, path,
                                     "resolved Page Link target is truncated");
                }
                const auto* target = bytes + offset + nameLength + 1;
                reverseKey.insert(reverseKey.end(), target,
                                  target + BlockId{}.bytes.size());
                offset += nameLength + 1 + BlockId{}.bytes.size();
            } else {
                const auto name = std::string_view{
                    reinterpret_cast<const char*>(bytes + offset), nameLength};
                reverseKey = unresolvedPageLinkTargetPrefix(name);
                offset += nameLength + 1;
            }
            const auto* source =
                static_cast<const std::uint8_t*>(sourceKey.mv_data);
            reverseKey.insert(reverseKey.end(), source,
                              source + sourceKey.mv_size);
            MDB_val reverseKeyValue{reverseKey.size(), reverseKey.data()};
            MDB_val empty{0, nullptr};
            const auto put = mdb_put(transaction, databases.referencesByTarget,
                                     &reverseKeyValue, &empty, 0);
            if (put != MDB_SUCCESS) {
                mdb_cursor_close(sourceCursor);
                return errorFromLmdb(path, put,
                                     "write Page Link reverse index");
            }
        }
        if (offset != sourceValue.mv_size) {
            mdb_cursor_close(sourceCursor);
            return makeError(NotebookErrorCode::invalidNotebook, path,
                             "Page Link source index has trailing bytes");
        }
        result =
            mdb_cursor_get(sourceCursor, &sourceKey, &sourceValue, MDB_NEXT);
    }
    mdb_cursor_close(sourceCursor);
    return result == MDB_NOTFOUND
               ? std::nullopt
               : std::optional<NotebookError>{errorFromLmdb(
                     path, result, "scan Page Link reverse sources")};
}

auto
incrementRevision(MDB_txn* transaction, MDB_dbi metadata,
                  const std::filesystem::path& path)
    -> std::optional<NotebookError>
{
    auto databases = openJournalDatabases(transaction, path);
    if (!databases) {
        return databases.error();
    }
    if (auto error = rebuildSemanticReferenceIndexes(transaction,
                                                     databases.value(), path)) {
        return error;
    }
    return writeIncrementedRevision(transaction, metadata, path);
}

} // namespace

class NotebookSubscription::Impl {
  public:
    Impl(std::weak_ptr<SubscriptionState> state, std::size_t identifier)
        : state_(std::move(state)), identifier_(identifier)
    {
    }

    ~Impl()
    {
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
          subscriptions(std::make_shared<SubscriptionState>())
    {
    }

    ~Impl()
    {
        closeUnlocked();
    }

    void
    closeUnlocked() noexcept
    {
        if (environment != nullptr) {
            mdb_env_close(environment);
            environment = nullptr;
        }
        lockFile.reset();
        dataLockFile.reset();
        info.reset();
        journalHistory.clear();
        pageHistories.clear();
        crossPageUndo.clear();
        crossPageRedo.clear();
        historyBytes = 0;
        nextHistorySequence = 1;
    }

    auto
    acquireLock(const std::filesystem::path& path)
        -> std::optional<NotebookError>
    {
        auto acquired = platform::acquireExclusiveFileLock(
            pathWithSuffix(path, ".open-lock"), true);
        if (const auto* error = std::get_if<platform::FileError>(&acquired)) {
            if (error->kind == platform::FileErrorKind::alreadyLocked) {
                return makeError(NotebookErrorCode::alreadyInUse, path,
                                 "Notebook is already open in another session");
            }
            return errorFromPlatform(path, *error, "lock Notebook");
        }
        lockFile.emplace(
            std::get<platform::ExclusiveFileLock>(std::move(acquired)));
        return std::nullopt;
    }

    auto
    acquireDataLock(const std::filesystem::path& path)
        -> std::optional<NotebookError>
    {
        auto acquired = platform::acquireExclusiveFileLock(path, false);
        if (const auto* error = std::get_if<platform::FileError>(&acquired)) {
            if (error->kind == platform::FileErrorKind::alreadyLocked) {
                return makeError(
                    NotebookErrorCode::alreadyInUse, path,
                    "Notebook is already open through another path");
            }
            return errorFromPlatform(path, *error, "lock Notebook data file");
        }
        dataLockFile.emplace(
            std::get<platform::ExclusiveFileLock>(std::move(acquired)));
        return std::nullopt;
    }

    auto
    openEnvironment(const std::filesystem::path& path) -> Result<NotebookInfo>
    {
        auto opened = openLmdbEnvironment(path);
        if (!opened) {
            return Result<NotebookInfo>::failure(opened.error());
        }
        MDB_env* openedEnvironment = opened.value();
        auto result = MDB_SUCCESS;

        MDB_txn* transaction = nullptr;
        MDB_dbi metadata = 0;
        result =
            mdb_txn_begin(openedEnvironment, nullptr, MDB_RDONLY, &transaction);
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
        bool pageLinkIndexesMissing = false;
        if (manifest) {
            for (const auto* name :
                 {"references_by_source", "references_by_target"}) {
                MDB_dbi database = 0;
                result = mdb_dbi_open(transaction, name, 0, &database);
                if (result == MDB_NOTFOUND) {
                    pageLinkIndexesMissing = true;
                } else if (result != MDB_SUCCESS) {
                    break;
                }
            }
        }
        mdb_txn_abort(transaction);
        if (!manifest) {
            mdb_env_close(openedEnvironment);
            return Result<NotebookInfo>::failure(manifest.error());
        }
        if (result != MDB_SUCCESS && result != MDB_NOTFOUND) {
            mdb_env_close(openedEnvironment);
            return Result<NotebookInfo>::failure(
                errorFromLmdb(path, result, "inspect Page Link indexes"));
        }
        if (pageLinkIndexesMissing) {
            transaction = nullptr;
            result = mdb_txn_begin(openedEnvironment, nullptr, 0, &transaction);
            if (result == MDB_SUCCESS) {
                auto databases = openJournalDatabases(transaction, path, true);
                if (!databases) {
                    mdb_txn_abort(transaction);
                    mdb_env_close(openedEnvironment);
                    return Result<NotebookInfo>::failure(databases.error());
                }
                if (auto error = rebuildSemanticReferenceIndexes(
                        transaction, databases.value(), path)) {
                    mdb_txn_abort(transaction);
                    mdb_env_close(openedEnvironment);
                    return Result<NotebookInfo>::failure(std::move(*error));
                }
                result = mdb_txn_commit(transaction);
                transaction = nullptr;
            }
            if (result != MDB_SUCCESS) {
                if (transaction != nullptr) {
                    mdb_txn_abort(transaction);
                }
                mdb_env_close(openedEnvironment);
                return Result<NotebookInfo>::failure(
                    errorFromLmdb(path, result, "backfill Page Link indexes"));
            }
        }

        environment = openedEnvironment;
        info = NotebookInfo{manifest.value().id, path, schemaVersion,
                            manifest.value().revision};
        return Result<NotebookInfo>::success(*info);
    }

    auto
    finishOpen(const std::filesystem::path& path) -> Result<NotebookInfo>
    {
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

    auto
    childrenOf(MDB_txn* transaction, const JournalDatabases& databases,
               const BlockId& parent) const
        -> Result<std::vector<std::pair<BlockId, std::uint64_t>>>
    {
        const auto path = info.value_or(NotebookInfo{}).path;
        std::vector<std::pair<BlockId, std::uint64_t>> children;
        MDB_cursor* cursor = nullptr;
        auto result = mdb_cursor_open(transaction,
                                      databases.containmentByParent, &cursor);
        if (result != MDB_SUCCESS) {
            return Result<std::vector<std::pair<BlockId, std::uint64_t>>>::
                failure(
                    errorFromLmdb(path, result, "open Containment children"));
        }
        auto start = containmentParentKey(parent, 0);
        MDB_val key{start.size(), start.data()};
        MDB_val value{};
        result = mdb_cursor_get(cursor, &key, &value, MDB_SET_RANGE);
        while (result == MDB_SUCCESS && key.mv_size == 24 &&
               std::memcmp(key.mv_data, parent.bytes.data(),
                           parent.bytes.size()) == 0) {
            if (value.mv_size != BlockId{}.bytes.size()) {
                mdb_cursor_close(cursor);
                return Result<std::vector<std::pair<BlockId, std::uint64_t>>>::
                    failure(makeError(NotebookErrorCode::invalidNotebook, path,
                                      "invalid Containment child"));
            }
            BlockId child;
            std::memcpy(child.bytes.data(), value.mv_data, child.bytes.size());
            children.emplace_back(child, rankFromParentKey(key));
            result = mdb_cursor_get(cursor, &key, &value, MDB_NEXT);
        }
        mdb_cursor_close(cursor);
        if (result != MDB_SUCCESS && result != MDB_NOTFOUND) {
            return Result<std::vector<std::pair<BlockId, std::uint64_t>>>::
                failure(
                    errorFromLmdb(path, result, "read Containment children"));
        }
        return Result<std::vector<std::pair<BlockId, std::uint64_t>>>::success(
            std::move(children));
    }

    auto
    parentOf(MDB_txn* transaction, const JournalDatabases& databases,
             const BlockId& child) const -> Result<ParentLink>
    {
        const auto path = info.value_or(NotebookInfo{}).path;
        auto key = blockKey(child);
        MDB_val value{};
        const auto result =
            mdb_get(transaction, databases.containmentByChild, &key, &value);
        if (result == MDB_NOTFOUND) {
            return Result<ParentLink>::failure(
                makeError(NotebookErrorCode::invalidNotebook, path,
                          "contained Block has no parent"));
        }
        if (result != MDB_SUCCESS || value.mv_size != 24) {
            return Result<ParentLink>::failure(
                result == MDB_SUCCESS
                    ? makeError(NotebookErrorCode::invalidNotebook, path,
                                "invalid Containment parent index")
                    : errorFromLmdb(path, result, "read Containment parent"));
        }
        ParentLink link;
        std::memcpy(link.parent.bytes.data(), value.mv_data,
                    link.parent.bytes.size());
        MDB_val encoded{value.mv_size, value.mv_data};
        link.rank = rankFromParentKey(encoded);
        return Result<ParentLink>::success(link);
    }

    auto
    loadOutline(MDB_txn* transaction, const JournalDatabases& databases,
                BlockRecord page) const -> Result<LoadedOutline>
    {
        const auto path = info.value_or(NotebookInfo{}).path;
        LoadedOutline outline{std::move(page), {}};
        struct PendingEntry {
            BlockId id;
            std::optional<BlockId> parentEntry;
            std::uint64_t rank{0};
        };
        std::vector<PendingEntry> pending;
        auto roots =
            childrenOf(transaction, databases, outline.page.metadata.id);
        if (!roots) {
            return Result<LoadedOutline>::failure(roots.error());
        }
        for (const auto& [identifier, rank] :
             roots.value() | std::views::reverse) {
            pending.push_back({identifier, std::nullopt, rank});
        }
        std::vector<BlockId> visited;
        while (!pending.empty()) {
            const auto current = pending.back();
            pending.pop_back();
            if (std::ranges::find(visited, current.id) != visited.end()) {
                return Result<LoadedOutline>::failure(
                    makeError(NotebookErrorCode::invalidNotebook, path,
                              "cyclic Journal Containment"));
            }
            visited.push_back(current.id);
            auto parent = parentOf(transaction, databases, current.id);
            const auto expectedParent =
                current.parentEntry.value_or(outline.page.metadata.id);
            if (!parent || parent.value().parent != expectedParent ||
                parent.value().rank != current.rank) {
                return Result<LoadedOutline>::failure(
                    parent
                        ? makeError(NotebookErrorCode::invalidNotebook, path,
                                    "inconsistent Journal Containment indexes")
                        : parent.error());
            }
            auto block =
                readBlock(transaction, databases.blocks, current.id, path);
            if (!block || (block.value().type != BlockType::journalEntry &&
                           block.value().type != BlockType::pageEntry)) {
                return Result<LoadedOutline>::failure(
                    block ? makeError(NotebookErrorCode::invalidNotebook, path,
                                      "Journal contains a non-Entry Block")
                          : block.error());
            }
            outline.entries.push_back({block.value().metadata,
                                       block.value().authoredText,
                                       current.parentEntry});
            auto children = childrenOf(transaction, databases, current.id);
            if (!children) {
                return Result<LoadedOutline>::failure(children.error());
            }
            for (const auto& [identifier, rank] :
                 children.value() | std::views::reverse) {
                pending.push_back({identifier, current.id, rank});
            }
        }
        return Result<LoadedOutline>::success(std::move(outline));
    }

    auto
    loadOutlineForEntry(MDB_txn* transaction, const JournalDatabases& databases,
                        BlockId entryId) const -> Result<LoadedOutline>
    {
        const auto path = info.value_or(NotebookInfo{}).path;
        auto entry = readBlock(transaction, databases.blocks, entryId, path);
        if (!entry || (entry.value().type != BlockType::journalEntry &&
                       entry.value().type != BlockType::pageEntry)) {
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
            if (std::ranges::find(visited, parent.value().parent) !=
                visited.end()) {
                return Result<LoadedOutline>::failure(
                    makeError(NotebookErrorCode::invalidNotebook, path,
                              "cyclic Journal Containment"));
            }
            visited.push_back(parent.value().parent);
            auto block = readBlock(transaction, databases.blocks,
                                   parent.value().parent, path);
            if (!block) {
                return Result<LoadedOutline>::failure(block.error());
            }
            if (block.value().type == BlockType::page) {
                return loadOutline(transaction, databases,
                                   std::move(block).value());
            }
            if (block.value().type != entry.value().type) {
                return Result<LoadedOutline>::failure(
                    makeError(NotebookErrorCode::invalidNotebook, path,
                              "invalid Journal ancestor"));
            }
            current = parent.value().parent;
        }
    }

    auto
    eraseContainment(MDB_txn* transaction, const JournalDatabases& databases,
                     const OutlineEntryRecord& entry) const
        -> std::optional<NotebookError>
    {
        const auto path = info.value_or(NotebookInfo{}).path;
        auto parent = parentOf(transaction, databases, entry.metadata.id);
        if (!parent) {
            return parent.error();
        }
        auto parentKey =
            containmentParentKey(parent.value().parent, parent.value().rank);
        MDB_val encodedParent{parentKey.size(), parentKey.data()};
        auto result = mdb_del(transaction, databases.containmentByParent,
                              &encodedParent, nullptr);
        if (result != MDB_SUCCESS) {
            return errorFromLmdb(path, result, "remove Containment ordering");
        }
        auto childKey = blockKey(entry.metadata.id);
        result = mdb_del(transaction, databases.containmentByChild, &childKey,
                         nullptr);
        if (result != MDB_SUCCESS) {
            return errorFromLmdb(path, result, "remove Containment parent");
        }
        return std::nullopt;
    }

    auto
    rewriteContainment(MDB_txn* transaction, const JournalDatabases& databases,
                       const LoadedOutline& before,
                       const LoadedOutline& after) const
        -> std::optional<NotebookError>
    {
        const auto path = info.value_or(NotebookInfo{}).path;
        for (const auto& entry : before.entries) {
            if (auto error = eraseContainment(transaction, databases, entry)) {
                return error;
            }
        }
        constexpr std::uint64_t rankGap = 1ULL << 32U;
        std::vector<std::pair<BlockId, std::uint64_t>> nextRanks;
        for (const auto& entry : after.entries) {
            const auto parent =
                entry.parentEntry.value_or(after.page.metadata.id);
            auto rank = rankGap;
            auto found =
                std::ranges::find_if(nextRanks, [&](const auto& item) -> bool {
                    return item.first == parent;
                });
            if (found == nextRanks.end()) {
                nextRanks.emplace_back(parent, rankGap * 2U);
            } else {
                rank = found->second;
                found->second += rankGap;
            }
            auto parentBytes = containmentParentKey(parent, rank);
            MDB_val parentKey{parentBytes.size(), parentBytes.data()};
            MDB_val childValue{
                entry.metadata.id.bytes.size(),
                const_cast<std::byte*>(entry.metadata.id.bytes.data())};
            auto result = mdb_put(transaction, databases.containmentByParent,
                                  &parentKey, &childValue, MDB_NOOVERWRITE);
            if (result != MDB_SUCCESS) {
                return errorFromLmdb(path, result,
                                     "write Containment ordering");
            }
            auto childKey = blockKey(entry.metadata.id);
            MDB_val parentValue{parentBytes.size(), parentBytes.data()};
            result = mdb_put(transaction, databases.containmentByChild,
                             &childKey, &parentValue, MDB_NOOVERWRITE);
            if (result != MDB_SUCCESS) {
                return errorFromLmdb(path, result, "write Containment parent");
            }
        }
        return std::nullopt;
    }

    static auto
    subtreeEnd(const std::vector<OutlineEntryRecord>& entries, std::size_t root)
        -> std::size_t
    {
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
                const auto found = std::ranges::find_if(
                    entries, [&](const auto& candidate) -> bool {
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

    auto
    touchContainer(MDB_txn* transaction, const JournalDatabases& databases,
                   LoadedOutline& outline, std::optional<BlockId> parent,
                   BlockTimestamp now) const -> std::optional<NotebookError>
    {
        if (!parent) {
            outline.page.metadata.updatedAt = now;
            return writeBlock(transaction, databases.blocks, outline.page,
                              info.value_or(NotebookInfo{}).path);
        }
        const auto found = std::ranges::find_if(
            outline.entries, [&](const auto& entry) -> bool {
                return entry.metadata.id == *parent;
            });
        if (found == outline.entries.end()) {
            return makeError(NotebookErrorCode::invalidNotebook,
                             info.value_or(NotebookInfo{}).path,
                             "Journal parent is outside its Page");
        }
        auto loaded = readBlock(transaction, databases.blocks, *parent,
                                info.value_or(NotebookInfo{}).path);
        if (!loaded) {
            return loaded.error();
        }
        auto block = std::move(loaded).value();
        block.metadata.updatedAt = now;
        found->metadata.updatedAt = now;
        return writeBlock(transaction, databases.blocks, block,
                          info.value_or(NotebookInfo{}).path);
    }

    auto
    loadOutlineForDate(MDB_txn* transaction, const JournalDatabases& databases,
                       JournalDate date) const
        -> Result<std::optional<LoadedOutline>>
    {
        const auto path = info.value_or(NotebookInfo{}).path;
        const auto encodedDate = dateKey(date);
        MDB_val key{encodedDate.size(),
                    const_cast<std::uint8_t*>(encodedDate.data())};
        MDB_val value{};
        const auto result =
            mdb_get(transaction, databases.journalByDate, &key, &value);
        if (result == MDB_NOTFOUND) {
            return Result<std::optional<LoadedOutline>>::success(std::nullopt);
        }
        if (result != MDB_SUCCESS || value.mv_size != BlockId{}.bytes.size()) {
            return Result<std::optional<LoadedOutline>>::failure(
                result == MDB_SUCCESS
                    ? makeError(NotebookErrorCode::invalidNotebook, path,
                                "invalid Journal date index")
                    : errorFromLmdb(path, result, "read Journal date index"));
        }
        BlockId pageId;
        std::memcpy(pageId.bytes.data(), value.mv_data, pageId.bytes.size());
        auto page = readBlock(transaction, databases.blocks, pageId, path);
        if (!page || page.value().type != BlockType::page ||
            page.value().pageKind != PageKind::journal ||
            page.value().journalDate != date) {
            return Result<std::optional<LoadedOutline>>::failure(
                page ? makeError(NotebookErrorCode::invalidNotebook, path,
                                 "Journal date points to an invalid Page")
                     : page.error());
        }
        auto outline =
            loadOutline(transaction, databases, std::move(page).value());
        if (!outline) {
            return Result<std::optional<LoadedOutline>>::failure(
                outline.error());
        }
        return Result<std::optional<LoadedOutline>>::success(
            std::move(outline).value());
    }

    static auto
    estimateOutlineBytes(const std::optional<LoadedOutline>& outline)
        -> std::size_t
    {
        if (!outline) {
            return sizeof(std::optional<LoadedOutline>);
        }
        auto bytes = sizeof(LoadedOutline) + outline->page.authoredText.size();
        for (const auto& entry : outline->entries) {
            bytes += sizeof(OutlineEntryRecord) + entry.authoredText.size();
        }
        return bytes;
    }

    auto
    pageHistory(JournalDate date) -> JournalPageHistory&
    {
        const auto found = std::ranges::find_if(
            journalHistory,
            [&](const auto& history) -> bool { return history.date == date; });
        if (found != journalHistory.end()) {
            return *found;
        }
        return journalHistory.emplace_back(JournalPageHistory{date, {}, {}});
    }

    auto
    pageHistory(JournalDate date) const -> const JournalPageHistory*
    {
        const auto found = std::ranges::find_if(
            journalHistory,
            [&](const auto& history) -> bool { return history.date == date; });
        return found == journalHistory.end() ? nullptr : &*found;
    }

    auto
    historyActionCount() const -> std::size_t
    {
        std::size_t count = crossPageUndo.size() + crossPageRedo.size();
        for (const auto& history : journalHistory) {
            count += history.undo.size() + history.redo.size();
        }
        for (const auto& history : pageHistories) {
            count += history.undo.size() + history.redo.size();
        }
        return count;
    }

    void
    clearRedoHistory()
    {
        for (auto& history : journalHistory) {
            for (const auto& action : history.redo) {
                historyBytes -= action.estimatedBytes;
            }
            history.redo.clear();
        }
        for (auto& history : pageHistories) {
            for (const auto& action : history.redo) {
                historyBytes -= action.estimatedBytes;
            }
            history.redo.clear();
        }
        for (const auto& action : crossPageRedo) {
            historyBytes -= action.estimatedBytes;
        }
        crossPageRedo.clear();
    }

    void
    enforceHistoryBudget()
    {
        constexpr std::size_t historyBudget = 32ULL * 1024ULL * 1024ULL;
        while (historyBytes > historyBudget && historyActionCount() > 1) {
            JournalPageHistory* oldestHistory = nullptr;
            PageHistory* oldestPageHistory = nullptr;
            bool oldestIsRedo = false;
            bool oldestIsCrossPage = false;
            auto oldestSequence = std::numeric_limits<std::uint64_t>::max();
            for (auto& history : journalHistory) {
                if (!history.undo.empty() &&
                    history.undo.front().sequence < oldestSequence) {
                    oldestHistory = &history;
                    oldestPageHistory = nullptr;
                    oldestIsRedo = false;
                    oldestIsCrossPage = false;
                    oldestSequence = history.undo.front().sequence;
                }
                if (!history.redo.empty() &&
                    history.redo.back().sequence < oldestSequence) {
                    oldestHistory = &history;
                    oldestPageHistory = nullptr;
                    oldestIsRedo = true;
                    oldestIsCrossPage = false;
                    oldestSequence = history.redo.back().sequence;
                }
            }
            for (auto& history : pageHistories) {
                if (!history.undo.empty() &&
                    history.undo.front().sequence < oldestSequence) {
                    oldestHistory = nullptr;
                    oldestPageHistory = &history;
                    oldestIsRedo = false;
                    oldestIsCrossPage = false;
                    oldestSequence = history.undo.front().sequence;
                }
                if (!history.redo.empty() &&
                    history.redo.back().sequence < oldestSequence) {
                    oldestHistory = nullptr;
                    oldestPageHistory = &history;
                    oldestIsRedo = true;
                    oldestIsCrossPage = false;
                    oldestSequence = history.redo.back().sequence;
                }
            }
            if (!crossPageUndo.empty() &&
                crossPageUndo.front().sequence < oldestSequence) {
                oldestHistory = nullptr;
                oldestPageHistory = nullptr;
                oldestIsRedo = false;
                oldestIsCrossPage = true;
                oldestSequence = crossPageUndo.front().sequence;
            }
            if (!crossPageRedo.empty() &&
                crossPageRedo.back().sequence < oldestSequence) {
                oldestHistory = nullptr;
                oldestPageHistory = nullptr;
                oldestIsRedo = true;
                oldestIsCrossPage = true;
            }
            if (oldestHistory == nullptr && oldestPageHistory == nullptr &&
                !oldestIsCrossPage) {
                break;
            }
            if (oldestIsCrossPage && oldestIsRedo) {
                for (const auto& action : crossPageRedo) {
                    historyBytes -= action.estimatedBytes;
                }
                crossPageRedo.clear();
            } else if (oldestIsCrossPage) {
                historyBytes -= crossPageUndo.front().estimatedBytes;
                crossPageUndo.pop_front();
            } else if (oldestHistory != nullptr && oldestIsRedo) {
                for (const auto& action : oldestHistory->redo) {
                    historyBytes -= action.estimatedBytes;
                }
                oldestHistory->redo.clear();
            } else if (oldestHistory != nullptr) {
                historyBytes -= oldestHistory->undo.front().estimatedBytes;
                oldestHistory->undo.pop_front();
            } else if (oldestIsRedo) {
                for (const auto& action : oldestPageHistory->redo) {
                    historyBytes -= action.estimatedBytes;
                }
                oldestPageHistory->redo.clear();
            } else {
                historyBytes -= oldestPageHistory->undo.front().estimatedBytes;
                oldestPageHistory->undo.pop_front();
            }
        }
    }

    void
    recordHistory(JournalDate date, std::optional<LoadedOutline> before,
                  std::optional<LoadedOutline> after)
    {
        clearRedoHistory();
        auto& history = pageHistory(date);
        JournalHistoryAction action{nextHistorySequence++, date,
                                    std::move(before), std::move(after), 0};
        action.estimatedBytes = sizeof(JournalHistoryAction) +
                                estimateOutlineBytes(action.before) +
                                estimateOutlineBytes(action.after);
        historyBytes += action.estimatedBytes;
        history.undo.push_back(std::move(action));
        enforceHistoryBudget();
    }

    auto
    removeTypeIndex(MDB_txn* transaction, MDB_dbi database, BlockType type,
                    const BlockId& identifier) const
        -> std::optional<NotebookError>
    {
        auto bytes = typeIndexKey(type, identifier);
        MDB_val key{bytes.size(), bytes.data()};
        const auto result = mdb_del(transaction, database, &key, nullptr);
        if (result != MDB_SUCCESS && result != MDB_NOTFOUND) {
            return errorFromLmdb(info.value_or(NotebookInfo{}).path, result,
                                 "remove Block type index");
        }
        return std::nullopt;
    }

    auto
    restoreOutline(MDB_txn* transaction, const JournalDatabases& databases,
                   JournalDate date, const std::optional<LoadedOutline>& target)
        -> std::optional<NotebookError>
    {
        const auto path = info.value_or(NotebookInfo{}).path;
        auto currentResult = loadOutlineForDate(transaction, databases, date);
        if (!currentResult) {
            return currentResult.error();
        }
        auto current = std::move(currentResult).value();
        if (current) {
            for (const auto& entry : current->entries) {
                if (auto error =
                        eraseContainment(transaction, databases, entry)) {
                    return error;
                }
                auto key = blockKey(entry.metadata.id);
                auto result =
                    mdb_del(transaction, databases.blocks, &key, nullptr);
                if (result != MDB_SUCCESS) {
                    return errorFromLmdb(path, result,
                                         "remove Journal Entry for history");
                }
                if (auto error = removeTypeIndex(
                        transaction, databases.blocksByType,
                        BlockType::journalEntry, entry.metadata.id)) {
                    return error;
                }
            }
            auto pageKey = blockKey(current->page.metadata.id);
            auto result =
                mdb_del(transaction, databases.blocks, &pageKey, nullptr);
            if (result != MDB_SUCCESS) {
                return errorFromLmdb(path, result,
                                     "remove Journal Page for history");
            }
            if (auto error = removeTypeIndex(
                    transaction, databases.blocksByType, BlockType::journalPage,
                    current->page.metadata.id)) {
                return error;
            }
        }
        const auto encodedDate = dateKey(date);
        MDB_val dateKeyValue{encodedDate.size(),
                             const_cast<std::uint8_t*>(encodedDate.data())};
        auto result = mdb_del(transaction, databases.journalByDate,
                              &dateKeyValue, nullptr);
        if (result != MDB_SUCCESS && result != MDB_NOTFOUND) {
            return errorFromLmdb(path, result,
                                 "remove Journal date for history");
        }
        if (!target) {
            return std::nullopt;
        }
        if (auto error =
                writeBlock(transaction, databases.blocks, target->page, path)) {
            return error;
        }
        if (auto error = writeTypeIndex(transaction, databases.blocksByType,
                                        BlockType::journalPage,
                                        target->page.metadata.id, path)) {
            return error;
        }
        MDB_val pageValue{
            target->page.metadata.id.bytes.size(),
            const_cast<std::byte*>(target->page.metadata.id.bytes.data())};
        result = mdb_put(transaction, databases.journalByDate, &dateKeyValue,
                         &pageValue, MDB_NOOVERWRITE);
        if (result != MDB_SUCCESS) {
            return errorFromLmdb(path, result,
                                 "restore Journal date for history");
        }
        for (const auto& entry : target->entries) {
            const BlockRecord block{BlockType::entry,
                                    entry.metadata,
                                    std::nullopt,
                                    entry.authoredText,
                                    {},
                                    {},
                                    std::nullopt};
            if (auto error =
                    writeBlock(transaction, databases.blocks, block, path)) {
                return error;
            }
            if (auto error = writeTypeIndex(transaction, databases.blocksByType,
                                            BlockType::journalEntry,
                                            entry.metadata.id, path)) {
                return error;
            }
        }
        const LoadedOutline emptyBefore{target->page, {}};
        return rewriteContainment(transaction, databases, emptyBefore, *target);
    }

    auto
    applyHistory(JournalDate date, bool redo) -> Result<JournalPage>
    {
        lastCommandCommitted = false;
        auto& history = pageHistory(date);
        auto& source = redo ? history.redo : history.undo;
        if (source.empty()) {
            return Result<JournalPage>::failure(
                makeError(redo ? NotebookErrorCode::redoUnavailable
                               : NotebookErrorCode::undoUnavailable,
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
            return Result<JournalPage>::failure(errorFromLmdb(
                path, result,
                redo ? "begin Journal redo" : "begin Journal undo"));
        }
        const auto fail = [&](NotebookError error) -> Result<JournalPage> {
            mdb_txn_abort(transaction);
            return Result<JournalPage>::failure(std::move(error));
        };
        auto databases = openJournalDatabases(transaction, path);
        if (!databases) {
            return fail(databases.error());
        }
        if (auto error =
                restoreOutline(transaction, databases.value(), date, target)) {
            return fail(std::move(*error));
        }
        if (auto error = incrementRevision(transaction,
                                           databases.value().metadata, path)) {
            return fail(std::move(*error));
        }
        result = commitAdapter->commit(transaction);
        if (result != MDB_SUCCESS) {
            return Result<JournalPage>::failure(errorFromLmdb(
                path, result,
                redo ? "commit Journal redo" : "commit Journal undo"));
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
        return Result<JournalPage>::success(
            {date, target->page.metadata,
             publicJournalEntries(target->entries)});
    }

    static auto
    publicPage(LoadedOutline outline) -> Page
    {
        Page result{outline.page.metadata,
                    outline.page.pageName,
                    outline.page.displayTitle,
                    {}};
        result.entries.reserve(outline.entries.size());
        for (auto& entry : outline.entries) {
            result.entries.push_back(Entry{entry.metadata,
                                           std::move(entry.authoredText),
                                           entry.parentEntry});
        }
        return result;
    }

    auto
    readPage(BlockId pageId) const -> Result<Page>
    {
        const auto path = info.value_or(NotebookInfo{}).path;
        MDB_txn* transaction = nullptr;
        auto result =
            mdb_txn_begin(environment, nullptr, MDB_RDONLY, &transaction);
        if (result != MDB_SUCCESS) {
            return Result<Page>::failure(
                errorFromLmdb(path, result, "begin Page read"));
        }
        auto databases = openJournalDatabases(transaction, path);
        if (!databases) {
            mdb_txn_abort(transaction);
            return Result<Page>::failure(databases.error());
        }
        auto loaded =
            readBlock(transaction, databases.value().blocks, pageId, path);
        if (!loaded || loaded.value().type != BlockType::page ||
            loaded.value().pageKind != PageKind::named) {
            mdb_txn_abort(transaction);
            if (!loaded &&
                loaded.error().code != NotebookErrorCode::blockNotFound) {
                return Result<Page>::failure(loaded.error());
            }
            return Result<Page>::failure(makeError(
                NotebookErrorCode::pageNotFound, path, "Block is not a Page"));
        }
        auto outline = loadOutline(transaction, databases.value(),
                                   std::move(loaded).value());
        mdb_txn_abort(transaction);
        if (!outline) {
            return Result<Page>::failure(outline.error());
        }
        return Result<Page>::success(publicPage(std::move(outline).value()));
    }

    auto
    readPageLinks(BlockId entryId) const -> Result<std::vector<PageLink>>
    {
        const auto path = info.value_or(NotebookInfo{}).path;
        MDB_txn* transaction = nullptr;
        auto result =
            mdb_txn_begin(environment, nullptr, MDB_RDONLY, &transaction);
        if (result != MDB_SUCCESS) {
            return Result<std::vector<PageLink>>::failure(
                errorFromLmdb(path, result, "begin Page Link read"));
        }
        auto databases = openJournalDatabases(transaction, path);
        if (!databases) {
            mdb_txn_abort(transaction);
            return Result<std::vector<PageLink>>::failure(databases.error());
        }
        auto entry =
            readBlock(transaction, databases.value().blocks, entryId, path);
        if (!entry || entry.value().type != BlockType::entry) {
            mdb_txn_abort(transaction);
            return Result<std::vector<PageLink>>::failure(
                entry ? makeError(NotebookErrorCode::blockNotFound, path,
                                  "Block is not an Entry")
                      : entry.error());
        }
        auto sourceKey = blockKey(entryId);
        MDB_val encoded{};
        result = mdb_get(transaction, databases.value().referencesBySource,
                         &sourceKey, &encoded);
        if (result == MDB_NOTFOUND) {
            mdb_txn_abort(transaction);
            return Result<std::vector<PageLink>>::success({});
        }
        if (result != MDB_SUCCESS || encoded.mv_size < 6) {
            mdb_txn_abort(transaction);
            return Result<std::vector<PageLink>>::failure(
                result == MDB_SUCCESS
                    ? makeError(NotebookErrorCode::invalidNotebook, path,
                                "Page Link source index is truncated")
                    : errorFromLmdb(path, result,
                                    "read Page Link source index"));
        }
        const auto* bytes = static_cast<const std::uint8_t*>(encoded.mv_data);
        if (readU16(bytes) != 1) {
            mdb_txn_abort(transaction);
            return Result<std::vector<PageLink>>::failure(
                makeError(NotebookErrorCode::invalidNotebook, path,
                          "unsupported Page Link index version"));
        }
        const auto count = readU32(bytes + 2);
        std::size_t offset = 6;
        std::vector<PageLink> links;
        links.reserve(count);
        for (std::uint32_t index = 0; index < count; ++index) {
            if (encoded.mv_size - offset < 11) {
                mdb_txn_abort(transaction);
                return Result<std::vector<PageLink>>::failure(
                    makeError(NotebookErrorCode::invalidNotebook, path,
                              "Page Link occurrence is truncated"));
            }
            const auto sourceOffset = readU32(bytes + offset);
            const auto sourceLength = readU32(bytes + offset + 4);
            const auto nameLength = readU16(bytes + offset + 8);
            offset += 10;
            if (encoded.mv_size - offset <
                static_cast<std::size_t>(nameLength) + 1) {
                mdb_txn_abort(transaction);
                return Result<std::vector<PageLink>>::failure(
                    makeError(NotebookErrorCode::invalidNotebook, path,
                              "Page Link name is truncated"));
            }
            std::string pageName(reinterpret_cast<const char*>(bytes + offset),
                                 nameLength);
            offset += nameLength;
            const auto resolved = bytes[offset++] != 0;
            std::optional<PageSummary> target;
            if (resolved) {
                if (encoded.mv_size - offset < BlockId{}.bytes.size()) {
                    mdb_txn_abort(transaction);
                    return Result<std::vector<PageLink>>::failure(
                        makeError(NotebookErrorCode::invalidNotebook, path,
                                  "Page Link target is truncated"));
                }
                BlockId targetId;
                std::memcpy(targetId.bytes.data(), bytes + offset,
                            targetId.bytes.size());
                offset += targetId.bytes.size();
                auto block = readBlock(transaction, databases.value().blocks,
                                       targetId, path);
                if (!block || block.value().type != BlockType::page ||
                    block.value().pageKind != PageKind::named) {
                    mdb_txn_abort(transaction);
                    return Result<std::vector<PageLink>>::failure(
                        block ? makeError(
                                    NotebookErrorCode::invalidNotebook, path,
                                    "Page Link target is not a Named Page")
                              : block.error());
                }
                target =
                    PageSummary{block.value().metadata, block.value().pageName,
                                block.value().displayTitle};
            }
            links.push_back({sourceOffset, sourceLength, std::move(pageName),
                             std::move(target)});
        }
        if (offset != encoded.mv_size) {
            mdb_txn_abort(transaction);
            return Result<std::vector<PageLink>>::failure(
                makeError(NotebookErrorCode::invalidNotebook, path,
                          "Page Link source index has trailing bytes"));
        }
        mdb_txn_abort(transaction);
        return Result<std::vector<PageLink>>::success(std::move(links));
    }

    auto
    readBlockReferences(BlockId entryId) const
        -> Result<std::vector<BlockReference>>
    {
        const auto path = info.value_or(NotebookInfo{}).path;
        MDB_txn* transaction = nullptr;
        auto result =
            mdb_txn_begin(environment, nullptr, MDB_RDONLY, &transaction);
        if (result != MDB_SUCCESS) {
            return Result<std::vector<BlockReference>>::failure(
                errorFromLmdb(path, result, "begin Block Reference read"));
        }
        auto databases = openJournalDatabases(transaction, path);
        if (!databases) {
            mdb_txn_abort(transaction);
            return Result<std::vector<BlockReference>>::failure(
                databases.error());
        }
        auto entry =
            readBlock(transaction, databases.value().blocks, entryId, path);
        if (!entry || entry.value().type != BlockType::entry) {
            mdb_txn_abort(transaction);
            return Result<std::vector<BlockReference>>::failure(
                entry ? makeError(NotebookErrorCode::blockNotFound, path,
                                  "Block is not an Entry")
                      : entry.error());
        }
        std::vector<BlockReference> references;
        for (const auto& occurrence :
             authored_text::blockReferences(entry.value().authoredText)) {
            const auto targetId = occurrence.targetId;
            std::optional<BlockMetadata> target;
            auto targetBlock = readBlock(transaction, databases.value().blocks,
                                         targetId, path);
            if (targetBlock) {
                target = targetBlock.value().metadata;
            } else if (targetBlock.error().code !=
                       NotebookErrorCode::blockNotFound) {
                mdb_txn_abort(transaction);
                return Result<std::vector<BlockReference>>::failure(
                    targetBlock.error());
            }
            references.push_back({occurrence.sourceByteOffset,
                                  occurrence.sourceByteLength, targetId,
                                  std::move(target)});
        }
        mdb_txn_abort(transaction);
        return Result<std::vector<BlockReference>>::success(
            std::move(references));
    }

    auto
    readEntry(BlockId entryId) const -> Result<Entry>
    {
        const auto path = info.value_or(NotebookInfo{}).path;
        MDB_txn* transaction = nullptr;
        auto result =
            mdb_txn_begin(environment, nullptr, MDB_RDONLY, &transaction);
        if (result != MDB_SUCCESS) {
            return Result<Entry>::failure(
                errorFromLmdb(path, result, "begin Entry read"));
        }
        auto databases = openJournalDatabases(transaction, path);
        if (!databases) {
            mdb_txn_abort(transaction);
            return Result<Entry>::failure(databases.error());
        }
        auto block =
            readBlock(transaction, databases.value().blocks, entryId, path);
        if (!block || block.value().type != BlockType::entry) {
            mdb_txn_abort(transaction);
            return Result<Entry>::failure(
                block ? makeError(NotebookErrorCode::blockNotFound, path,
                                  "Block is not an Entry")
                      : block.error());
        }
        auto parent = parentOf(transaction, databases.value(), entryId);
        if (!parent) {
            mdb_txn_abort(transaction);
            return Result<Entry>::failure(parent.error());
        }
        auto parentBlock = readBlock(transaction, databases.value().blocks,
                                     parent.value().parent, path);
        if (!parentBlock) {
            mdb_txn_abort(transaction);
            return Result<Entry>::failure(parentBlock.error());
        }
        const auto parentEntry =
            parentBlock.value().type == BlockType::entry
                ? std::optional<BlockId>{parent.value().parent}
                : std::nullopt;
        Entry entry{block.value().metadata, block.value().authoredText,
                    parentEntry};
        mdb_txn_abort(transaction);
        return Result<Entry>::success(std::move(entry));
    }

    auto
    blockReferenceDestination(BlockId targetId) const
        -> Result<BlockReferenceDestination>
    {
        const auto path = info.value_or(NotebookInfo{}).path;
        MDB_txn* transaction = nullptr;
        auto result =
            mdb_txn_begin(environment, nullptr, MDB_RDONLY, &transaction);
        if (result != MDB_SUCCESS) {
            return Result<BlockReferenceDestination>::failure(errorFromLmdb(
                path, result, "begin Block Reference target read"));
        }
        const auto fail =
            [&](NotebookError error) -> Result<BlockReferenceDestination> {
            mdb_txn_abort(transaction);
            return Result<BlockReferenceDestination>::failure(std::move(error));
        };
        auto databases = openJournalDatabases(transaction, path);
        if (!databases) {
            return fail(databases.error());
        }
        auto target =
            readBlock(transaction, databases.value().blocks, targetId, path);
        if (!target) {
            return fail(target.error());
        }
        LoadedOutline outline;
        std::vector<BlockId> pathIds;
        if (target.value().type == BlockType::page) {
            auto loaded =
                loadOutline(transaction, databases.value(), target.value());
            if (!loaded) {
                return fail(loaded.error());
            }
            outline = std::move(loaded).value();
        } else {
            auto loaded =
                loadOutlineForEntry(transaction, databases.value(), targetId);
            if (!loaded) {
                return fail(loaded.error());
            }
            outline = std::move(loaded).value();
            auto current = targetId;
            while (true) {
                pathIds.push_back(current);
                auto parent = parentOf(transaction, databases.value(), current);
                if (!parent) {
                    return fail(parent.error());
                }
                auto parentBlock =
                    readBlock(transaction, databases.value().blocks,
                              parent.value().parent, path);
                if (!parentBlock) {
                    return fail(parentBlock.error());
                }
                if (parentBlock.value().type == BlockType::page) {
                    break;
                }
                current = parent.value().parent;
            }
            std::ranges::reverse(pathIds);
        }
        auto destination = BlockReferenceDestination{target.value().metadata,
                                                     publicOutline(outline),
                                                     std::move(pathIds)};
        mdb_txn_abort(transaction);
        return Result<BlockReferenceDestination>::success(
            std::move(destination));
    }

    auto
    readLinkedReferences(BlockId targetId,
                         const std::optional<std::string>& cursorText) const
        -> Result<LinkedReferencesBatch>
    {
        const auto path = info.value_or(NotebookInfo{}).path;
        std::size_t batchOffset = 0;
        if (cursorText) {
            const auto cursor = LinkedReferencesCursor::decode(*cursorText);
            if (!cursor ||
                cursor->revision != info.value_or(NotebookInfo{}).revision) {
                return Result<LinkedReferencesBatch>::failure(makeError(
                    NotebookErrorCode::staleLinkedReferencesCursor, path,
                    "Linked References cursor is stale or invalid"));
            }
            batchOffset = cursor->offset;
        }

        MDB_txn* transaction = nullptr;
        auto result =
            mdb_txn_begin(environment, nullptr, MDB_RDONLY, &transaction);
        if (result != MDB_SUCCESS) {
            return Result<LinkedReferencesBatch>::failure(
                errorFromLmdb(path, result, "begin Linked References read"));
        }
        const auto fail =
            [&](NotebookError error) -> Result<LinkedReferencesBatch> {
            mdb_txn_abort(transaction);
            return Result<LinkedReferencesBatch>::failure(std::move(error));
        };
        auto databases = openJournalDatabases(transaction, path);
        if (!databases) {
            return fail(databases.error());
        }
        auto target =
            readBlock(transaction, databases.value().blocks, targetId, path);
        if (!target) {
            return fail(target.error());
        }

        std::vector<BlockId> sourceIds;
        const auto collectSources = [&](const std::vector<std::uint8_t>& prefix)
            -> std::optional<NotebookError> {
            MDB_cursor* cursor = nullptr;
            auto scan = mdb_cursor_open(
                transaction, databases.value().referencesByTarget, &cursor);
            if (scan != MDB_SUCCESS) {
                return errorFromLmdb(path, scan, "open Linked References scan");
            }
            MDB_val key{prefix.size(),
                        const_cast<std::uint8_t*>(prefix.data())};
            MDB_val value{};
            scan = mdb_cursor_get(cursor, &key, &value, MDB_SET_RANGE);
            while (scan == MDB_SUCCESS &&
                   key.mv_size == prefix.size() + BlockId{}.bytes.size() &&
                   std::memcmp(key.mv_data, prefix.data(), prefix.size()) ==
                       0) {
                BlockId sourceId;
                std::memcpy(sourceId.bytes.data(),
                            static_cast<const std::uint8_t*>(key.mv_data) +
                                prefix.size(),
                            sourceId.bytes.size());
                if (std::ranges::find(sourceIds, sourceId) == sourceIds.end()) {
                    sourceIds.push_back(sourceId);
                }
                scan = mdb_cursor_get(cursor, &key, &value, MDB_NEXT);
            }
            mdb_cursor_close(cursor);
            if (scan != MDB_SUCCESS && scan != MDB_NOTFOUND) {
                return errorFromLmdb(path, scan, "scan Linked References");
            }
            return std::nullopt;
        };
        if (target.value().type == BlockType::page &&
            target.value().pageKind == PageKind::named) {
            if (auto error =
                    collectSources(resolvedPageLinkTargetPrefix(targetId))) {
                return fail(std::move(*error));
            }
        }
        if (auto error =
                collectSources(blockReferenceTargetPrefix(targetId, true))) {
            return fail(std::move(*error));
        }

        struct SourceCandidate {
            BlockId sourceId;
            BlockId pageId;
            BlockTimestamp updatedAt;
            std::vector<std::uint64_t> containmentRanks;
        };
        std::vector<SourceCandidate> candidates;
        candidates.reserve(sourceIds.size());
        for (const auto& sourceId : sourceIds) {
            auto source = readBlock(transaction, databases.value().blocks,
                                    sourceId, path);
            if (!source) {
                return fail(source.error());
            }
            SourceCandidate candidate{
                sourceId, {}, source.value().metadata.updatedAt, {}};
            auto current = sourceId;
            while (true) {
                auto parent = parentOf(transaction, databases.value(), current);
                if (!parent) {
                    return fail(parent.error());
                }
                candidate.containmentRanks.push_back(parent.value().rank);
                auto parentBlock =
                    readBlock(transaction, databases.value().blocks,
                              parent.value().parent, path);
                if (!parentBlock) {
                    return fail(parentBlock.error());
                }
                if (parentBlock.value().type == BlockType::page) {
                    candidate.pageId = parentBlock.value().metadata.id;
                    break;
                }
                current = parent.value().parent;
            }
            std::ranges::reverse(candidate.containmentRanks);
            candidates.push_back(std::move(candidate));
        }
        std::unordered_map<std::string, BlockTimestamp> newestSourceByPage;
        for (const auto& candidate : candidates) {
            const auto pageKey = candidate.pageId.toString();
            const auto found = newestSourceByPage.find(pageKey);
            if (found == newestSourceByPage.end() ||
                found->second < candidate.updatedAt) {
                newestSourceByPage[pageKey] = candidate.updatedAt;
            }
        }
        std::ranges::sort(
            candidates, [&](const auto& left, const auto& right) -> bool {
                const auto leftPage = left.pageId.toString();
                const auto rightPage = right.pageId.toString();
                if (leftPage != rightPage) {
                    if (newestSourceByPage.at(leftPage) !=
                        newestSourceByPage.at(rightPage)) {
                        return newestSourceByPage.at(leftPage) >
                               newestSourceByPage.at(rightPage);
                    }
                    return leftPage < rightPage;
                }
                return std::ranges::lexicographical_compare(
                    left.containmentRanks, right.containmentRanks);
            });
        const auto total = candidates.size();
        if (batchOffset > total) {
            return fail(makeError(
                NotebookErrorCode::staleLinkedReferencesCursor, path,
                "Linked References cursor is outside the result set"));
        }
        constexpr std::size_t batchSize = 100;
        const auto batchEnd = std::min(total, batchOffset + batchSize);

        std::vector<LinkedReferenceSource> sources;
        sources.reserve(batchEnd - batchOffset);
        for (auto index = batchOffset; index < batchEnd; ++index) {
            const auto sourceId = candidates[index].sourceId;
            auto source = readBlock(transaction, databases.value().blocks,
                                    sourceId, path);
            if (!source) {
                return fail(source.error());
            }
            std::vector<LinkedReferenceOccurrence> occurrences;
            if (target.value().type == BlockType::page &&
                target.value().pageKind == PageKind::named) {
                for (const auto& link :
                     authored_text::pageLinks(source.value().authoredText)) {
                    if (link.pageName == target.value().pageName) {
                        occurrences.push_back({SemanticReferenceKind::pageLink,
                                               link.sourceByteOffset,
                                               link.sourceByteLength});
                    }
                }
            }
            for (const auto& reference :
                 authored_text::blockReferences(source.value().authoredText)) {
                if (reference.targetId == targetId) {
                    occurrences.push_back(
                        {SemanticReferenceKind::blockReference,
                         reference.sourceByteOffset,
                         reference.sourceByteLength});
                }
            }
            std::ranges::sort(occurrences, {},
                              &LinkedReferenceOccurrence::sourceByteOffset);
            const auto occurrenceCount = occurrences.size();
            if (occurrences.size() > 3) {
                occurrences.resize(3);
            }
            auto loaded =
                loadOutlineForEntry(transaction, databases.value(), sourceId);
            if (!loaded) {
                return fail(loaded.error());
            }
            auto outline = std::move(loaded).value();
            std::vector<BlockId> containmentPath;
            auto current = sourceId;
            while (true) {
                containmentPath.push_back(current);
                auto parent = parentOf(transaction, databases.value(), current);
                if (!parent) {
                    return fail(parent.error());
                }
                auto parentBlock =
                    readBlock(transaction, databases.value().blocks,
                              parent.value().parent, path);
                if (!parentBlock) {
                    return fail(parentBlock.error());
                }
                if (parentBlock.value().type == BlockType::page) {
                    break;
                }
                current = parent.value().parent;
            }
            std::ranges::reverse(containmentPath);
            const auto parentEntry =
                containmentPath.size() > 1
                    ? std::optional<
                          BlockId>{containmentPath[containmentPath.size() - 2]}
                    : std::nullopt;
            sources.push_back({Entry{source.value().metadata,
                                     source.value().authoredText, parentEntry},
                               publicOutline(outline),
                               std::move(containmentPath),
                               std::move(occurrences), occurrenceCount});
        }
        std::optional<std::string> nextCursor;
        if (batchEnd < total) {
            nextCursor =
                LinkedReferencesCursor{info.value_or(NotebookInfo{}).revision,
                                       batchEnd}
                    .encode();
        }
        mdb_txn_abort(transaction);
        return Result<LinkedReferencesBatch>::success(
            {std::move(sources), total, std::move(nextCursor)});
    }

    auto
    readPagePreview(const std::string& name) const -> Result<PagePreview>
    {
        const auto path = info.value_or(NotebookInfo{}).path;
        MDB_txn* transaction = nullptr;
        auto result =
            mdb_txn_begin(environment, nullptr, MDB_RDONLY, &transaction);
        if (result != MDB_SUCCESS) {
            return Result<PagePreview>::failure(
                errorFromLmdb(path, result, "begin Page Preview read"));
        }
        const auto fail = [&](NotebookError error) -> Result<PagePreview> {
            mdb_txn_abort(transaction);
            return Result<PagePreview>::failure(std::move(error));
        };
        auto databases = openJournalDatabases(transaction, path);
        if (!databases) {
            return fail(databases.error());
        }
        auto materialized =
            namedPageByName(transaction, databases.value(), name);
        if (!materialized) {
            return fail(materialized.error());
        }
        if (materialized.value()) {
            return fail(makeError(NotebookErrorCode::pageNameConflict, path,
                                  "Page Preview name is materialized"));
        }

        PagePreview preview{name, {}};
        MDB_cursor* cursor = nullptr;
        result = mdb_cursor_open(transaction,
                                 databases.value().referencesByTarget, &cursor);
        if (result != MDB_SUCCESS) {
            return fail(
                errorFromLmdb(path, result, "open Page Preview source scan"));
        }
        auto prefix = unresolvedPageLinkTargetPrefix(name);
        MDB_val key{prefix.size(), prefix.data()};
        MDB_val value{};
        result = mdb_cursor_get(cursor, &key, &value, MDB_SET_RANGE);
        while (result == MDB_SUCCESS &&
               key.mv_size == prefix.size() + BlockId{}.bytes.size() &&
               std::memcmp(key.mv_data, prefix.data(), prefix.size()) == 0) {
            BlockId sourceId;
            std::memcpy(sourceId.bytes.data(),
                        static_cast<const std::uint8_t*>(key.mv_data) +
                            prefix.size(),
                        sourceId.bytes.size());
            auto source = readBlock(transaction, databases.value().blocks,
                                    sourceId, path);
            if (!source) {
                mdb_cursor_close(cursor);
                return fail(source.error());
            }
            auto parent = parentOf(transaction, databases.value(), sourceId);
            if (!parent) {
                mdb_cursor_close(cursor);
                return fail(parent.error());
            }
            auto parentBlock = readBlock(transaction, databases.value().blocks,
                                         parent.value().parent, path);
            if (!parentBlock) {
                mdb_cursor_close(cursor);
                return fail(parentBlock.error());
            }
            const auto parentEntry =
                parentBlock.value().type == BlockType::entry
                    ? std::optional<BlockId>{parent.value().parent}
                    : std::nullopt;
            preview.sources.push_back({source.value().metadata,
                                       source.value().authoredText,
                                       parentEntry});
            result = mdb_cursor_get(cursor, &key, &value, MDB_NEXT);
        }
        mdb_cursor_close(cursor);
        if (result != MDB_SUCCESS && result != MDB_NOTFOUND) {
            return fail(
                errorFromLmdb(path, result, "scan Page Preview sources"));
        }
        mdb_txn_abort(transaction);
        return Result<PagePreview>::success(std::move(preview));
    }

    auto
    readPagePreviewSources(const std::string& name,
                           const std::optional<std::string>& cursorText) const
        -> Result<UnresolvedPageLinkSourcesBatch>
    {
        const auto path = info.value_or(NotebookInfo{}).path;
        std::size_t batchOffset = 0;
        if (cursorText) {
            const auto cursor = LinkedReferencesCursor::decode(*cursorText);
            if (!cursor ||
                cursor->revision != info.value_or(NotebookInfo{}).revision) {
                return Result<UnresolvedPageLinkSourcesBatch>::failure(
                    makeError(NotebookErrorCode::staleLinkedReferencesCursor,
                              path, "Page Preview cursor is stale or invalid"));
            }
            batchOffset = cursor->offset;
        }
        MDB_txn* transaction = nullptr;
        auto result =
            mdb_txn_begin(environment, nullptr, MDB_RDONLY, &transaction);
        if (result != MDB_SUCCESS) {
            return Result<UnresolvedPageLinkSourcesBatch>::failure(
                errorFromLmdb(path, result,
                              "begin Page Preview Linked References read"));
        }
        const auto fail =
            [&](NotebookError error) -> Result<UnresolvedPageLinkSourcesBatch> {
            mdb_txn_abort(transaction);
            return Result<UnresolvedPageLinkSourcesBatch>::failure(
                std::move(error));
        };
        auto databases = openJournalDatabases(transaction, path);
        if (!databases) {
            return fail(databases.error());
        }
        auto materialized =
            namedPageByName(transaction, databases.value(), name);
        if (!materialized) {
            return fail(materialized.error());
        }
        if (materialized.value()) {
            return fail(makeError(NotebookErrorCode::pageNameConflict, path,
                                  "Page Preview name is materialized"));
        }

        std::vector<BlockId> sourceIds;
        MDB_cursor* cursor = nullptr;
        result = mdb_cursor_open(transaction,
                                 databases.value().referencesByTarget, &cursor);
        if (result != MDB_SUCCESS) {
            return fail(
                errorFromLmdb(path, result, "open Page Preview source scan"));
        }
        auto prefix = unresolvedPageLinkTargetPrefix(name);
        MDB_val key{prefix.size(), prefix.data()};
        MDB_val value{};
        result = mdb_cursor_get(cursor, &key, &value, MDB_SET_RANGE);
        while (result == MDB_SUCCESS &&
               key.mv_size == prefix.size() + BlockId{}.bytes.size() &&
               std::memcmp(key.mv_data, prefix.data(), prefix.size()) == 0) {
            BlockId sourceId;
            std::memcpy(sourceId.bytes.data(),
                        static_cast<const std::uint8_t*>(key.mv_data) +
                            prefix.size(),
                        sourceId.bytes.size());
            sourceIds.push_back(sourceId);
            result = mdb_cursor_get(cursor, &key, &value, MDB_NEXT);
        }
        mdb_cursor_close(cursor);
        if (result != MDB_SUCCESS && result != MDB_NOTFOUND) {
            return fail(
                errorFromLmdb(path, result, "scan Page Preview sources"));
        }

        struct SourceCandidate {
            BlockId sourceId;
            BlockId pageId;
            BlockTimestamp updatedAt;
            std::vector<std::uint64_t> containmentRanks;
        };
        std::vector<SourceCandidate> candidates;
        candidates.reserve(sourceIds.size());
        for (const auto& sourceId : sourceIds) {
            auto source = readBlock(transaction, databases.value().blocks,
                                    sourceId, path);
            if (!source) {
                return fail(source.error());
            }
            SourceCandidate candidate{
                sourceId, {}, source.value().metadata.updatedAt, {}};
            auto current = sourceId;
            while (true) {
                auto parent = parentOf(transaction, databases.value(), current);
                if (!parent) {
                    return fail(parent.error());
                }
                candidate.containmentRanks.push_back(parent.value().rank);
                auto parentBlock =
                    readBlock(transaction, databases.value().blocks,
                              parent.value().parent, path);
                if (!parentBlock) {
                    return fail(parentBlock.error());
                }
                if (parentBlock.value().type == BlockType::page) {
                    candidate.pageId = parentBlock.value().metadata.id;
                    break;
                }
                current = parent.value().parent;
            }
            std::ranges::reverse(candidate.containmentRanks);
            candidates.push_back(std::move(candidate));
        }
        std::unordered_map<std::string, BlockTimestamp> newestSourceByPage;
        for (const auto& candidate : candidates) {
            const auto pageKey = candidate.pageId.toString();
            const auto found = newestSourceByPage.find(pageKey);
            if (found == newestSourceByPage.end() ||
                found->second < candidate.updatedAt) {
                newestSourceByPage[pageKey] = candidate.updatedAt;
            }
        }
        std::ranges::sort(
            candidates, [&](const auto& left, const auto& right) -> bool {
                const auto leftPage = left.pageId.toString();
                const auto rightPage = right.pageId.toString();
                if (leftPage != rightPage) {
                    if (newestSourceByPage.at(leftPage) !=
                        newestSourceByPage.at(rightPage)) {
                        return newestSourceByPage.at(leftPage) >
                               newestSourceByPage.at(rightPage);
                    }
                    return leftPage < rightPage;
                }
                return std::ranges::lexicographical_compare(
                    left.containmentRanks, right.containmentRanks);
            });
        const auto total = candidates.size();
        if (batchOffset > total) {
            return fail(
                makeError(NotebookErrorCode::staleLinkedReferencesCursor, path,
                          "Page Preview cursor is outside the result set"));
        }
        constexpr std::size_t batchSize = 100;
        const auto batchEnd = std::min(total, batchOffset + batchSize);
        std::vector<UnresolvedPageLinkSource> sources;
        sources.reserve(batchEnd - batchOffset);
        for (auto index = batchOffset; index < batchEnd; ++index) {
            const auto sourceId = candidates[index].sourceId;
            auto source = readBlock(transaction, databases.value().blocks,
                                    sourceId, path);
            if (!source) {
                return fail(source.error());
            }
            std::vector<UnresolvedPageLinkOccurrence> occurrences;
            for (const auto& link :
                 authored_text::pageLinks(source.value().authoredText)) {
                if (link.pageName == name) {
                    occurrences.push_back(
                        {link.sourceByteOffset, link.sourceByteLength});
                }
            }
            const auto occurrenceCount = occurrences.size();
            if (occurrences.size() > 3) {
                occurrences.resize(3);
            }
            auto loaded =
                loadOutlineForEntry(transaction, databases.value(), sourceId);
            if (!loaded) {
                return fail(loaded.error());
            }
            auto outline = std::move(loaded).value();
            std::vector<BlockId> containmentPath;
            auto current = sourceId;
            while (true) {
                containmentPath.push_back(current);
                auto parent = parentOf(transaction, databases.value(), current);
                if (!parent) {
                    return fail(parent.error());
                }
                auto parentBlock =
                    readBlock(transaction, databases.value().blocks,
                              parent.value().parent, path);
                if (!parentBlock) {
                    return fail(parentBlock.error());
                }
                if (parentBlock.value().type == BlockType::page) {
                    break;
                }
                current = parent.value().parent;
            }
            std::ranges::reverse(containmentPath);
            const auto parentEntry =
                containmentPath.size() > 1
                    ? std::optional<
                          BlockId>{containmentPath[containmentPath.size() - 2]}
                    : std::nullopt;
            sources.push_back({Entry{source.value().metadata,
                                     source.value().authoredText, parentEntry},
                               publicOutline(outline),
                               std::move(containmentPath),
                               std::move(occurrences), occurrenceCount});
        }
        std::optional<std::string> nextCursor;
        if (batchEnd < total) {
            nextCursor =
                LinkedReferencesCursor{info.value_or(NotebookInfo{}).revision,
                                       batchEnd}
                    .encode();
        }
        mdb_txn_abort(transaction);
        return Result<UnresolvedPageLinkSourcesBatch>::success(
            {std::move(sources), total, std::move(nextCursor)});
    }

    auto
    readPages() const -> Result<std::vector<PageSummary>>
    {
        const auto path = info.value_or(NotebookInfo{}).path;
        MDB_txn* transaction = nullptr;
        auto result =
            mdb_txn_begin(environment, nullptr, MDB_RDONLY, &transaction);
        if (result != MDB_SUCCESS) {
            return Result<std::vector<PageSummary>>::failure(
                errorFromLmdb(path, result, "begin Page list read"));
        }
        auto databases = openJournalDatabases(transaction, path);
        if (!databases) {
            mdb_txn_abort(transaction);
            return Result<std::vector<PageSummary>>::failure(databases.error());
        }
        MDB_cursor* cursor = nullptr;
        result = mdb_cursor_open(transaction, databases.value().blocksByType,
                                 &cursor);
        std::vector<PageSummary> pages;
        auto start = typeIndexKey(BlockType::page, BlockId{});
        MDB_val key{start.size(), start.data()};
        MDB_val value{};
        if (result == MDB_SUCCESS) {
            result = mdb_cursor_get(cursor, &key, &value, MDB_SET_RANGE);
        }
        while (result == MDB_SUCCESS && key.mv_size == 17 &&
               static_cast<const std::uint8_t*>(key.mv_data)[0] ==
                   static_cast<std::uint8_t>(BlockType::page)) {
            BlockId id;
            std::memcpy(id.bytes.data(),
                        static_cast<const std::uint8_t*>(key.mv_data) + 1,
                        id.bytes.size());
            auto block =
                readBlock(transaction, databases.value().blocks, id, path);
            if (!block || block.value().type != BlockType::page) {
                mdb_cursor_close(cursor);
                mdb_txn_abort(transaction);
                return Result<std::vector<PageSummary>>::failure(
                    block ? makeError(NotebookErrorCode::invalidNotebook, path,
                                      "Page type index is invalid")
                          : block.error());
            }
            if (block.value().pageKind == PageKind::named) {
                pages.push_back({block.value().metadata, block.value().pageName,
                                 block.value().displayTitle});
            }
            result = mdb_cursor_get(cursor, &key, &value, MDB_NEXT);
        }
        if (cursor != nullptr) {
            mdb_cursor_close(cursor);
        }
        mdb_txn_abort(transaction);
        if (result != MDB_SUCCESS && result != MDB_NOTFOUND) {
            return Result<std::vector<PageSummary>>::failure(
                errorFromLmdb(path, result, "read Page list"));
        }
        std::ranges::sort(pages, {}, &PageSummary::name);
        return Result<std::vector<PageSummary>>::success(std::move(pages));
    }

    auto
    namedPageByName(MDB_txn* transaction, const JournalDatabases& databases,
                    std::string_view name) const
        -> Result<std::optional<PageSummary>>
    {
        const auto path = info.value_or(NotebookInfo{}).path;
        MDB_val key{name.size(), const_cast<char*>(name.data())};
        MDB_val value{};
        const auto result =
            mdb_get(transaction, databases.pagesByName, &key, &value);
        if (result == MDB_NOTFOUND) {
            return Result<std::optional<PageSummary>>::success(std::nullopt);
        }
        if (result != MDB_SUCCESS) {
            return Result<std::optional<PageSummary>>::failure(
                errorFromLmdb(path, result, "read Page hierarchy name"));
        }
        if (value.mv_size != BlockId{}.bytes.size()) {
            return Result<std::optional<PageSummary>>::failure(
                makeError(NotebookErrorCode::invalidNotebook, path,
                          "invalid Page name index value"));
        }
        BlockId id;
        std::memcpy(id.bytes.data(), value.mv_data, id.bytes.size());
        auto block = readBlock(transaction, databases.blocks, id, path);
        if (!block || block.value().type != BlockType::page ||
            block.value().pageKind != PageKind::named ||
            block.value().pageName != name) {
            return Result<std::optional<PageSummary>>::failure(
                block ? makeError(
                            NotebookErrorCode::invalidNotebook, path,
                            "Page name index points to an invalid Named Page")
                      : block.error());
        }
        return Result<std::optional<PageSummary>>::success(
            PageSummary{block.value().metadata, block.value().pageName,
                        block.value().displayTitle});
    }

    auto
    hierarchyNameHasChildren(MDB_txn* transaction,
                             const JournalDatabases& databases,
                             std::string_view name) const -> Result<bool>
    {
        const auto path = info.value_or(NotebookInfo{}).path;
        const auto prefix = std::string(name) + '/';
        MDB_cursor* cursor = nullptr;
        auto result =
            mdb_cursor_open(transaction, databases.pagesByName, &cursor);
        if (result != MDB_SUCCESS) {
            return Result<bool>::failure(errorFromLmdb(
                path, result, "open Page hierarchy child lookup"));
        }
        MDB_val key{prefix.size(), const_cast<char*>(prefix.data())};
        MDB_val value{};
        result = mdb_cursor_get(cursor, &key, &value, MDB_SET_RANGE);
        const auto hasChildren =
            result == MDB_SUCCESS && key.mv_size >= prefix.size() &&
            std::memcmp(key.mv_data, prefix.data(), prefix.size()) == 0;
        mdb_cursor_close(cursor);
        if (result != MDB_SUCCESS && result != MDB_NOTFOUND) {
            return Result<bool>::failure(errorFromLmdb(
                path, result, "read Page hierarchy child lookup"));
        }
        return Result<bool>::success(hasChildren);
    }

    auto
    readHierarchyNode(std::string name) const
        -> Result<std::optional<PageHierarchyNode>>
    {
        const auto path = info.value_or(NotebookInfo{}).path;
        MDB_txn* transaction = nullptr;
        auto result =
            mdb_txn_begin(environment, nullptr, MDB_RDONLY, &transaction);
        if (result != MDB_SUCCESS) {
            return Result<std::optional<PageHierarchyNode>>::failure(
                errorFromLmdb(path, result, "begin Page hierarchy lookup"));
        }
        auto databases = openJournalDatabases(transaction, path);
        if (!databases) {
            mdb_txn_abort(transaction);
            return Result<std::optional<PageHierarchyNode>>::failure(
                databases.error());
        }
        auto page = namedPageByName(transaction, databases.value(), name);
        auto hasChildren =
            hierarchyNameHasChildren(transaction, databases.value(), name);
        if (!page || !hasChildren) {
            mdb_txn_abort(transaction);
            return Result<std::optional<PageHierarchyNode>>::failure(
                page ? hasChildren.error() : page.error());
        }
        if (!page.value() && !hasChildren.value()) {
            mdb_txn_abort(transaction);
            return Result<std::optional<PageHierarchyNode>>::success(
                std::nullopt);
        }
        const auto separator = name.rfind('/');
        auto localSegment =
            separator == std::string::npos ? name : name.substr(separator + 1);
        PageHierarchyNode node{std::move(name), std::move(localSegment),
                               std::move(page).value(), hasChildren.value()};
        mdb_txn_abort(transaction);
        return Result<std::optional<PageHierarchyNode>>::success(
            std::move(node));
    }

    auto
    readHierarchyChildren(const std::optional<std::string>& parentName,
                          const std::optional<std::string>& continuationCursor)
        const -> Result<PageHierarchyBatch>
    {
        const auto path = info.value_or(NotebookInfo{}).path;
        const auto revision = info.value_or(NotebookInfo{}).revision;
        const auto parent = parentName.value_or("");
        std::string lastSegment;
        if (continuationCursor) {
            const auto firstBreak = continuationCursor->find('\n');
            const auto secondBreak =
                firstBreak == std::string::npos
                    ? std::string::npos
                    : continuationCursor->find('\n', firstBreak + 1);
            std::uint64_t cursorRevision = 0;
            const auto revisionText =
                std::string_view(*continuationCursor).substr(0, firstBreak);
            const auto parsed = std::from_chars(
                revisionText.data(), revisionText.data() + revisionText.size(),
                cursorRevision);
            const auto cursorParent =
                secondBreak == std::string::npos
                    ? std::string_view{}
                    : std::string_view(*continuationCursor)
                          .substr(firstBreak + 1, secondBreak - firstBreak - 1);
            if (firstBreak == std::string::npos ||
                secondBreak == std::string::npos || parsed.ec != std::errc{} ||
                parsed.ptr != revisionText.data() + revisionText.size() ||
                cursorRevision != revision || cursorParent != parent) {
                return Result<PageHierarchyBatch>::failure(
                    makeError(NotebookErrorCode::staleHierarchyCursor, path,
                              "Page hierarchy cursor is stale; request a fresh "
                              "first batch"));
            }
            lastSegment = continuationCursor->substr(secondBreak + 1);
        }

        MDB_txn* transaction = nullptr;
        auto result =
            mdb_txn_begin(environment, nullptr, MDB_RDONLY, &transaction);
        if (result != MDB_SUCCESS) {
            return Result<PageHierarchyBatch>::failure(errorFromLmdb(
                path, result, "begin Page hierarchy enumeration"));
        }
        const auto fail =
            [&](NotebookError error) -> Result<PageHierarchyBatch> {
            mdb_txn_abort(transaction);
            return Result<PageHierarchyBatch>::failure(std::move(error));
        };
        auto databases = openJournalDatabases(transaction, path);
        if (!databases) {
            return fail(databases.error());
        }
        MDB_cursor* cursor = nullptr;
        result = mdb_cursor_open(transaction, databases.value().pagesByName,
                                 &cursor);
        if (result != MDB_SUCCESS) {
            return fail(
                errorFromLmdb(path, result, "open Page hierarchy enumeration"));
        }
        const auto prefix = parent.empty() ? std::string{} : parent + '/';
        const auto scanStart = prefix + lastSegment;
        MDB_val key{scanStart.size(), const_cast<char*>(scanStart.data())};
        MDB_val value{};
        result = scanStart.empty()
                     ? mdb_cursor_get(cursor, &key, &value, MDB_FIRST)
                     : mdb_cursor_get(cursor, &key, &value, MDB_SET_RANGE);
        PageHierarchyBatch batch;
        std::string previousSegment;
        while (result == MDB_SUCCESS) {
            const auto pageName = std::string_view(
                static_cast<const char*>(key.mv_data), key.mv_size);
            if (!prefix.empty() && !pageName.starts_with(prefix)) {
                break;
            }
            const auto remainder = pageName.substr(prefix.size());
            const auto separator = remainder.find('/');
            const auto segment = std::string(remainder.substr(0, separator));
            if (!segment.empty() && segment != previousSegment &&
                segment > lastSegment) {
                previousSegment = segment;
                const auto name = prefix + segment;
                auto materialized =
                    namedPageByName(transaction, databases.value(), name);
                auto hasChildren = hierarchyNameHasChildren(
                    transaction, databases.value(), name);
                if (!materialized || !hasChildren) {
                    mdb_cursor_close(cursor);
                    return fail(materialized ? hasChildren.error()
                                             : materialized.error());
                }
                batch.nodes.push_back({name, segment,
                                       std::move(materialized).value(),
                                       hasChildren.value()});
                if (batch.nodes.size() == 101) {
                    break;
                }
            }
            result = mdb_cursor_get(cursor, &key, &value, MDB_NEXT);
        }
        mdb_cursor_close(cursor);
        if (result != MDB_SUCCESS && result != MDB_NOTFOUND) {
            return fail(
                errorFromLmdb(path, result, "read Page hierarchy enumeration"));
        }
        if (batch.nodes.size() == 101) {
            batch.nodes.pop_back();
            batch.continuationCursor = std::to_string(revision) + '\n' +
                                       parent + '\n' +
                                       batch.nodes.back().localSegment;
        }
        mdb_txn_abort(transaction);
        return Result<PageHierarchyBatch>::success(std::move(batch));
    }

    auto
    createPage(std::string name, std::string displayTitle) -> Result<Page>
    {
        lastCommandCommitted = false;
        const auto path = info.value_or(NotebookInfo{}).path;
        MDB_txn* transaction = nullptr;
        auto result = mdb_txn_begin(environment, nullptr, 0, &transaction);
        if (result != MDB_SUCCESS) {
            return Result<Page>::failure(
                errorFromLmdb(path, result, "begin Page creation"));
        }
        const auto fail = [&](NotebookError error) -> Result<Page> {
            mdb_txn_abort(transaction);
            return Result<Page>::failure(std::move(error));
        };
        auto databases = openJournalDatabases(transaction, path);
        if (!databases) {
            return fail(databases.error());
        }
        MDB_val nameKey{name.size(), name.data()};
        MDB_val existing{};
        result = mdb_get(transaction, databases.value().pagesByName, &nameKey,
                         &existing);
        if (result == MDB_SUCCESS) {
            return fail(makeError(NotebookErrorCode::pageNameConflict, path,
                                  "Page name is already in use"));
        }
        if (result != MDB_NOTFOUND) {
            return fail(errorFromLmdb(path, result, "check Page name"));
        }
        const auto now = currentTimestamp();
        BlockRecord block{
            BlockType::page, BlockMetadata{generateBlockId(), now, now},
            std::nullopt,    {},
            std::move(name), std::move(displayTitle),
            PageKind::named};
        nameKey = MDB_val{block.pageName.size(), block.pageName.data()};
        if (auto error = writeBlock(transaction, databases.value().blocks,
                                    block, path)) {
            return fail(std::move(*error));
        }
        if (auto error =
                writeTypeIndex(transaction, databases.value().blocksByType,
                               block.type, block.metadata.id, path)) {
            return fail(std::move(*error));
        }
        MDB_val idValue{block.metadata.id.bytes.size(),
                        block.metadata.id.bytes.data()};
        result = mdb_put(transaction, databases.value().pagesByName, &nameKey,
                         &idValue, MDB_NOOVERWRITE);
        if (result != MDB_SUCCESS) {
            return fail(errorFromLmdb(path, result, "index Page name"));
        }
        if (auto error = incrementRevision(transaction,
                                           databases.value().metadata, path)) {
            return fail(std::move(*error));
        }
        result = commitAdapter->commit(transaction);
        if (result != MDB_SUCCESS) {
            return Result<Page>::failure(
                errorFromLmdb(path, result, "commit Page creation"));
        }
        incrementCachedRevision();
        lastCommandCommitted = true;
        Page created{block.metadata, block.pageName, block.displayTitle, {}};
        recordPageHistory(std::nullopt, created);
        return Result<Page>::success(std::move(created));
    }

    auto
    deletePage(BlockId pageId) -> Result<Page>
    {
        lastCommandCommitted = false;
        auto before = readPage(pageId);
        if (!before) {
            return before;
        }
        auto removed = restorePageState(pageId, std::nullopt);
        if (!removed) {
            return Result<Page>::failure(removed.error());
        }
        recordPageHistory(before.value(), std::nullopt);
        return before;
    }

    auto
    renamePage(BlockId pageId, std::string name, std::string displayTitle)
        -> Result<Page>
    {
        lastCommandCommitted = false;
        lastMechanicalTextChanges.clear();
        const auto path = info.value_or(NotebookInfo{}).path;
        MDB_txn* transaction = nullptr;
        auto result = mdb_txn_begin(environment, nullptr, 0, &transaction);
        if (result != MDB_SUCCESS) {
            return Result<Page>::failure(
                errorFromLmdb(path, result, "begin Page rename"));
        }
        const auto fail = [&](NotebookError error) -> Result<Page> {
            mdb_txn_abort(transaction);
            return Result<Page>::failure(std::move(error));
        };
        auto databases = openJournalDatabases(transaction, path);
        if (!databases) {
            return fail(databases.error());
        }
        auto loaded =
            readBlock(transaction, databases.value().blocks, pageId, path);
        if (!loaded || loaded.value().type != BlockType::page ||
            loaded.value().pageKind != PageKind::named) {
            if (!loaded &&
                loaded.error().code != NotebookErrorCode::blockNotFound) {
                return fail(loaded.error());
            }
            return fail(makeError(NotebookErrorCode::pageNotFound, path,
                                  "Block is not a Page"));
        }
        auto block = std::move(loaded).value();
        const auto oldName = block.pageName;
        std::vector<MechanicalTextChange> textChanges;
        if (oldName != name) {
            MDB_cursor* cursor = nullptr;
            result = mdb_cursor_open(
                transaction, databases.value().referencesByTarget, &cursor);
            if (result != MDB_SUCCESS) {
                return fail(
                    errorFromLmdb(path, result, "open Page Link rewrite scan"));
            }
            auto prefix = resolvedPageLinkTargetPrefix(pageId);
            MDB_val key{prefix.size(), prefix.data()};
            MDB_val value{};
            result = mdb_cursor_get(cursor, &key, &value, MDB_SET_RANGE);
            while (result == MDB_SUCCESS &&
                   key.mv_size == prefix.size() + BlockId{}.bytes.size() &&
                   std::memcmp(key.mv_data, prefix.data(), prefix.size()) ==
                       0) {
                BlockId sourceId;
                std::memcpy(sourceId.bytes.data(),
                            static_cast<const std::uint8_t*>(key.mv_data) +
                                prefix.size(),
                            sourceId.bytes.size());
                auto source = readBlock(transaction, databases.value().blocks,
                                        sourceId, path);
                if (!source) {
                    mdb_cursor_close(cursor);
                    return fail(source.error());
                }
                auto rewritten = source.value().authoredText;
                const auto links = authored_text::pageLinks(rewritten);
                for (const auto& occurrence : std::views::reverse(links)) {
                    if (occurrence.pageName == oldName) {
                        rewritten.replace(occurrence.sourceByteOffset + 2,
                                          occurrence.sourceByteLength - 4,
                                          name);
                    }
                }
                if (rewritten != source.value().authoredText) {
                    if (!validAuthoredText(rewritten)) {
                        mdb_cursor_close(cursor);
                        return fail(makeError(
                            NotebookErrorCode::invalidAuthoredText, path,
                            "Page Link rewrite exceeds Authored Text limits"));
                    }
                    textChanges.push_back({sourceId,
                                           source.value().authoredText,
                                           std::move(rewritten)});
                }
                result = mdb_cursor_get(cursor, &key, &value, MDB_NEXT);
            }
            mdb_cursor_close(cursor);
            if (result != MDB_SUCCESS && result != MDB_NOTFOUND) {
                return fail(errorFromLmdb(path, result,
                                          "scan Page Link rewrite sources"));
            }
            for (const auto& change : textChanges) {
                auto source = readBlock(transaction, databases.value().blocks,
                                        change.sourceId, path);
                if (!source) {
                    return fail(source.error());
                }
                auto rewritten = std::move(source).value();
                rewritten.authoredText = change.after;
                if (auto error =
                        writeBlock(transaction, databases.value().blocks,
                                   rewritten, path)) {
                    return fail(std::move(*error));
                }
            }
        }
        if (block.pageName != name) {
            MDB_val newKey{name.size(), name.data()};
            MDB_val existing{};
            result = mdb_get(transaction, databases.value().pagesByName,
                             &newKey, &existing);
            if (result == MDB_SUCCESS) {
                return fail(makeError(NotebookErrorCode::pageNameConflict, path,
                                      "Page name is already in use"));
            }
            if (result != MDB_NOTFOUND) {
                return fail(
                    errorFromLmdb(path, result, "check renamed Page name"));
            }
            MDB_val oldKey{block.pageName.size(), block.pageName.data()};
            result = mdb_del(transaction, databases.value().pagesByName,
                             &oldKey, nullptr);
            if (result != MDB_SUCCESS) {
                return fail(
                    errorFromLmdb(path, result, "remove old Page name"));
            }
            MDB_val idValue{pageId.bytes.size(), pageId.bytes.data()};
            result = mdb_put(transaction, databases.value().pagesByName,
                             &newKey, &idValue, MDB_NOOVERWRITE);
            if (result != MDB_SUCCESS) {
                return fail(errorFromLmdb(path, result, "index renamed Page"));
            }
        }
        block.pageName = std::move(name);
        block.displayTitle = std::move(displayTitle);
        block.metadata.updatedAt = currentTimestamp();
        if (auto error = writeBlock(transaction, databases.value().blocks,
                                    block, path)) {
            return fail(std::move(*error));
        }
        if (auto error = incrementRevision(transaction,
                                           databases.value().metadata, path)) {
            return fail(std::move(*error));
        }
        result = commitAdapter->commit(transaction);
        if (result != MDB_SUCCESS) {
            return Result<Page>::failure(
                errorFromLmdb(path, result, "commit Page rename"));
        }
        incrementCachedRevision();
        lastCommandCommitted = true;
        lastMechanicalTextChanges = std::move(textChanges);
        auto reread = readPage(pageId);
        return reread;
    }

    auto
    insertPageEntry(BlockId pageId, std::optional<BlockId> afterEntry,
                    std::string authoredText) -> Result<Page>
    {
        lastCommandCommitted = false;
        const auto path = info.value_or(NotebookInfo{}).path;
        MDB_txn* transaction = nullptr;
        auto result = mdb_txn_begin(environment, nullptr, 0, &transaction);
        if (result != MDB_SUCCESS) {
            return Result<Page>::failure(
                errorFromLmdb(path, result, "begin Page Entry insertion"));
        }
        const auto fail = [&](NotebookError error) -> Result<Page> {
            mdb_txn_abort(transaction);
            return Result<Page>::failure(std::move(error));
        };
        auto databases = openJournalDatabases(transaction, path);
        if (!databases) {
            return fail(databases.error());
        }
        auto loaded =
            readBlock(transaction, databases.value().blocks, pageId, path);
        if (!loaded || loaded.value().type != BlockType::page ||
            loaded.value().pageKind != PageKind::named) {
            return fail(loaded ? makeError(NotebookErrorCode::pageNotFound,
                                           path, "Block is not a Page")
                               : loaded.error());
        }
        auto loadedOutline = loadOutline(transaction, databases.value(),
                                         std::move(loaded).value());
        if (!loadedOutline) {
            return fail(loadedOutline.error());
        }
        auto outline = std::move(loadedOutline).value();
        const auto before = outline;
        std::optional<BlockId> parent;
        auto insertion = outline.entries.size();
        if (afterEntry) {
            const auto found = std::ranges::find_if(
                outline.entries, [&](const auto& entry) -> auto {
                    return entry.metadata.id == *afterEntry;
                });
            if (found == outline.entries.end()) {
                return fail(makeError(NotebookErrorCode::invalidInsertionPoint,
                                      path,
                                      "insertion point is not on this Page"));
            }
            const auto index = static_cast<std::size_t>(
                std::distance(outline.entries.begin(), found));
            parent = found->parentEntry;
            insertion = subtreeEnd(outline.entries, index);
        }
        const auto now = currentTimestamp();
        BlockRecord block{BlockType::pageEntry,
                          BlockMetadata{generateBlockId(), now, now},
                          std::nullopt,
                          std::move(authoredText),
                          {},
                          {},
                          std::nullopt};
        if (auto error = writeBlock(transaction, databases.value().blocks,
                                    block, path)) {
            return fail(std::move(*error));
        }
        if (auto error =
                writeTypeIndex(transaction, databases.value().blocksByType,
                               block.type, block.metadata.id, path)) {
            return fail(std::move(*error));
        }
        outline.entries.insert(outline.entries.begin() +
                                   static_cast<std::ptrdiff_t>(insertion),
                               {block.metadata, block.authoredText, parent});
        if (auto error = touchContainer(transaction, databases.value(), outline,
                                        parent, now)) {
            return fail(std::move(*error));
        }
        if (auto error = rewriteContainment(transaction, databases.value(),
                                            before, outline)) {
            return fail(std::move(*error));
        }
        if (auto error = incrementRevision(transaction,
                                           databases.value().metadata, path)) {
            return fail(std::move(*error));
        }
        result = commitAdapter->commit(transaction);
        if (result != MDB_SUCCESS) {
            return Result<Page>::failure(
                errorFromLmdb(path, result, "commit Page Entry insertion"));
        }
        incrementCachedRevision();
        lastCommandCommitted = true;
        return Result<Page>::success(publicPage(std::move(outline)));
    }

    auto
    pageAfterOutline(const Result<JournalPage>& outcome) const -> Result<Page>
    {
        if (!outcome) {
            return Result<Page>::failure(outcome.error());
        }
        if (!outcome.value().metadata) {
            return Result<Page>::failure(
                makeError(NotebookErrorCode::invalidNotebook,
                          info.value_or(NotebookInfo{}).path,
                          "Page edit lost its container"));
        }
        return readPage(outcome.value().metadata->id);
    }

    auto
    pageIdForEntry(BlockId entryId) const -> Result<BlockId>
    {
        const auto path = info.value_or(NotebookInfo{}).path;
        MDB_txn* transaction = nullptr;
        auto result =
            mdb_txn_begin(environment, nullptr, MDB_RDONLY, &transaction);
        if (result != MDB_SUCCESS) {
            return Result<BlockId>::failure(
                errorFromLmdb(path, result, "begin Page Entry read"));
        }
        auto databases = openJournalDatabases(transaction, path);
        if (!databases) {
            mdb_txn_abort(transaction);
            return Result<BlockId>::failure(databases.error());
        }
        auto outline =
            loadOutlineForEntry(transaction, databases.value(), entryId);
        mdb_txn_abort(transaction);
        if (!outline || outline.value().page.pageKind != PageKind::named) {
            return Result<BlockId>::failure(
                outline ? makeError(NotebookErrorCode::blockNotFound, path,
                                    "Block is not a Page Entry")
                        : outline.error());
        }
        return Result<BlockId>::success(outline.value().page.metadata.id);
    }

    auto
    pageKindForEntry(BlockId entryId) const -> Result<PageKind>
    {
        const auto path = info.value_or(NotebookInfo{}).path;
        MDB_txn* transaction = nullptr;
        auto result =
            mdb_txn_begin(environment, nullptr, MDB_RDONLY, &transaction);
        if (result != MDB_SUCCESS) {
            return Result<PageKind>::failure(
                errorFromLmdb(path, result, "begin containing Page read"));
        }
        auto databases = openJournalDatabases(transaction, path);
        if (!databases) {
            mdb_txn_abort(transaction);
            return Result<PageKind>::failure(databases.error());
        }
        auto outline =
            loadOutlineForEntry(transaction, databases.value(), entryId);
        mdb_txn_abort(transaction);
        if (!outline) {
            return Result<PageKind>::failure(outline.error());
        }
        if (!outline.value().page.pageKind) {
            return Result<PageKind>::failure(
                makeError(NotebookErrorCode::invalidNotebook, path,
                          "containing Page has no kind"));
        }
        return Result<PageKind>::success(*outline.value().page.pageKind);
    }

    auto
    pageHistory(BlockId pageId) -> PageHistory&
    {
        const auto found = std::ranges::find_if(
            pageHistories, [&](const auto& history) -> auto {
                return history.pageId == pageId;
            });
        return found != pageHistories.end()
                   ? *found
                   : pageHistories.emplace_back(PageHistory{pageId, {}, {}});
    }

    auto
    pageHistory(BlockId pageId) const -> const PageHistory*
    {
        const auto found = std::ranges::find_if(
            pageHistories, [&](const auto& history) -> auto {
                return history.pageId == pageId;
            });
        return found == pageHistories.end() ? nullptr : &*found;
    }

    void
    recordPageHistory(
        std::optional<Page> before, std::optional<Page> after,
        std::vector<MechanicalTextChange> mechanicalTextChanges = {})
    {
        clearRedoHistory();
        if (!before && !after) {
            throw NotebookException("Page history action has no state");
        }
        const auto pageId = before ? before->metadata.id : after->metadata.id;
        auto& history = pageHistory(pageId);
        const auto estimatePageBytes =
            [](const std::optional<Page>& page) -> std::size_t {
            if (!page) {
                return sizeof(std::optional<Page>);
            }
            auto bytes =
                sizeof(Page) + page->name.size() + page->displayTitle.size();
            for (const auto& entry : page->entries) {
                bytes += sizeof(Entry) + entry.authoredText.size();
            }
            return bytes;
        };
        PageHistoryAction action{nextHistorySequence++, std::move(before),
                                 std::move(after),
                                 std::move(mechanicalTextChanges), 0};
        action.estimatedBytes = sizeof(PageHistoryAction) +
                                estimatePageBytes(action.before) +
                                estimatePageBytes(action.after);
        for (const auto& change : action.mechanicalTextChanges) {
            action.estimatedBytes += sizeof(MechanicalTextChange) +
                                     change.before.size() + change.after.size();
        }
        historyBytes += action.estimatedBytes;
        history.undo.push_back(std::move(action));
        enforceHistoryBudget();
    }

    auto
    restorePageState(BlockId pageId, const std::optional<Page>& target,
                     const std::vector<MechanicalTextChange>& textChanges = {},
                     bool restoreAfterText = false)
        -> Result<std::optional<Page>>
    {
        lastCommandCommitted = false;
        const auto path = info.value_or(NotebookInfo{}).path;
        MDB_txn* transaction = nullptr;
        auto result = mdb_txn_begin(environment, nullptr, 0, &transaction);
        if (result != MDB_SUCCESS) {
            return Result<std::optional<Page>>::failure(
                errorFromLmdb(path, result, "begin Page history"));
        }
        const auto fail =
            [&](NotebookError error) -> Result<std::optional<Page>> {
            mdb_txn_abort(transaction);
            return Result<std::optional<Page>>::failure(std::move(error));
        };
        auto databases = openJournalDatabases(transaction, path);
        if (!databases) {
            return fail(databases.error());
        }
        auto currentBlock =
            readBlock(transaction, databases.value().blocks, pageId, path);
        if (currentBlock) {
            if (currentBlock.value().type != BlockType::page ||
                currentBlock.value().pageKind != PageKind::named) {
                return fail(
                    makeError(NotebookErrorCode::invalidNotebook, path,
                              "Page history identity is not a Named Page"));
            }
            auto current = loadOutline(transaction, databases.value(),
                                       currentBlock.value());
            if (!current) {
                return fail(current.error());
            }
            for (const auto& entry : current.value().entries) {
                if (auto error = eraseContainment(transaction,
                                                  databases.value(), entry)) {
                    return fail(std::move(*error));
                }
                auto key = blockKey(entry.metadata.id);
                result = mdb_del(transaction, databases.value().blocks, &key,
                                 nullptr);
                if (result != MDB_SUCCESS) {
                    return fail(errorFromLmdb(path, result,
                                              "remove Page Entry for history"));
                }
                if (auto error = removeTypeIndex(
                        transaction, databases.value().blocksByType,
                        BlockType::entry, entry.metadata.id)) {
                    return fail(std::move(*error));
                }
            }
            MDB_val nameKey{
                currentBlock.value().pageName.size(),
                const_cast<char*>(currentBlock.value().pageName.data())};
            result = mdb_del(transaction, databases.value().pagesByName,
                             &nameKey, nullptr);
            if (result != MDB_SUCCESS) {
                return fail(errorFromLmdb(path, result,
                                          "remove Page name for history"));
            }
            auto key = blockKey(pageId);
            result =
                mdb_del(transaction, databases.value().blocks, &key, nullptr);
            if (result != MDB_SUCCESS) {
                return fail(
                    errorFromLmdb(path, result, "remove Page for history"));
            }
            if (auto error =
                    removeTypeIndex(transaction, databases.value().blocksByType,
                                    BlockType::page, pageId)) {
                return fail(std::move(*error));
            }
        } else if (currentBlock.error().code !=
                   NotebookErrorCode::blockNotFound) {
            return fail(currentBlock.error());
        }
        const auto restoreMechanicalText =
            [&]() -> std::optional<NotebookError> {
            for (const auto& change : textChanges) {
                auto source = readBlock(transaction, databases.value().blocks,
                                        change.sourceId, path);
                if (!source) {
                    return source.error();
                }
                auto block = std::move(source).value();
                block.authoredText =
                    restoreAfterText ? change.after : change.before;
                if (auto error = writeBlock(
                        transaction, databases.value().blocks, block, path)) {
                    return error;
                }
            }
            return std::nullopt;
        };
        if (!target) {
            if (auto error = restoreMechanicalText()) {
                return fail(std::move(*error));
            }
            if (auto error = incrementRevision(
                    transaction, databases.value().metadata, path)) {
                return fail(std::move(*error));
            }
            result = commitAdapter->commit(transaction);
            if (result != MDB_SUCCESS) {
                return Result<std::optional<Page>>::failure(
                    errorFromLmdb(path, result, "commit Page history"));
            }
            incrementCachedRevision();
            lastCommandCommitted = true;
            return Result<std::optional<Page>>::success(std::nullopt);
        }
        BlockRecord restoredBlock{
            BlockType::page, target->metadata,     std::nullopt,   {},
            target->name,    target->displayTitle, PageKind::named};
        if (auto error = writeBlock(transaction, databases.value().blocks,
                                    restoredBlock, path)) {
            return fail(std::move(*error));
        }
        if (auto error =
                writeTypeIndex(transaction, databases.value().blocksByType,
                               BlockType::page, pageId, path)) {
            return fail(std::move(*error));
        }
        MDB_val nameKey{restoredBlock.pageName.size(),
                        restoredBlock.pageName.data()};
        MDB_val pageValue{pageId.bytes.size(), pageId.bytes.data()};
        result = mdb_put(transaction, databases.value().pagesByName, &nameKey,
                         &pageValue, MDB_NOOVERWRITE);
        if (result != MDB_SUCCESS) {
            return fail(result == MDB_KEYEXIST
                            ? makeError(NotebookErrorCode::pageNameConflict,
                                        path, "Page name is already in use")
                            : errorFromLmdb(path, result,
                                            "restore Page name for history"));
        }
        LoadedOutline restored{restoredBlock, {}};
        for (const auto& entry : target->entries) {
            BlockRecord block{BlockType::entry,
                              entry.metadata,
                              std::nullopt,
                              entry.authoredText,
                              {},
                              {},
                              std::nullopt};
            if (auto error = writeBlock(transaction, databases.value().blocks,
                                        block, path)) {
                return fail(std::move(*error));
            }
            if (auto error = writeTypeIndex(
                    transaction, databases.value().blocksByType,
                    BlockType::pageEntry, entry.metadata.id, path)) {
                return fail(std::move(*error));
            }
            restored.entries.push_back(
                {entry.metadata, entry.authoredText, entry.parentEntry});
        }
        const LoadedOutline empty{restoredBlock, {}};
        if (auto error = rewriteContainment(transaction, databases.value(),
                                            empty, restored)) {
            return fail(std::move(*error));
        }
        if (auto error = restoreMechanicalText()) {
            return fail(std::move(*error));
        }
        if (auto error = incrementRevision(transaction,
                                           databases.value().metadata, path)) {
            return fail(std::move(*error));
        }
        result = commitAdapter->commit(transaction);
        if (result != MDB_SUCCESS) {
            return Result<std::optional<Page>>::failure(
                errorFromLmdb(path, result, "commit Page history"));
        }
        incrementCachedRevision();
        lastCommandCommitted = true;
        return Result<std::optional<Page>>::success(
            publicPage(std::move(restored)));
    }

    auto
    applyPageHistory(BlockId pageId, bool redo) -> Result<std::optional<Page>>
    {
        auto& history = pageHistory(pageId);
        auto& source = redo ? history.redo : history.undo;
        if (source.empty()) {
            return Result<std::optional<Page>>::failure(
                makeError(redo ? NotebookErrorCode::redoUnavailable
                               : NotebookErrorCode::undoUnavailable,
                          info.value_or(NotebookInfo{}).path,
                          "no Page edit is available"));
        }
        const auto target = redo ? source.back().after : source.back().before;
        auto result = restorePageState(
            pageId, target, source.back().mechanicalTextChanges, redo);
        if (!result) {
            return result;
        }
        auto action = std::move(source.back());
        source.pop_back();
        (redo ? history.undo : history.redo).push_back(std::move(action));
        return result;
    }

    auto
    readNestedJournalPage(JournalDate date) const -> Result<JournalPage>
    {
        const auto path = info.value_or(NotebookInfo{}).path;
        MDB_txn* transaction = nullptr;
        auto result =
            mdb_txn_begin(environment, nullptr, MDB_RDONLY, &transaction);
        if (result != MDB_SUCCESS) {
            return Result<JournalPage>::failure(
                errorFromLmdb(path, result, "begin Journal read"));
        }
        auto databases = openJournalDatabases(transaction, path);
        if (!databases) {
            mdb_txn_abort(transaction);
            return Result<JournalPage>::failure(databases.error());
        }
        const auto encodedDate = dateKey(date);
        MDB_val key{encodedDate.size(),
                    const_cast<std::uint8_t*>(encodedDate.data())};
        MDB_val value{};
        result =
            mdb_get(transaction, databases.value().journalByDate, &key, &value);
        if (result == MDB_NOTFOUND) {
            mdb_txn_abort(transaction);
            return Result<JournalPage>::success({date, std::nullopt, {}});
        }
        if (result != MDB_SUCCESS || value.mv_size != BlockId{}.bytes.size()) {
            mdb_txn_abort(transaction);
            return Result<JournalPage>::failure(
                result == MDB_SUCCESS
                    ? makeError(NotebookErrorCode::invalidNotebook, path,
                                "invalid Journal date index")
                    : errorFromLmdb(path, result, "read Journal date index"));
        }
        BlockId pageId;
        std::memcpy(pageId.bytes.data(), value.mv_data, pageId.bytes.size());
        auto page =
            readBlock(transaction, databases.value().blocks, pageId, path);
        if (!page || page.value().type != BlockType::page ||
            page.value().pageKind != PageKind::journal ||
            page.value().journalDate != date) {
            mdb_txn_abort(transaction);
            return Result<JournalPage>::failure(
                page ? makeError(NotebookErrorCode::invalidNotebook, path,
                                 "Journal date points to an invalid Page")
                     : page.error());
        }
        auto outline = loadOutline(transaction, databases.value(),
                                   std::move(page).value());
        mdb_txn_abort(transaction);
        if (!outline) {
            return Result<JournalPage>::failure(outline.error());
        }
        auto loaded = std::move(outline).value();
        return Result<JournalPage>::success(
            {date, loaded.page.metadata, publicJournalEntries(loaded.entries)});
    }

    auto
    readJournalPage(JournalDate date) const -> Result<JournalPage>
    {
        const auto path = info.value_or(NotebookInfo{}).path;
        MDB_txn* transaction = nullptr;
        auto result =
            mdb_txn_begin(environment, nullptr, MDB_RDONLY, &transaction);
        if (result != MDB_SUCCESS) {
            return Result<JournalPage>::failure(
                errorFromLmdb(path, result, "begin Journal read"));
        }
        auto databases = openJournalDatabases(transaction, path);
        if (!databases) {
            mdb_txn_abort(transaction);
            return Result<JournalPage>::failure(databases.error());
        }

        const auto encodedDate = dateKey(date);
        MDB_val dateKeyValue{encodedDate.size(),
                             const_cast<std::uint8_t*>(encodedDate.data())};
        MDB_val pageValue{};
        result = mdb_get(transaction, databases.value().journalByDate,
                         &dateKeyValue, &pageValue);
        if (result == MDB_NOTFOUND) {
            mdb_txn_abort(transaction);
            return Result<JournalPage>::success(
                JournalPage{date, std::nullopt, {}});
        }
        if (result != MDB_SUCCESS ||
            pageValue.mv_size != BlockId{}.bytes.size()) {
            mdb_txn_abort(transaction);
            return Result<JournalPage>::failure(
                result == MDB_SUCCESS
                    ? makeError(NotebookErrorCode::invalidNotebook, path,
                                "invalid Journal date index")
                    : errorFromLmdb(path, result, "read Journal date index"));
        }
        BlockId pageId;
        std::memcpy(pageId.bytes.data(), pageValue.mv_data,
                    pageId.bytes.size());
        auto pageBlock =
            readBlock(transaction, databases.value().blocks, pageId, path);
        if (!pageBlock || pageBlock.value().type != BlockType::page ||
            pageBlock.value().pageKind != PageKind::journal ||
            pageBlock.value().journalDate != date) {
            mdb_txn_abort(transaction);
            return Result<JournalPage>::failure(
                pageBlock ? makeError(NotebookErrorCode::invalidNotebook, path,
                                      "Journal date points to an invalid Page")
                          : pageBlock.error());
        }

        JournalPage page{date, pageBlock.value().metadata, {}};
        MDB_cursor* cursor = nullptr;
        result = mdb_cursor_open(
            transaction, databases.value().containmentByParent, &cursor);
        if (result != MDB_SUCCESS) {
            mdb_txn_abort(transaction);
            return Result<JournalPage>::failure(
                errorFromLmdb(path, result, "open Journal containment"));
        }
        auto start = containmentParentKey(pageId, 0);
        MDB_val containmentKey{start.size(), start.data()};
        MDB_val childValue{};
        result =
            mdb_cursor_get(cursor, &containmentKey, &childValue, MDB_SET_RANGE);
        while (result == MDB_SUCCESS) {
            if (containmentKey.mv_size != 24 ||
                std::memcmp(containmentKey.mv_data, pageId.bytes.data(),
                            pageId.bytes.size()) != 0) {
                break;
            }
            if (childValue.mv_size != BlockId{}.bytes.size()) {
                mdb_cursor_close(cursor);
                mdb_txn_abort(transaction);
                return Result<JournalPage>::failure(
                    makeError(NotebookErrorCode::invalidNotebook, path,
                              "invalid Journal containment"));
            }
            BlockId entryId;
            std::memcpy(entryId.bytes.data(), childValue.mv_data,
                        entryId.bytes.size());
            auto entryBlock =
                readBlock(transaction, databases.value().blocks, entryId, path);
            if (!entryBlock ||
                (entryBlock.value().type != BlockType::journalEntry &&
                 entryBlock.value().type != BlockType::pageEntry)) {
                mdb_cursor_close(cursor);
                mdb_txn_abort(transaction);
                return Result<JournalPage>::failure(
                    entryBlock
                        ? makeError(NotebookErrorCode::invalidNotebook, path,
                                    "Journal Page contains a non-Entry Block")
                        : entryBlock.error());
            }
            page.entries.push_back(Entry{entryBlock.value().metadata,
                                         entryBlock.value().authoredText,
                                         std::nullopt});
            result =
                mdb_cursor_get(cursor, &containmentKey, &childValue, MDB_NEXT);
        }
        mdb_cursor_close(cursor);
        mdb_txn_abort(transaction);
        if (result != MDB_SUCCESS && result != MDB_NOTFOUND) {
            return Result<JournalPage>::failure(
                errorFromLmdb(path, result, "read Journal containment"));
        }
        return Result<JournalPage>::success(std::move(page));
    }

    auto
    insertEntry(JournalDate date, std::optional<BlockId> afterEntry,
                std::string authoredText) -> Result<JournalPage>
    {
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
        MDB_val dateIndexKey{encodedDate.size(),
                             const_cast<std::uint8_t*>(encodedDate.data())};
        MDB_val pageValue{};
        result = mdb_get(transaction, databases.value().journalByDate,
                         &dateIndexKey, &pageValue);
        BlockRecord pageBlock;
        if (result == MDB_NOTFOUND) {
            if (afterEntry) {
                return fail(
                    makeError(NotebookErrorCode::invalidInsertionPoint, path,
                              "insertion point is not on this Journal Page"));
            }
            pageBlock = BlockRecord{BlockType::journalPage,
                                    BlockMetadata{generateBlockId(), now, now},
                                    date,
                                    {},
                                    {},
                                    {},
                                    PageKind::journal};
            if (auto error = writeBlock(transaction, databases.value().blocks,
                                        pageBlock, path)) {
                return fail(std::move(*error));
            }
            if (auto error = writeTypeIndex(
                    transaction, databases.value().blocksByType, pageBlock.type,
                    pageBlock.metadata.id, path)) {
                return fail(std::move(*error));
            }
            MDB_val pageIdValue{pageBlock.metadata.id.bytes.size(),
                                pageBlock.metadata.id.bytes.data()};
            result = mdb_put(transaction, databases.value().journalByDate,
                             &dateIndexKey, &pageIdValue, MDB_NOOVERWRITE);
            if (result != MDB_SUCCESS) {
                return fail(errorFromLmdb(path, result, "index Journal date"));
            }
        } else {
            if (result != MDB_SUCCESS ||
                pageValue.mv_size != BlockId{}.bytes.size()) {
                return fail(result == MDB_SUCCESS
                                ? makeError(NotebookErrorCode::invalidNotebook,
                                            path, "invalid Journal date index")
                                : errorFromLmdb(path, result,
                                                "read Journal date index"));
            }
            BlockId pageId;
            std::memcpy(pageId.bytes.data(), pageValue.mv_data,
                        pageId.bytes.size());
            auto loaded =
                readBlock(transaction, databases.value().blocks, pageId, path);
            if (!loaded || loaded.value().type != BlockType::page ||
                loaded.value().pageKind != PageKind::journal) {
                return fail(
                    loaded ? makeError(NotebookErrorCode::invalidNotebook, path,
                                       "Journal date points to an invalid Page")
                           : loaded.error());
            }
            pageBlock = loaded.value();
            pageBlock.metadata.updatedAt = now;
            if (auto error = writeBlock(transaction, databases.value().blocks,
                                        pageBlock, path)) {
                return fail(std::move(*error));
            }
        }

        std::vector<std::pair<BlockId, std::uint64_t>> siblings;
        MDB_cursor* cursor = nullptr;
        result = mdb_cursor_open(
            transaction, databases.value().containmentByParent, &cursor);
        if (result != MDB_SUCCESS) {
            return fail(
                errorFromLmdb(path, result, "open Journal containment"));
        }
        auto start = containmentParentKey(pageBlock.metadata.id, 0);
        MDB_val parentKey{start.size(), start.data()};
        MDB_val childValue{};
        result = mdb_cursor_get(cursor, &parentKey, &childValue, MDB_SET_RANGE);
        while (result == MDB_SUCCESS && parentKey.mv_size == 24 &&
               std::memcmp(parentKey.mv_data,
                           pageBlock.metadata.id.bytes.data(),
                           pageBlock.metadata.id.bytes.size()) == 0) {
            if (childValue.mv_size != BlockId{}.bytes.size()) {
                mdb_cursor_close(cursor);
                return fail(makeError(NotebookErrorCode::invalidNotebook, path,
                                      "invalid Journal containment"));
            }
            BlockId child;
            std::memcpy(child.bytes.data(), childValue.mv_data,
                        child.bytes.size());
            siblings.emplace_back(child, rankFromParentKey(parentKey));
            result = mdb_cursor_get(cursor, &parentKey, &childValue, MDB_NEXT);
        }
        mdb_cursor_close(cursor);
        if (result != MDB_SUCCESS && result != MDB_NOTFOUND) {
            return fail(
                errorFromLmdb(path, result, "read Journal containment"));
        }

        std::size_t insertionIndex = siblings.size();
        if (afterEntry) {
            const auto found =
                std::ranges::find_if(siblings, [&](const auto& item) -> bool {
                    return item.first == *afterEntry;
                });
            if (found == siblings.end()) {
                return fail(
                    makeError(NotebookErrorCode::invalidInsertionPoint, path,
                              "insertion point is not on this Journal Page"));
            }
            insertionIndex = static_cast<std::size_t>(
                                 std::distance(siblings.begin(), found)) +
                             1;
        }
        constexpr std::uint64_t rankGap = 1ULL << 32U;
        std::uint64_t rank = rankGap;
        if (!siblings.empty()) {
            if (insertionIndex == siblings.size()) {
                rank = siblings.back().second + rankGap;
            } else {
                const auto lower = insertionIndex == 0
                                       ? 0
                                       : siblings[insertionIndex - 1].second;
                const auto upper = siblings[insertionIndex].second;
                rank = lower + ((upper - lower) / 2U);
            }
        }
        const auto needsRebalancing =
            (!siblings.empty() && insertionIndex == siblings.size() &&
             rank < siblings.back().second) ||
            (insertionIndex < siblings.size() &&
             rank == (insertionIndex == 0
                          ? 0
                          : siblings[insertionIndex - 1].second));
        if (needsRebalancing) {
            if (siblings.size() >=
                std::numeric_limits<std::uint64_t>::max() / rankGap) {
                return fail(
                    makeError(NotebookErrorCode::ioFailure, path,
                              "Journal ordering capacity is exhausted"));
            }
            for (const auto& [siblingId, oldRank] : siblings) {
                static_cast<void>(siblingId);
                auto oldKeyBytes =
                    containmentParentKey(pageBlock.metadata.id, oldRank);
                MDB_val oldKey{oldKeyBytes.size(), oldKeyBytes.data()};
                result =
                    mdb_del(transaction, databases.value().containmentByParent,
                            &oldKey, nullptr);
                if (result != MDB_SUCCESS) {
                    return fail(errorFromLmdb(path, result,
                                              "rebalance Journal ordering"));
                }
            }
            for (std::size_t index = 0; index < siblings.size(); ++index) {
                const auto rebalancedRank =
                    (static_cast<std::uint64_t>(index) + 1U) * rankGap;
                siblings[index].second = rebalancedRank;
                auto parentBytes =
                    containmentParentKey(pageBlock.metadata.id, rebalancedRank);
                MDB_val rebalancedParentKey{parentBytes.size(),
                                            parentBytes.data()};
                MDB_val siblingValue{siblings[index].first.bytes.size(),
                                     siblings[index].first.bytes.data()};
                result = mdb_put(
                    transaction, databases.value().containmentByParent,
                    &rebalancedParentKey, &siblingValue, MDB_NOOVERWRITE);
                if (result != MDB_SUCCESS) {
                    return fail(errorFromLmdb(path, result,
                                              "rebalance Journal ordering"));
                }
                auto siblingKey = blockKey(siblings[index].first);
                auto childIndex =
                    containmentParentKey(pageBlock.metadata.id, rebalancedRank);
                MDB_val childIndexValue{childIndex.size(), childIndex.data()};
                result =
                    mdb_put(transaction, databases.value().containmentByChild,
                            &siblingKey, &childIndexValue, 0);
                if (result != MDB_SUCCESS) {
                    return fail(errorFromLmdb(
                        path, result, "rebalance Journal parent index"));
                }
            }
            if (insertionIndex == siblings.size()) {
                rank = siblings.empty() ? rankGap
                                        : siblings.back().second + rankGap;
            } else {
                const auto lower = insertionIndex == 0
                                       ? 0
                                       : siblings[insertionIndex - 1].second;
                rank = lower + ((siblings[insertionIndex].second - lower) / 2U);
            }
        }

        BlockRecord entryBlock{BlockType::journalEntry,
                               BlockMetadata{generateBlockId(), now, now},
                               std::nullopt,
                               std::move(authoredText),
                               {},
                               {},
                               std::nullopt};
        if (auto error = writeBlock(transaction, databases.value().blocks,
                                    entryBlock, path)) {
            return fail(std::move(*error));
        }
        if (auto error =
                writeTypeIndex(transaction, databases.value().blocksByType,
                               entryBlock.type, entryBlock.metadata.id, path)) {
            return fail(std::move(*error));
        }
        auto encodedParent = containmentParentKey(pageBlock.metadata.id, rank);
        MDB_val newParentKey{encodedParent.size(), encodedParent.data()};
        MDB_val entryIdValue{entryBlock.metadata.id.bytes.size(),
                             entryBlock.metadata.id.bytes.data()};
        result = mdb_put(transaction, databases.value().containmentByParent,
                         &newParentKey, &entryIdValue, MDB_NOOVERWRITE);
        if (result != MDB_SUCCESS) {
            return fail(errorFromLmdb(path, result, "write Journal ordering"));
        }
        auto childKey = blockKey(entryBlock.metadata.id);
        std::array<std::uint8_t, 24> childIndex{};
        std::memcpy(childIndex.data(), pageBlock.metadata.id.bytes.data(),
                    pageBlock.metadata.id.bytes.size());
        for (std::size_t index = 0; index < 8; ++index) {
            childIndex[16 + index] =
                static_cast<std::uint8_t>(rank >> ((7U - index) * 8U));
        }
        MDB_val childIndexValue{childIndex.size(), childIndex.data()};
        result = mdb_put(transaction, databases.value().containmentByChild,
                         &childKey, &childIndexValue, MDB_NOOVERWRITE);
        if (result != MDB_SUCCESS) {
            return fail(
                errorFromLmdb(path, result, "write Journal parent index"));
        }

        JournalPage committedPage{date, pageBlock.metadata, {}};
        committedPage.entries.reserve(siblings.size() + 1);
        for (std::size_t index = 0; index <= siblings.size(); ++index) {
            if (index == insertionIndex) {
                committedPage.entries.push_back(Entry{entryBlock.metadata,
                                                      entryBlock.authoredText,
                                                      std::nullopt});
            }
            if (index < siblings.size()) {
                auto sibling = readBlock(transaction, databases.value().blocks,
                                         siblings[index].first, path);
                if (!sibling ||
                    sibling.value().type != BlockType::journalEntry) {
                    return fail(
                        sibling ? makeError(
                                      NotebookErrorCode::invalidNotebook, path,
                                      "Journal Page contains a non-Entry Block")
                                : sibling.error());
                }
                committedPage.entries.push_back(
                    Entry{sibling.value().metadata,
                          sibling.value().authoredText, std::nullopt});
            }
        }
        if (auto error = incrementRevision(transaction,
                                           databases.value().metadata, path)) {
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

    auto
    updateEntry(BlockId entryId, std::string authoredText)
        -> Result<OutlineEntryRecord>
    {
        lastCommandCommitted = false;
        const auto path = info.value_or(NotebookInfo{}).path;
        MDB_txn* transaction = nullptr;
        auto result = mdb_txn_begin(environment, nullptr, 0, &transaction);
        if (result != MDB_SUCCESS) {
            return Result<OutlineEntryRecord>::failure(
                errorFromLmdb(path, result, "begin outline edit"));
        }
        const auto fail =
            [&](NotebookError error) -> Result<OutlineEntryRecord> {
            mdb_txn_abort(transaction);
            return Result<OutlineEntryRecord>::failure(std::move(error));
        };
        auto databases = openJournalDatabases(transaction, path);
        if (!databases) {
            return fail(databases.error());
        }
        auto outlineResult =
            loadOutlineForEntry(transaction, databases.value(), entryId);
        if (!outlineResult) {
            return fail(outlineResult.error());
        }
        const auto before = std::move(outlineResult).value();
        auto loaded =
            readBlock(transaction, databases.value().blocks, entryId, path);
        if (!loaded) {
            return fail(loaded.error());
        }
        if (loaded.value().type != BlockType::journalEntry &&
            loaded.value().type != BlockType::pageEntry) {
            return fail(makeError(NotebookErrorCode::blockNotFound, path,
                                  "Block is not a Journal Entry"));
        }
        auto entry = loaded.value();
        auto parentLink = parentOf(transaction, databases.value(), entryId);
        if (!parentLink) {
            return fail(parentLink.error());
        }
        auto parentBlock = readBlock(transaction, databases.value().blocks,
                                     parentLink.value().parent, path);
        if (!parentBlock) {
            return fail(parentBlock.error());
        }
        const auto parentEntry =
            (parentBlock.value().type == BlockType::journalEntry ||
             parentBlock.value().type == BlockType::pageEntry)
                ? std::optional<BlockId>{parentLink.value().parent}
                : std::nullopt;
        if (entry.authoredText == authoredText) {
            mdb_txn_abort(transaction);
            return Result<OutlineEntryRecord>::success(
                {entry.metadata, entry.authoredText, parentEntry});
        }
        entry.authoredText = std::move(authoredText);
        entry.metadata.updatedAt = currentTimestamp();
        if (auto error = writeBlock(transaction, databases.value().blocks,
                                    entry, path)) {
            return fail(std::move(*error));
        }
        if (auto error = incrementRevision(transaction,
                                           databases.value().metadata, path)) {
            return fail(std::move(*error));
        }
        result = commitAdapter->commit(transaction);
        if (result != MDB_SUCCESS) {
            return Result<OutlineEntryRecord>::failure(
                errorFromLmdb(path, result, "commit outline edit"));
        }
        incrementCachedRevision();
        lastCommandCommitted = true;
        auto after = before;
        const auto changed = std::ranges::find_if(
            after.entries, [&](const auto& candidate) -> auto {
                return candidate.metadata.id == entryId;
            });
        changed->metadata = entry.metadata;
        changed->authoredText = entry.authoredText;
        if (before.page.pageKind == PageKind::journal) {
            recordHistory(before.page.journalDate.value_or(JournalDate{}),
                          before, std::move(after));
        }
        return Result<OutlineEntryRecord>::success(
            {entry.metadata, entry.authoredText, parentEntry});
    }

    auto
    insertNestedEntry(JournalDate date, std::optional<BlockId> afterEntry,
                      std::string authoredText) -> Result<JournalPage>
    {
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
        MDB_val dateIndexKey{encodedDate.size(),
                             const_cast<std::uint8_t*>(encodedDate.data())};
        MDB_val pageValue{};
        result = mdb_get(transaction, databases.value().journalByDate,
                         &dateIndexKey, &pageValue);
        const auto pageWasCreated = result == MDB_NOTFOUND;
        BlockRecord page;
        if (result == MDB_NOTFOUND) {
            if (afterEntry) {
                return fail(
                    makeError(NotebookErrorCode::invalidInsertionPoint, path,
                              "insertion point is not on this Journal Page"));
            }
            page = {BlockType::journalPage,
                    BlockMetadata{generateBlockId(), now, now},
                    date,
                    {},
                    {},
                    {},
                    PageKind::journal};
            if (auto error = writeBlock(transaction, databases.value().blocks,
                                        page, path)) {
                return fail(std::move(*error));
            }
            if (auto error =
                    writeTypeIndex(transaction, databases.value().blocksByType,
                                   page.type, page.metadata.id, path)) {
                return fail(std::move(*error));
            }
            MDB_val pageIdValue{page.metadata.id.bytes.size(),
                                page.metadata.id.bytes.data()};
            result = mdb_put(transaction, databases.value().journalByDate,
                             &dateIndexKey, &pageIdValue, MDB_NOOVERWRITE);
            if (result != MDB_SUCCESS) {
                return fail(errorFromLmdb(path, result, "index Journal date"));
            }
        } else {
            if (result != MDB_SUCCESS ||
                pageValue.mv_size != BlockId{}.bytes.size()) {
                return fail(result == MDB_SUCCESS
                                ? makeError(NotebookErrorCode::invalidNotebook,
                                            path, "invalid Journal date index")
                                : errorFromLmdb(path, result,
                                                "read Journal date index"));
            }
            BlockId pageId;
            std::memcpy(pageId.bytes.data(), pageValue.mv_data,
                        pageId.bytes.size());
            auto loaded =
                readBlock(transaction, databases.value().blocks, pageId, path);
            if (!loaded || loaded.value().type != BlockType::page ||
                loaded.value().pageKind != PageKind::journal ||
                loaded.value().journalDate != date) {
                return fail(
                    loaded ? makeError(NotebookErrorCode::invalidNotebook, path,
                                       "Journal date points to an invalid Page")
                           : loaded.error());
            }
            page = std::move(loaded).value();
        }
        auto loadedOutline =
            loadOutline(transaction, databases.value(), std::move(page));
        if (!loadedOutline) {
            return fail(loadedOutline.error());
        }
        auto outline = std::move(loadedOutline).value();
        const auto before = outline;
        std::optional<BlockId> parent;
        auto insertionIndex = outline.entries.size();
        if (afterEntry) {
            const auto found = std::ranges::find_if(
                outline.entries, [&](const auto& entry) -> bool {
                    return entry.metadata.id == *afterEntry;
                });
            if (found == outline.entries.end()) {
                return fail(
                    makeError(NotebookErrorCode::invalidInsertionPoint, path,
                              "insertion point is not on this Journal Page"));
            }
            const auto index = static_cast<std::size_t>(
                std::distance(outline.entries.begin(), found));
            parent = found->parentEntry;
            insertionIndex = subtreeEnd(outline.entries, index);
        }
        BlockRecord entry{BlockType::journalEntry,
                          BlockMetadata{generateBlockId(), now, now},
                          std::nullopt,
                          std::move(authoredText),
                          {},
                          {},
                          std::nullopt};
        if (auto error = writeBlock(transaction, databases.value().blocks,
                                    entry, path)) {
            return fail(std::move(*error));
        }
        if (auto error =
                writeTypeIndex(transaction, databases.value().blocksByType,
                               entry.type, entry.metadata.id, path)) {
            return fail(std::move(*error));
        }
        outline.entries.insert(outline.entries.begin() +
                                   static_cast<std::ptrdiff_t>(insertionIndex),
                               {entry.metadata, entry.authoredText, parent});
        if (auto error = touchContainer(transaction, databases.value(), outline,
                                        parent, now)) {
            return fail(std::move(*error));
        }
        if (auto error = rewriteContainment(transaction, databases.value(),
                                            before, outline)) {
            return fail(std::move(*error));
        }
        if (auto error = incrementRevision(transaction,
                                           databases.value().metadata, path)) {
            return fail(std::move(*error));
        }
        result = commitAdapter->commit(transaction);
        if (result != MDB_SUCCESS) {
            return Result<JournalPage>::failure(
                errorFromLmdb(path, result, "commit Journal insertion"));
        }
        incrementCachedRevision();
        lastCommandCommitted = true;
        recordHistory(date,
                      pageWasCreated ? std::nullopt
                                     : std::optional<LoadedOutline>{before},
                      outline);
        return Result<JournalPage>::success(
            {date, outline.page.metadata,
             publicJournalEntries(outline.entries)});
    }

    auto
    editOutline(BlockId entryId, OutlineEditKind edit,
                std::size_t cursorByteOffset = 0, std::string editedText = {})
        -> Result<JournalPage>
    {
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
        auto loaded =
            loadOutlineForEntry(transaction, databases.value(), entryId);
        if (!loaded) {
            return fail(loaded.error());
        }
        auto outline = std::move(loaded).value();
        const auto before = outline;
        auto found = std::ranges::find_if(
            outline.entries, [&](const auto& entry) -> bool {
                return entry.metadata.id == entryId;
            });
        if (found == outline.entries.end()) {
            return fail(makeError(NotebookErrorCode::blockNotFound, path,
                                  "Journal Entry is not on its Page"));
        }
        auto index = static_cast<std::size_t>(
            std::distance(outline.entries.begin(), found));
        const auto originalParent = found->parentEntry;
        const auto hasChildren = std::ranges::any_of(
            outline.entries, [&](const auto& entry) -> bool {
                return entry.parentEntry == entryId;
            });
        const auto now = currentTimestamp();
        std::vector<std::optional<BlockId>> touchedContainers;
        auto touch = [&](std::optional<BlockId> parent) -> void {
            if (std::ranges::find(touchedContainers, parent) ==
                touchedContainers.end()) {
                touchedContainers.push_back(parent);
            }
        };
        bool deleteOriginal = false;
        std::optional<BlockRecord> blockToWrite;
        std::optional<BlockRecord> blockToCreate;

        if (edit == OutlineEditKind::split) {
            if (cursorByteOffset > editedText.size() ||
                (cursorByteOffset < editedText.size() &&
                 (static_cast<unsigned char>(editedText[cursorByteOffset]) &
                  0xC0U) == 0x80U)) {
                return fail(
                    makeError(NotebookErrorCode::invalidCursorPosition, path,
                              "split cursor is not on a Unicode boundary"));
            }
            auto original =
                readBlock(transaction, databases.value().blocks, entryId, path);
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
            BlockRecord created{BlockType::entry,
                                BlockMetadata{generateBlockId(), now, now},
                                std::nullopt,
                                suffix,
                                {},
                                {},
                                std::nullopt};
            blockToCreate = created;
            const auto insertion = subtreeEnd(outline.entries, index);
            outline.entries.insert(
                outline.entries.begin() +
                    static_cast<std::ptrdiff_t>(insertion),
                {created.metadata, created.authoredText, originalParent});
            touch(originalParent);
        } else if (edit == OutlineEditKind::join) {
            if (hasChildren) {
                return fail(
                    makeError(NotebookErrorCode::blockHasChildren, path,
                              "an Entry with children cannot be joined"));
            }
            if (index == 0) {
                return fail(
                    makeError(NotebookErrorCode::invalidStructuralMove, path,
                              "the first Entry has no previous visible Entry"));
            }
            auto& target = outline.entries[index - 1];
            const auto combined = target.authoredText + editedText;
            if (!validAuthoredText(combined)) {
                return fail(
                    makeError(NotebookErrorCode::invalidAuthoredText, path,
                              "joined Journal Entry text is too large"));
            }
            auto targetBlock = readBlock(transaction, databases.value().blocks,
                                         target.metadata.id, path);
            if (!targetBlock) {
                return fail(targetBlock.error());
            }
            auto updated = std::move(targetBlock).value();
            updated.authoredText = combined;
            updated.metadata.updatedAt = now;
            target.authoredText = combined;
            target.metadata.updatedAt = now;
            blockToWrite = updated;
            outline.entries.erase(outline.entries.begin() +
                                  static_cast<std::ptrdiff_t>(index));
            deleteOriginal = true;
            touch(originalParent);
        } else if (edit == OutlineEditKind::erase) {
            if (hasChildren) {
                return fail(
                    makeError(NotebookErrorCode::blockHasChildren, path,
                              "an Entry with children cannot be deleted"));
            }
            outline.entries.erase(outline.entries.begin() +
                                  static_cast<std::ptrdiff_t>(index));
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
                return fail(makeError(NotebookErrorCode::invalidStructuralMove,
                                      path,
                                      "the first sibling cannot be indented"));
            }
            found->parentEntry = outline.entries[*previousSibling].metadata.id;
            touch(originalParent);
            touch(found->parentEntry);
        } else if (edit == OutlineEditKind::outdent) {
            if (!originalParent) {
                return fail(makeError(NotebookErrorCode::invalidStructuralMove,
                                      path,
                                      "a top-level Entry cannot be outdented"));
            }
            const auto parent = std::ranges::find_if(
                outline.entries, [&](const auto& entry) -> bool {
                    return entry.metadata.id == *originalParent;
                });
            if (parent == outline.entries.end()) {
                return fail(makeError(NotebookErrorCode::invalidNotebook, path,
                                      "Journal parent is outside its Page"));
            }
            const auto parentParent = parent->parentEntry;
            const auto end = subtreeEnd(outline.entries, index);
            std::vector<OutlineEntryRecord> moving(
                outline.entries.begin() + static_cast<std::ptrdiff_t>(index),
                outline.entries.begin() + static_cast<std::ptrdiff_t>(end));
            moving.front().parentEntry = parentParent;
            outline.entries.erase(
                outline.entries.begin() + static_cast<std::ptrdiff_t>(index),
                outline.entries.begin() + static_cast<std::ptrdiff_t>(end));
            const auto parentAfterErase =
                static_cast<std::size_t>(std::distance(
                    outline.entries.begin(),
                    std::ranges::find_if(
                        outline.entries, [&](const auto& entry) -> bool {
                            return entry.metadata.id == *originalParent;
                        })));
            const auto insertion =
                subtreeEnd(outline.entries, parentAfterErase);
            outline.entries.insert(outline.entries.begin() +
                                       static_cast<std::ptrdiff_t>(insertion),
                                   moving.begin(), moving.end());
            touch(originalParent);
            touch(parentParent);
        } else {
            const auto end = subtreeEnd(outline.entries, index);
            if (edit == OutlineEditKind::up) {
                std::optional<std::size_t> previousSibling;
                for (std::size_t candidate = 0; candidate < index;
                     ++candidate) {
                    if (outline.entries[candidate].parentEntry ==
                        originalParent) {
                        previousSibling = candidate;
                    }
                }
                if (!previousSibling) {
                    return fail(
                        makeError(NotebookErrorCode::invalidStructuralMove,
                                  path, "the first sibling cannot move up"));
                }
                std::rotate(outline.entries.begin() +
                                static_cast<std::ptrdiff_t>(*previousSibling),
                            outline.entries.begin() +
                                static_cast<std::ptrdiff_t>(index),
                            outline.entries.begin() +
                                static_cast<std::ptrdiff_t>(end));
            } else {
                if (end >= outline.entries.size() ||
                    outline.entries[end].parentEntry != originalParent) {
                    return fail(
                        makeError(NotebookErrorCode::invalidStructuralMove,
                                  path, "the last sibling cannot move down"));
                }
                const auto nextEnd = subtreeEnd(outline.entries, end);
                std::rotate(outline.entries.begin() +
                                static_cast<std::ptrdiff_t>(index),
                            outline.entries.begin() +
                                static_cast<std::ptrdiff_t>(end),
                            outline.entries.begin() +
                                static_cast<std::ptrdiff_t>(nextEnd));
            }
            touch(originalParent);
        }

        if (edit == OutlineEditKind::indent ||
            edit == OutlineEditKind::outdent || edit == OutlineEditKind::up ||
            edit == OutlineEditKind::down) {
            auto moved =
                readBlock(transaction, databases.value().blocks, entryId, path);
            if (!moved) {
                return fail(moved.error());
            }
            auto updated = std::move(moved).value();
            updated.authoredText = editedText;
            updated.metadata.updatedAt = now;
            blockToWrite = updated;
            const auto movedEntry = std::ranges::find_if(
                outline.entries, [&](const auto& entry) -> bool {
                    return entry.metadata.id == entryId;
                });
            movedEntry->metadata.updatedAt = now;
            movedEntry->authoredText = editedText;
        }
        if (blockToWrite) {
            if (auto error = writeBlock(transaction, databases.value().blocks,
                                        *blockToWrite, path)) {
                return fail(std::move(*error));
            }
        }
        if (blockToCreate) {
            if (auto error = writeBlock(transaction, databases.value().blocks,
                                        *blockToCreate, path)) {
                return fail(std::move(*error));
            }
            if (auto error = writeTypeIndex(
                    transaction, databases.value().blocksByType,
                    blockToCreate->type, blockToCreate->metadata.id, path)) {
                return fail(std::move(*error));
            }
        }
        for (const auto& container : touchedContainers) {
            if (auto error = touchContainer(transaction, databases.value(),
                                            outline, container, now)) {
                return fail(std::move(*error));
            }
        }
        if (auto error = rewriteContainment(transaction, databases.value(),
                                            before, outline)) {
            return fail(std::move(*error));
        }
        if (deleteOriginal) {
            auto blockKeyValue = blockKey(entryId);
            result = mdb_del(transaction, databases.value().blocks,
                             &blockKeyValue, nullptr);
            if (result != MDB_SUCCESS) {
                return fail(
                    errorFromLmdb(path, result, "delete Journal Entry"));
            }
            const auto entryType = BlockType::entry;
            auto typeKeyBytes = typeIndexKey(entryType, entryId);
            MDB_val typeKey{typeKeyBytes.size(), typeKeyBytes.data()};
            result = mdb_del(transaction, databases.value().blocksByType,
                             &typeKey, nullptr);
            if (result != MDB_SUCCESS) {
                return fail(errorFromLmdb(path, result,
                                          "delete Journal Entry type index"));
            }
        }
        if (auto error = incrementRevision(transaction,
                                           databases.value().metadata, path)) {
            return fail(std::move(*error));
        }
        result = commitAdapter->commit(transaction);
        if (result != MDB_SUCCESS) {
            return Result<JournalPage>::failure(
                errorFromLmdb(path, result, "commit Journal structural edit"));
        }
        incrementCachedRevision();
        lastCommandCommitted = true;
        if (outline.page.pageKind == PageKind::journal) {
            recordHistory(outline.page.journalDate.value_or(JournalDate{}),
                          before, outline);
        }
        return Result<JournalPage>::success(
            {outline.page.journalDate.value_or(JournalDate{}),
             outline.page.metadata, publicJournalEntries(outline.entries)});
    }

    auto
    deleteSubtrees(const std::vector<BlockId>& entryIds) -> Result<JournalPage>
    {
        lastCommandCommitted = false;
        const auto path = info.value_or(NotebookInfo{}).path;
        if (entryIds.empty()) {
            return Result<JournalPage>::failure(makeError(
                NotebookErrorCode::invalidStructuralMove, path,
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
        auto loaded = loadOutlineForEntry(transaction, databases.value(),
                                          entryIds.front());
        if (!loaded) {
            return fail(loaded.error());
        }
        auto outline = std::move(loaded).value();
        const auto before = outline;
        const auto containsId = [](const std::vector<BlockId>& ids,
                                   const BlockId& identifier) -> bool {
            return std::ranges::find(ids, identifier) != ids.end();
        };
        const auto hasAncestorIn =
            [&](const OutlineEntryRecord& entry,
                const std::vector<BlockId>& ids) -> bool {
            auto ancestor = entry.parentEntry;
            while (ancestor) {
                if (containsId(ids, *ancestor)) {
                    return true;
                }
                const auto found = std::ranges::find_if(
                    before.entries, [&](const auto& candidate) -> bool {
                        return candidate.metadata.id == *ancestor;
                    });
                ancestor = found == before.entries.end() ? std::nullopt
                                                         : found->parentEntry;
            }
            return false;
        };
        for (const auto& id : entryIds) {
            if (std::ranges::none_of(outline.entries,
                                     [&](const auto& entry) -> bool {
                                         return entry.metadata.id == id;
                                     })) {
                return fail(makeError(
                    NotebookErrorCode::blockNotFound, path,
                    "selected Journal Entry is not on the same Page"));
            }
        }

        std::vector<BlockId> roots;
        for (const auto& entry : outline.entries) {
            if (!containsId(entryIds, entry.metadata.id)) {
                continue;
            }
            if (!hasAncestorIn(entry, entryIds)) {
                roots.push_back(entry.metadata.id);
            }
        }
        const auto isDeleted = [&](const OutlineEntryRecord& entry) -> bool {
            return containsId(roots, entry.metadata.id) ||
                   hasAncestorIn(entry, roots);
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
            if (auto error = touchContainer(transaction, databases.value(),
                                            outline, parent, now)) {
                return fail(std::move(*error));
            }
        }
        if (auto error = rewriteContainment(transaction, databases.value(),
                                            before, outline)) {
            return fail(std::move(*error));
        }
        for (const auto& entry : before.entries) {
            if (!isDeleted(entry)) {
                continue;
            }
            auto key = blockKey(entry.metadata.id);
            result =
                mdb_del(transaction, databases.value().blocks, &key, nullptr);
            if (result != MDB_SUCCESS) {
                return fail(errorFromLmdb(path, result,
                                          "delete selected Journal Entry"));
            }
            const auto entryType = BlockType::entry;
            if (auto error =
                    removeTypeIndex(transaction, databases.value().blocksByType,
                                    entryType, entry.metadata.id)) {
                return fail(std::move(*error));
            }
        }
        if (auto error = incrementRevision(transaction,
                                           databases.value().metadata, path)) {
            return fail(std::move(*error));
        }
        result = commitAdapter->commit(transaction);
        if (result != MDB_SUCCESS) {
            return Result<JournalPage>::failure(
                errorFromLmdb(path, result, "commit Journal subtree deletion"));
        }
        incrementCachedRevision();
        lastCommandCommitted = true;
        if (outline.page.pageKind == PageKind::journal) {
            recordHistory(outline.page.journalDate.value_or(JournalDate{}),
                          before, outline);
        }
        return Result<JournalPage>::success(
            {outline.page.journalDate.value_or(JournalDate{}),
             outline.page.metadata, publicJournalEntries(outline.entries)});
    }

    auto
    moveEntryToPage(BlockId entryId, const PageAddress& destinationAddress,
                    std::optional<BlockId> afterEntry)
        -> Result<std::vector<LoadedOutline>>
    {
        lastCommandCommitted = false;
        const auto path = info.value_or(NotebookInfo{}).path;
        MDB_txn* transaction = nullptr;
        auto result = mdb_txn_begin(environment, nullptr, 0, &transaction);
        if (result != MDB_SUCCESS) {
            return Result<std::vector<LoadedOutline>>::failure(
                errorFromLmdb(path, result, "begin cross-Page Entry move"));
        }
        const auto fail =
            [&](NotebookError error) -> Result<std::vector<LoadedOutline>> {
            mdb_txn_abort(transaction);
            return Result<std::vector<LoadedOutline>>::failure(
                std::move(error));
        };
        auto databases = openJournalDatabases(transaction, path);
        if (!databases) {
            return fail(databases.error());
        }
        auto sourceResult =
            loadOutlineForEntry(transaction, databases.value(), entryId);
        if (!sourceResult) {
            return fail(sourceResult.error());
        }
        auto source = std::move(sourceResult).value();
        const auto beforeSource = source;
        std::optional<LoadedOutline> beforeDestination;
        LoadedOutline destination;
        const auto now = currentTimestamp();
        if (const auto* pageId = std::get_if<BlockId>(&destinationAddress)) {
            auto block =
                readBlock(transaction, databases.value().blocks, *pageId, path);
            if (!block || block.value().type != BlockType::page ||
                block.value().pageKind != PageKind::named) {
                return fail(
                    block ? makeError(NotebookErrorCode::pageNotFound, path,
                                      "move destination is not a Named Page")
                          : block.error());
            }
            auto loaded = loadOutline(transaction, databases.value(),
                                      std::move(block).value());
            if (!loaded) {
                return fail(loaded.error());
            }
            destination = std::move(loaded).value();
            beforeDestination = destination;
        } else {
            const auto date = std::get<JournalDate>(destinationAddress);
            auto loaded =
                loadOutlineForDate(transaction, databases.value(), date);
            if (!loaded) {
                return fail(loaded.error());
            }
            auto loadedPage = std::move(loaded).value();
            if (loadedPage) {
                destination = std::move(loadedPage).value_or(LoadedOutline{});
                beforeDestination = destination;
            } else {
                BlockRecord page{BlockType::journalPage,
                                 BlockMetadata{generateBlockId(), now, now},
                                 date,
                                 {},
                                 {},
                                 {},
                                 PageKind::journal};
                if (auto error = writeBlock(
                        transaction, databases.value().blocks, page, path)) {
                    return fail(std::move(*error));
                }
                if (auto error = writeTypeIndex(
                        transaction, databases.value().blocksByType, page.type,
                        page.metadata.id, path)) {
                    return fail(std::move(*error));
                }
                const auto encodedDate = dateKey(date);
                MDB_val dateIndexKey{
                    encodedDate.size(),
                    const_cast<std::uint8_t*>(encodedDate.data())};
                MDB_val pageValue{page.metadata.id.bytes.size(),
                                  page.metadata.id.bytes.data()};
                result = mdb_put(transaction, databases.value().journalByDate,
                                 &dateIndexKey, &pageValue, MDB_NOOVERWRITE);
                if (result != MDB_SUCCESS) {
                    return fail(errorFromLmdb(path, result,
                                              "materialize move destination"));
                }
                destination = {std::move(page), {}};
            }
        }
        if (source.page.metadata.id == destination.page.metadata.id) {
            return fail(
                makeError(NotebookErrorCode::invalidStructuralMove, path,
                          "cross-Page move requires a different destination"));
        }
        const auto found = std::ranges::find_if(
            source.entries, [&](const auto& entry) -> bool {
                return entry.metadata.id == entryId;
            });
        if (found == source.entries.end()) {
            return fail(makeError(NotebookErrorCode::blockNotFound, path,
                                  "Entry is not in its containing Page"));
        }
        const auto sourceIndex = static_cast<std::size_t>(
            std::distance(source.entries.begin(), found));
        const auto sourceEnd = subtreeEnd(source.entries, sourceIndex);
        std::vector<OutlineEntryRecord> moving(
            source.entries.begin() + static_cast<std::ptrdiff_t>(sourceIndex),
            source.entries.begin() + static_cast<std::ptrdiff_t>(sourceEnd));
        const auto oldParent = moving.front().parentEntry;
        source.entries.erase(
            source.entries.begin() + static_cast<std::ptrdiff_t>(sourceIndex),
            source.entries.begin() + static_cast<std::ptrdiff_t>(sourceEnd));

        std::optional<BlockId> newParent;
        auto destinationIndex = destination.entries.size();
        if (afterEntry) {
            const auto insertion = std::ranges::find_if(
                destination.entries, [&](const auto& entry) -> bool {
                    return entry.metadata.id == *afterEntry;
                });
            if (insertion == destination.entries.end()) {
                return fail(makeError(
                    NotebookErrorCode::invalidInsertionPoint, path,
                    "insertion point is not on the destination Page"));
            }
            const auto index = static_cast<std::size_t>(
                std::distance(destination.entries.begin(), insertion));
            newParent = insertion->parentEntry;
            destinationIndex = subtreeEnd(destination.entries, index);
        }
        moving.front().parentEntry = newParent;
        moving.front().metadata.updatedAt = now;
        destination.entries.insert(
            destination.entries.begin() +
                static_cast<std::ptrdiff_t>(destinationIndex),
            moving.begin(), moving.end());

        auto movedBlock =
            readBlock(transaction, databases.value().blocks, entryId, path);
        if (!movedBlock) {
            return fail(movedBlock.error());
        }
        auto movedBlockRecord = std::move(movedBlock).value();
        movedBlockRecord.metadata.updatedAt = now;
        if (auto error = writeBlock(transaction, databases.value().blocks,
                                    movedBlockRecord, path)) {
            return fail(std::move(*error));
        }
        if (auto error = touchContainer(transaction, databases.value(), source,
                                        oldParent, now)) {
            return fail(std::move(*error));
        }
        if (auto error = touchContainer(transaction, databases.value(),
                                        destination, newParent, now)) {
            return fail(std::move(*error));
        }
        if (auto error = rewriteContainment(transaction, databases.value(),
                                            beforeSource, source)) {
            return fail(std::move(*error));
        }
        const LoadedOutline emptyDestination{destination.page, {}};
        if (auto error = rewriteContainment(
                transaction, databases.value(),
                beforeDestination.value_or(emptyDestination), destination)) {
            return fail(std::move(*error));
        }
        if (auto error = incrementRevision(transaction,
                                           databases.value().metadata, path)) {
            return fail(std::move(*error));
        }
        result = commitAdapter->commit(transaction);
        if (result != MDB_SUCCESS) {
            return Result<std::vector<LoadedOutline>>::failure(
                errorFromLmdb(path, result, "commit cross-Page Entry move"));
        }
        incrementCachedRevision();
        lastCommandCommitted = true;
        clearRedoHistory();
        CrossPageHistoryAction action{nextHistorySequence++, beforeSource,
                                      beforeDestination,     source,
                                      destination,           0};
        action.estimatedBytes = sizeof(CrossPageHistoryAction) +
                                estimateOutlineBytes(action.beforeSource) +
                                estimateOutlineBytes(action.beforeDestination) +
                                estimateOutlineBytes(action.afterSource) +
                                estimateOutlineBytes(action.afterDestination);
        historyBytes += action.estimatedBytes;
        crossPageUndo.push_back(std::move(action));
        enforceHistoryBudget();
        return Result<std::vector<LoadedOutline>>::success(
            {std::move(source), std::move(destination)});
    }

    auto
    applyCrossPageHistory(bool redo) -> Result<std::vector<LoadedOutline>>
    {
        lastCommandCommitted = false;
        auto& sourceActions = redo ? crossPageRedo : crossPageUndo;
        if (sourceActions.empty()) {
            return Result<std::vector<LoadedOutline>>::failure(
                makeError(redo ? NotebookErrorCode::redoUnavailable
                               : NotebookErrorCode::undoUnavailable,
                          info.value_or(NotebookInfo{}).path,
                          "no cross-Page edit is available"));
        }
        const auto& action = sourceActions.back();
        const auto& currentSource =
            redo ? action.beforeSource : action.afterSource;
        const auto& currentDestination =
            redo ? action.beforeDestination
                 : std::optional<LoadedOutline>{action.afterDestination};
        const auto& targetSource =
            redo ? action.afterSource : action.beforeSource;
        const auto& targetDestination =
            redo ? std::optional<LoadedOutline>{action.afterDestination}
                 : action.beforeDestination;
        const auto path = info.value_or(NotebookInfo{}).path;
        MDB_txn* transaction = nullptr;
        auto result = mdb_txn_begin(environment, nullptr, 0, &transaction);
        if (result != MDB_SUCCESS) {
            return Result<std::vector<LoadedOutline>>::failure(
                errorFromLmdb(path, result, "begin cross-Page history"));
        }
        const auto fail =
            [&](NotebookError error) -> Result<std::vector<LoadedOutline>> {
            mdb_txn_abort(transaction);
            return Result<std::vector<LoadedOutline>>::failure(
                std::move(error));
        };
        auto databases = openJournalDatabases(transaction, path);
        if (!databases) {
            return fail(databases.error());
        }
        for (const auto& entry : currentSource.entries) {
            if (auto error =
                    eraseContainment(transaction, databases.value(), entry)) {
                return fail(std::move(*error));
            }
        }
        if (currentDestination) {
            for (const auto& entry : currentDestination->entries) {
                if (auto error = eraseContainment(transaction,
                                                  databases.value(), entry)) {
                    return fail(std::move(*error));
                }
            }
        }
        if (auto error = writeBlock(transaction, databases.value().blocks,
                                    targetSource.page, path)) {
            return fail(std::move(*error));
        }
        if (targetDestination) {
            if (auto error = writeBlock(transaction, databases.value().blocks,
                                        targetDestination->page, path)) {
                return fail(std::move(*error));
            }
            if (!currentDestination) {
                if (auto error = writeTypeIndex(
                        transaction, databases.value().blocksByType,
                        targetDestination->page.type,
                        targetDestination->page.metadata.id, path)) {
                    return fail(std::move(*error));
                }
                if (targetDestination->page.journalDate) {
                    const auto encodedDate =
                        dateKey(*targetDestination->page.journalDate);
                    MDB_val dateIndexKey{
                        encodedDate.size(),
                        const_cast<std::uint8_t*>(encodedDate.data())};
                    MDB_val pageValue{
                        targetDestination->page.metadata.id.bytes.size(),
                        const_cast<std::byte*>(
                            targetDestination->page.metadata.id.bytes.data())};
                    result =
                        mdb_put(transaction, databases.value().journalByDate,
                                &dateIndexKey, &pageValue, MDB_NOOVERWRITE);
                    if (result != MDB_SUCCESS) {
                        return fail(errorFromLmdb(
                            path, result, "restore Journal move destination"));
                    }
                }
            }
        } else if (currentDestination) {
            auto pageKey = blockKey(currentDestination->page.metadata.id);
            result = mdb_del(transaction, databases.value().blocks, &pageKey,
                             nullptr);
            if (result != MDB_SUCCESS) {
                return fail(errorFromLmdb(path, result,
                                          "remove virtualized Journal Page"));
            }
            if (auto error =
                    removeTypeIndex(transaction, databases.value().blocksByType,
                                    currentDestination->page.type,
                                    currentDestination->page.metadata.id)) {
                return fail(std::move(*error));
            }
            if (currentDestination->page.journalDate) {
                const auto encodedDate =
                    dateKey(*currentDestination->page.journalDate);
                MDB_val dateIndexKey{
                    encodedDate.size(),
                    const_cast<std::uint8_t*>(encodedDate.data())};
                result = mdb_del(transaction, databases.value().journalByDate,
                                 &dateIndexKey, nullptr);
                if (result != MDB_SUCCESS) {
                    return fail(errorFromLmdb(
                        path, result, "virtualize Journal move destination"));
                }
            }
        }
        const auto writeEntries =
            [&](const LoadedOutline& outline) -> std::optional<NotebookError> {
            for (const auto& entry : outline.entries) {
                auto block = readBlock(transaction, databases.value().blocks,
                                       entry.metadata.id, path);
                if (!block) {
                    return block.error();
                }
                auto restored = std::move(block).value();
                restored.metadata = entry.metadata;
                restored.authoredText = entry.authoredText;
                if (auto error =
                        writeBlock(transaction, databases.value().blocks,
                                   restored, path)) {
                    return error;
                }
            }
            const LoadedOutline empty{outline.page, {}};
            return rewriteContainment(transaction, databases.value(), empty,
                                      outline);
        };
        if (auto error = writeEntries(targetSource)) {
            return fail(std::move(*error));
        }
        if (targetDestination) {
            if (auto error = writeEntries(*targetDestination)) {
                return fail(std::move(*error));
            }
        }
        if (auto error = incrementRevision(transaction,
                                           databases.value().metadata, path)) {
            return fail(std::move(*error));
        }
        result = commitAdapter->commit(transaction);
        if (result != MDB_SUCCESS) {
            return Result<std::vector<LoadedOutline>>::failure(
                errorFromLmdb(path, result, "commit cross-Page history"));
        }
        auto applied = std::move(sourceActions.back());
        sourceActions.pop_back();
        (redo ? crossPageUndo : crossPageRedo).push_back(std::move(applied));
        incrementCachedRevision();
        lastCommandCommitted = true;
        std::vector<LoadedOutline> restored{targetSource};
        if (targetDestination) {
            restored.push_back(*targetDestination);
        }
        return Result<std::vector<LoadedOutline>>::success(std::move(restored));
    }

    void
    incrementCachedRevision()
    {
        auto updated = info.value_or(NotebookInfo{});
        ++updated.revision;
        info = std::move(updated);
    }

    [[nodiscard]] auto
    committedCallbacks() const -> std::vector<std::function<void()>>
    {
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
    std::vector<MechanicalTextChange> lastMechanicalTextChanges;
    std::vector<JournalPageHistory> journalHistory;
    std::vector<PageHistory> pageHistories;
    std::deque<CrossPageHistoryAction> crossPageUndo;
    std::deque<CrossPageHistoryAction> crossPageRedo;
    std::size_t historyBytes{0};
    std::uint64_t nextHistorySequence{1};
};

#ifdef HIEDA_TESTING
void
NotebookSessionTestAccess::rejectNextCommit(NotebookSession& session)
{
    std::scoped_lock lock(session.impl_->mutex);
    session.impl_->commitAdapter =
        std::make_unique<RejectNextJournalCommitAdapter>();
}
#endif

auto
NotebookId::toString() const -> std::string
{
    return formatUuid(*this);
}

auto
BlockId::toString() const -> std::string
{
    return formatUuid(*this);
}

NotebookSubscription::NotebookSubscription() = default;
NotebookSubscription::~NotebookSubscription() = default;
NotebookSubscription::NotebookSubscription(NotebookSubscription&&) noexcept =
    default;
auto NotebookSubscription::operator=(NotebookSubscription&&) noexcept
    -> NotebookSubscription& = default;
NotebookSubscription::NotebookSubscription(std::unique_ptr<Impl> impl)
    : impl_(std::move(impl))
{
}

NotebookSession::NotebookSession() : impl_(std::make_unique<Impl>()) {}

NotebookSession::~NotebookSession() = default;

auto
NotebookSession::create(const std::filesystem::path& inputPath)
    -> Result<NotebookInfo>
{
    std::scoped_lock lock(impl_->mutex);
    if (impl_->info) {
        return Result<NotebookInfo>::failure(
            makeError(NotebookErrorCode::alreadyOpen, inputPath,
                      "a Notebook is already open"));
    }

    std::error_code filesystemError;
    const auto path = std::filesystem::absolute(inputPath, filesystemError)
                          .lexically_normal();
    if (filesystemError || path.filename().empty()) {
        return Result<NotebookInfo>::failure(
            makeError(NotebookErrorCode::invalidPath, inputPath,
                      "invalid Notebook path"));
    }
    if (!std::filesystem::is_directory(path.parent_path(), filesystemError) ||
        filesystemError) {
        return Result<NotebookInfo>::failure(
            makeError(NotebookErrorCode::invalidPath, path,
                      "parent directory does not exist"));
    }
    if (std::filesystem::exists(path, filesystemError)) {
        return Result<NotebookInfo>::failure(
            makeError(NotebookErrorCode::pathExists, path,
                      "the selected path already exists"));
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
            makeError(NotebookErrorCode::pathExists, path,
                      "the selected path already exists"));
    }

    const auto id = generateId();
    const auto temporaryPath = pathWithSuffix(path, ".tmp-" + id.toString());
    const auto temporaryLockPath = pathWithSuffix(temporaryPath, "-lock");
    const auto createdAt =
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count();
    std::optional<NotebookError> creationError;
    try {
        creationError =
            createEnvironment(temporaryPath, Manifest{id, createdAt, 0});
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

    if (const auto publishError =
            platform::publishNewFile(temporaryPath, path)) {
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
        return Result<NotebookInfo>::failure(errorFromPlatform(
            path, *syncError, "flush Notebook parent directory"));
    }

    return impl_->finishOpen(path);
}

auto
NotebookSession::open(const std::filesystem::path& inputPath)
    -> Result<NotebookInfo>
{
    std::scoped_lock lock(impl_->mutex);
    if (impl_->info) {
        return Result<NotebookInfo>::failure(
            makeError(NotebookErrorCode::alreadyOpen, inputPath,
                      "a Notebook is already open"));
    }

    std::error_code filesystemError;
    const auto path = std::filesystem::absolute(inputPath, filesystemError)
                          .lexically_normal();
    if (filesystemError || !std::filesystem::exists(path, filesystemError)) {
        return Result<NotebookInfo>::failure(
            makeError(NotebookErrorCode::pathNotFound, inputPath,
                      "Notebook does not exist"));
    }
    if (!std::filesystem::is_regular_file(path, filesystemError) ||
        filesystemError) {
        return Result<NotebookInfo>::failure(
            makeError(NotebookErrorCode::invalidPath, path,
                      "Notebook path is not a regular file"));
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

void
NotebookSession::close() noexcept
{
    std::scoped_lock lock(impl_->mutex);
    impl_->closeUnlocked();
}

auto
NotebookSession::isOpen() const noexcept -> bool
{
    std::scoped_lock lock(impl_->mutex);
    return impl_->info.has_value();
}

auto
NotebookSession::current() const -> std::optional<NotebookInfo>
{
    std::scoped_lock lock(impl_->mutex);
    return impl_->info;
}

auto
NotebookSession::pages() const -> Result<std::vector<PageSummary>>
{
    std::scoped_lock lock(impl_->mutex);
    if (!impl_->info) {
        return Result<std::vector<PageSummary>>::failure(makeError(
            NotebookErrorCode::notebookNotOpen, {}, "a Notebook must be open"));
    }
    return impl_->readPages();
}

auto
NotebookSession::pageHierarchyNode(std::string name) const
    -> Result<std::optional<PageHierarchyNode>>
{
    std::scoped_lock lock(impl_->mutex);
    if (!impl_->info) {
        return Result<std::optional<PageHierarchyNode>>::failure(makeError(
            NotebookErrorCode::notebookNotOpen, {}, "a Notebook must be open"));
    }
    if (!authored_text::validPageName(name)) {
        return Result<std::optional<PageHierarchyNode>>::failure(
            makeError(NotebookErrorCode::invalidPageName, impl_->info->path,
                      "invalid Page name"));
    }
    return impl_->readHierarchyNode(std::move(name));
}

auto
NotebookSession::pageHierarchyChildren(
    std::optional<std::string> parentName,
    std::optional<std::string> continuationCursor) const
    -> Result<PageHierarchyBatch>
{
    std::scoped_lock lock(impl_->mutex);
    if (!impl_->info) {
        return Result<PageHierarchyBatch>::failure(makeError(
            NotebookErrorCode::notebookNotOpen, {}, "a Notebook must be open"));
    }
    if (parentName && !authored_text::validPageName(*parentName)) {
        return Result<PageHierarchyBatch>::failure(
            makeError(NotebookErrorCode::invalidPageName, impl_->info->path,
                      "invalid parent Page name"));
    }
    return impl_->readHierarchyChildren(parentName, continuationCursor);
}

auto
NotebookSession::page(BlockId pageId) const -> Result<Page>
{
    std::scoped_lock lock(impl_->mutex);
    if (!impl_->info) {
        return Result<Page>::failure(makeError(
            NotebookErrorCode::notebookNotOpen, {}, "a Notebook must be open"));
    }
    return impl_->readPage(pageId);
}

auto
NotebookSession::createPage(std::string name, std::string displayTitle)
    -> Result<Page>
{
    std::vector<std::function<void()>> callbacks;
    auto result = [&]() -> Result<Page> {
        std::scoped_lock lock(impl_->mutex);
        if (!impl_->info) {
            return Result<Page>::failure(
                makeError(NotebookErrorCode::notebookNotOpen, {},
                          "a Notebook must be open"));
        }
        if (!authored_text::validPageName(name)) {
            return Result<Page>::failure(
                makeError(NotebookErrorCode::invalidPageName, impl_->info->path,
                          "invalid Page name"));
        }
        if (!validPageTitle(displayTitle)) {
            return Result<Page>::failure(
                makeError(NotebookErrorCode::invalidPageTitle,
                          impl_->info->path, "invalid Page title"));
        }
        auto outcome =
            impl_->createPage(std::move(name), std::move(displayTitle));
        if (impl_->lastCommandCommitted) {
            callbacks = impl_->committedCallbacks();
        }
        return outcome;
    }();
    notifyCallbacks(callbacks);
    return result;
}

auto
NotebookSession::deletePage(BlockId pageId) -> Result<Page>
{
    std::vector<std::function<void()>> callbacks;
    auto result = [&]() -> Result<Page> {
        std::scoped_lock lock(impl_->mutex);
        if (!impl_->info) {
            return Result<Page>::failure(
                makeError(NotebookErrorCode::notebookNotOpen, {},
                          "a Notebook must be open"));
        }
        auto outcome = impl_->deletePage(pageId);
        if (impl_->lastCommandCommitted) {
            callbacks = impl_->committedCallbacks();
        }
        return outcome;
    }();
    notifyCallbacks(callbacks);
    return result;
}

auto
NotebookSession::renamePage(BlockId pageId, std::string name,
                            std::string displayTitle) -> Result<Page>
{
    std::vector<std::function<void()>> callbacks;
    auto result = [&]() -> Result<Page> {
        std::scoped_lock lock(impl_->mutex);
        if (!impl_->info) {
            return Result<Page>::failure(
                makeError(NotebookErrorCode::notebookNotOpen, {},
                          "a Notebook must be open"));
        }
        if (!authored_text::validPageName(name)) {
            return Result<Page>::failure(
                makeError(NotebookErrorCode::invalidPageName, impl_->info->path,
                          "invalid Page name"));
        }
        if (!validPageTitle(displayTitle)) {
            return Result<Page>::failure(
                makeError(NotebookErrorCode::invalidPageTitle,
                          impl_->info->path, "invalid Page title"));
        }
        auto before = impl_->readPage(pageId);
        if (!before) {
            return before;
        }
        auto outcome =
            impl_->renamePage(pageId, std::move(name), std::move(displayTitle));
        if (outcome && before.value() != outcome.value()) {
            impl_->recordPageHistory(
                std::move(before).value(), outcome.value(),
                std::move(impl_->lastMechanicalTextChanges));
        }
        if (impl_->lastCommandCommitted) {
            callbacks = impl_->committedCallbacks();
        }
        return outcome;
    }();
    notifyCallbacks(callbacks);
    return result;
}

auto
NotebookSession::pageLinks(BlockId entryId) const
    -> Result<std::vector<PageLink>>
{
    std::scoped_lock lock(impl_->mutex);
    if (!impl_->info) {
        return Result<std::vector<PageLink>>::failure(makeError(
            NotebookErrorCode::notebookNotOpen, {}, "a Notebook must be open"));
    }
    return impl_->readPageLinks(entryId);
}

auto
NotebookSession::followPageLink(BlockId entryId,
                                std::size_t sourceByteOffset) const
    -> Result<PageLinkDestination>
{
    std::scoped_lock lock(impl_->mutex);
    if (!impl_->info) {
        return Result<PageLinkDestination>::failure(makeError(
            NotebookErrorCode::notebookNotOpen, {}, "a Notebook must be open"));
    }
    auto links = impl_->readPageLinks(entryId);
    if (!links) {
        return Result<PageLinkDestination>::failure(links.error());
    }
    const auto found =
        std::ranges::find_if(links.value(), [&](const auto& link) -> bool {
            return sourceByteOffset >= link.sourceByteOffset &&
                   sourceByteOffset <
                       link.sourceByteOffset + link.sourceByteLength;
        });
    if (found == links.value().end()) {
        return Result<PageLinkDestination>::failure(
            makeError(NotebookErrorCode::pageLinkNotFound, impl_->info->path,
                      "no committed Page Link contains the source offset"));
    }
    if (found->target) {
        return Result<PageLinkDestination>::success(*found->target);
    }
    auto preview = impl_->readPagePreview(found->pageName);
    return preview ? Result<PageLinkDestination>::success(
                         std::move(preview).value())
                   : Result<PageLinkDestination>::failure(preview.error());
}

auto
NotebookSession::insertBlockReference(BlockId sourceEntryId,
                                      std::size_t sourceByteOffset,
                                      BlockId targetId) -> Result<Entry>
{
    std::string authoredText;
    {
        std::scoped_lock lock(impl_->mutex);
        if (!impl_->info) {
            return Result<Entry>::failure(
                makeError(NotebookErrorCode::notebookNotOpen, {},
                          "a Notebook must be open"));
        }
        auto source = impl_->readEntry(sourceEntryId);
        if (!source) {
            return Result<Entry>::failure(source.error());
        }
        auto target = impl_->blockReferenceDestination(targetId);
        if (!target) {
            return Result<Entry>::failure(target.error());
        }
        authoredText = source.value().authoredText;
        if (sourceByteOffset > authoredText.size() ||
            (sourceByteOffset < authoredText.size() &&
             (static_cast<unsigned char>(authoredText[sourceByteOffset]) &
              0xC0U) == 0x80U)) {
            return Result<Entry>::failure(makeError(
                NotebookErrorCode::invalidCursorPosition, impl_->info->path,
                "Block Reference insertion offset is invalid"));
        }
        authoredText.insert(sourceByteOffset,
                            "[[block:" + targetId.toString() + "]]");
        if (!validAuthoredText(authoredText)) {
            return Result<Entry>::failure(makeError(
                NotebookErrorCode::invalidAuthoredText, impl_->info->path,
                "Block Reference exceeds Authored Text limits"));
        }
    }
    return updateEntry(sourceEntryId, std::move(authoredText));
}

auto
NotebookSession::blockReferences(BlockId entryId) const
    -> Result<std::vector<BlockReference>>
{
    std::scoped_lock lock(impl_->mutex);
    if (!impl_->info) {
        return Result<std::vector<BlockReference>>::failure(makeError(
            NotebookErrorCode::notebookNotOpen, {}, "a Notebook must be open"));
    }
    return impl_->readBlockReferences(entryId);
}

auto
NotebookSession::followBlockReference(BlockId entryId,
                                      std::size_t sourceByteOffset) const
    -> Result<BlockReferenceDestination>
{
    std::scoped_lock lock(impl_->mutex);
    if (!impl_->info) {
        return Result<BlockReferenceDestination>::failure(makeError(
            NotebookErrorCode::notebookNotOpen, {}, "a Notebook must be open"));
    }
    auto references = impl_->readBlockReferences(entryId);
    if (!references) {
        return Result<BlockReferenceDestination>::failure(references.error());
    }
    const auto found = std::ranges::find_if(
        references.value(), [&](const auto& reference) -> bool {
            return sourceByteOffset >= reference.sourceByteOffset &&
                   sourceByteOffset <
                       reference.sourceByteOffset + reference.sourceByteLength;
        });
    if (found == references.value().end()) {
        return Result<BlockReferenceDestination>::failure(makeError(
            NotebookErrorCode::blockReferenceNotFound, impl_->info->path,
            "no Block Reference exists at the requested Authored Text offset"));
    }
    if (!found->target) {
        return Result<BlockReferenceDestination>::failure(
            makeError(NotebookErrorCode::blockNotFound, impl_->info->path,
                      "Block Reference target not found"));
    }
    return impl_->blockReferenceDestination(found->targetId);
}

auto
NotebookSession::locateBlock(BlockId blockId) const
    -> Result<BlockReferenceDestination>
{
    std::scoped_lock lock(impl_->mutex);
    if (!impl_->info) {
        return Result<BlockReferenceDestination>::failure(makeError(
            NotebookErrorCode::notebookNotOpen, {}, "a Notebook must be open"));
    }
    return impl_->blockReferenceDestination(blockId);
}

auto
NotebookSession::linkedReferences(
    BlockId targetId, std::optional<std::string> continuationCursor) const
    -> Result<LinkedReferencesBatch>
{
    std::scoped_lock lock(impl_->mutex);
    if (!impl_->info) {
        return Result<LinkedReferencesBatch>::failure(makeError(
            NotebookErrorCode::notebookNotOpen, {}, "a Notebook must be open"));
    }
    return impl_->readLinkedReferences(targetId, continuationCursor);
}

auto
NotebookSession::linkedReferenceOccurrences(
    BlockId targetId, BlockId sourceId,
    std::optional<std::string> continuationCursor) const
    -> Result<LinkedReferenceOccurrencesBatch>
{
    const auto notebook = current();
    if (!notebook) {
        return Result<LinkedReferenceOccurrencesBatch>::failure(makeError(
            NotebookErrorCode::notebookNotOpen, {}, "a Notebook must be open"));
    }
    std::size_t offset = 0;
    if (continuationCursor) {
        const auto cursor = LinkedReferencesCursor::decode(*continuationCursor);
        if (!cursor || cursor->revision != notebook->revision) {
            return Result<LinkedReferenceOccurrencesBatch>::failure(makeError(
                NotebookErrorCode::staleLinkedReferencesCursor, notebook->path,
                "Linked Reference occurrence cursor is stale or invalid"));
        }
        offset = cursor->offset;
    }
    const auto target = locateBlock(targetId);
    if (!target) {
        return Result<LinkedReferenceOccurrencesBatch>::failure(target.error());
    }
    const auto pageLinksResult = pageLinks(sourceId);
    if (!pageLinksResult) {
        return Result<LinkedReferenceOccurrencesBatch>::failure(
            pageLinksResult.error());
    }
    const auto blockReferencesResult = blockReferences(sourceId);
    if (!blockReferencesResult) {
        return Result<LinkedReferenceOccurrencesBatch>::failure(
            blockReferencesResult.error());
    }
    std::vector<LinkedReferenceOccurrence> occurrences;
    const auto targetIsNamedPage =
        target.value().structuralPage.kind == PageKind::named &&
        target.value().structuralPage.metadata &&
        target.value().structuralPage.metadata->id == targetId;
    if (targetIsNamedPage) {
        for (const auto& link : pageLinksResult.value()) {
            if (link.target && link.target->metadata.id == targetId) {
                occurrences.push_back({SemanticReferenceKind::pageLink,
                                       link.sourceByteOffset,
                                       link.sourceByteLength});
            }
        }
    }
    for (const auto& reference : blockReferencesResult.value()) {
        if (reference.target && reference.targetId == targetId) {
            occurrences.push_back({SemanticReferenceKind::blockReference,
                                   reference.sourceByteOffset,
                                   reference.sourceByteLength});
        }
    }
    std::ranges::sort(occurrences, {},
                      &LinkedReferenceOccurrence::sourceByteOffset);
    const auto total = occurrences.size();
    if (offset > total ||
        current().value_or(NotebookInfo{}).revision != notebook->revision) {
        return Result<LinkedReferenceOccurrencesBatch>::failure(makeError(
            NotebookErrorCode::staleLinkedReferencesCursor, notebook->path,
            "Linked Reference occurrence cursor is outside the current result "
            "set"));
    }
    constexpr std::size_t batchSize = 3;
    const auto end = std::min(total, offset + batchSize);
    std::vector<LinkedReferenceOccurrence> batch(
        occurrences.begin() + static_cast<std::ptrdiff_t>(offset),
        occurrences.begin() + static_cast<std::ptrdiff_t>(end));
    std::optional<std::string> nextCursor;
    if (end < total) {
        nextCursor = LinkedReferencesCursor{notebook->revision, end}.encode();
    }
    return Result<LinkedReferenceOccurrencesBatch>::success(
        {std::move(batch), total, std::move(nextCursor)});
}

auto
NotebookSession::unresolvedPageLinkSources(
    std::string name, std::optional<std::string> continuationCursor) const
    -> Result<UnresolvedPageLinkSourcesBatch>
{
    std::scoped_lock lock(impl_->mutex);
    if (!impl_->info) {
        return Result<UnresolvedPageLinkSourcesBatch>::failure(makeError(
            NotebookErrorCode::notebookNotOpen, {}, "a Notebook must be open"));
    }
    if (!authored_text::validPageName(name)) {
        return Result<UnresolvedPageLinkSourcesBatch>::failure(
            makeError(NotebookErrorCode::invalidPageName, impl_->info->path,
                      "invalid Page Preview name"));
    }
    return impl_->readPagePreviewSources(name, continuationCursor);
}

auto
NotebookSession::unresolvedPageLinkOccurrences(
    std::string name, BlockId sourceId,
    std::optional<std::string> continuationCursor) const
    -> Result<UnresolvedPageLinkOccurrencesBatch>
{
    const auto notebook = current();
    if (!notebook) {
        return Result<UnresolvedPageLinkOccurrencesBatch>::failure(makeError(
            NotebookErrorCode::notebookNotOpen, {}, "a Notebook must be open"));
    }
    std::size_t offset = 0;
    if (continuationCursor) {
        const auto cursor = LinkedReferencesCursor::decode(*continuationCursor);
        if (!cursor || cursor->revision != notebook->revision) {
            return Result<UnresolvedPageLinkOccurrencesBatch>::failure(
                makeError(
                    NotebookErrorCode::staleLinkedReferencesCursor,
                    notebook->path,
                    "Page Preview occurrence cursor is stale or invalid"));
        }
        offset = cursor->offset;
    }
    const auto links = pageLinks(sourceId);
    if (!links) {
        return Result<UnresolvedPageLinkOccurrencesBatch>::failure(
            links.error());
    }
    std::vector<UnresolvedPageLinkOccurrence> occurrences;
    for (const auto& link : links.value()) {
        if (link.pageName == name && !link.target) {
            occurrences.push_back(
                {link.sourceByteOffset, link.sourceByteLength});
        }
    }
    const auto total = occurrences.size();
    if (offset > total ||
        current().value_or(NotebookInfo{}).revision != notebook->revision) {
        return Result<UnresolvedPageLinkOccurrencesBatch>::failure(makeError(
            NotebookErrorCode::staleLinkedReferencesCursor, notebook->path,
            "Page Preview occurrence cursor is outside the current result "
            "set"));
    }
    constexpr std::size_t batchSize = 3;
    const auto end = std::min(total, offset + batchSize);
    std::vector<UnresolvedPageLinkOccurrence> batch(
        occurrences.begin() + static_cast<std::ptrdiff_t>(offset),
        occurrences.begin() + static_cast<std::ptrdiff_t>(end));
    std::optional<std::string> nextCursor;
    if (end < total) {
        nextCursor = LinkedReferencesCursor{notebook->revision, end}.encode();
    }
    return Result<UnresolvedPageLinkOccurrencesBatch>::success(
        {std::move(batch), total, std::move(nextCursor)});
}

// NOLINTNEXTLINE(performance-unnecessary-value-param)
auto
NotebookSession::pagePreview(std::string name) const -> Result<PagePreview>
{
    std::scoped_lock lock(impl_->mutex);
    if (!impl_->info) {
        return Result<PagePreview>::failure(makeError(
            NotebookErrorCode::notebookNotOpen, {}, "a Notebook must be open"));
    }
    if (!authored_text::validPageName(name)) {
        return Result<PagePreview>::failure(
            makeError(NotebookErrorCode::invalidPageName, impl_->info->path,
                      "invalid Page Preview name"));
    }
    return impl_->readPagePreview(name);
}

auto
NotebookSession::insertPageEntry(BlockId pageId,
                                 std::optional<BlockId> afterEntry,
                                 std::string authoredText) -> Result<Page>
{
    std::vector<std::function<void()>> callbacks;
    auto result = [&]() -> Result<Page> {
        std::scoped_lock lock(impl_->mutex);
        if (!impl_->info) {
            return Result<Page>::failure(
                makeError(NotebookErrorCode::notebookNotOpen, {},
                          "a Notebook must be open"));
        }
        if (!validAuthoredText(authoredText)) {
            return Result<Page>::failure(
                makeError(NotebookErrorCode::invalidAuthoredText,
                          impl_->info->path, "invalid Page Entry text"));
        }
        auto before = impl_->readPage(pageId);
        if (!before) {
            return before;
        }
        auto outcome =
            impl_->insertPageEntry(pageId, afterEntry, std::move(authoredText));
        if (outcome) {
            impl_->recordPageHistory(std::move(before).value(),
                                     outcome.value());
        }
        if (impl_->lastCommandCommitted) {
            callbacks = impl_->committedCallbacks();
        }
        return outcome;
    }();
    notifyCallbacks(callbacks);
    return result;
}

auto
NotebookSession::updatePageEntry(BlockId entryId, std::string authoredText)
    -> Result<Entry>
{
    std::vector<std::function<void()>> callbacks;
    auto result = [&]() -> Result<Entry> {
        std::scoped_lock lock(impl_->mutex);
        if (!impl_->info) {
            return Result<Entry>::failure(
                makeError(NotebookErrorCode::notebookNotOpen, {},
                          "a Notebook must be open"));
        }
        if (!validAuthoredText(authoredText)) {
            return Result<Entry>::failure(
                makeError(NotebookErrorCode::invalidAuthoredText,
                          impl_->info->path, "invalid Page Entry text"));
        }
        auto pageId = impl_->pageIdForEntry(entryId);
        if (!pageId) {
            return Result<Entry>::failure(pageId.error());
        }
        auto before = impl_->readPage(pageId.value());
        if (!before) {
            return Result<Entry>::failure(before.error());
        }
        auto outcome = impl_->updateEntry(entryId, std::move(authoredText));
        if (!outcome) {
            return Result<Entry>::failure(outcome.error());
        }
        auto after = impl_->readPage(pageId.value());
        if (!after) {
            return Result<Entry>::failure(after.error());
        }
        if (before.value() != after.value()) {
            impl_->recordPageHistory(std::move(before).value(),
                                     std::move(after).value());
        }
        if (impl_->lastCommandCommitted) {
            callbacks = impl_->committedCallbacks();
        }
        return Result<Entry>::success({outcome.value().metadata,
                                       outcome.value().authoredText,
                                       outcome.value().parentEntry});
    }();
    notifyCallbacks(callbacks);
    return result;
}

auto
NotebookSession::splitPageEntry(BlockId entryId, std::string authoredText,
                                std::size_t cursorByteOffset) -> Result<Page>
{
    return runPageCommand([&]() -> Result<Page> {
        if (!impl_->info) {
            return Result<Page>::failure(
                makeError(NotebookErrorCode::notebookNotOpen, {},
                          "a Notebook must be open"));
        }
        if (!validAuthoredText(authoredText)) {
            return Result<Page>::failure(
                makeError(NotebookErrorCode::invalidAuthoredText,
                          impl_->info->path, "invalid Page Entry text"));
        }
        auto pageId = impl_->pageIdForEntry(entryId);
        if (!pageId) {
            return Result<Page>::failure(pageId.error());
        }
        auto before = impl_->readPage(pageId.value());
        auto outcome = impl_->pageAfterOutline(
            impl_->editOutline(entryId, OutlineEditKind::split,
                               cursorByteOffset, std::move(authoredText)));
        if (before && outcome) {
            impl_->recordPageHistory(std::move(before).value(),
                                     outcome.value());
        }
        return outcome;
    });
}

auto
NotebookSession::joinPageEntry(BlockId entryId, std::string authoredText)
    -> Result<Page>
{
    return runPageCommand([&]() -> Result<Page> {
        if (!impl_->info) {
            return Result<Page>::failure(
                makeError(NotebookErrorCode::notebookNotOpen, {},
                          "a Notebook must be open"));
        }
        if (!validAuthoredText(authoredText)) {
            return Result<Page>::failure(
                makeError(NotebookErrorCode::invalidAuthoredText,
                          impl_->info->path, "invalid Page Entry text"));
        }
        auto pageId = impl_->pageIdForEntry(entryId);
        if (!pageId) {
            return Result<Page>::failure(pageId.error());
        }
        auto before = impl_->readPage(pageId.value());
        auto outcome = impl_->pageAfterOutline(impl_->editOutline(
            entryId, OutlineEditKind::join, 0, std::move(authoredText)));
        if (before && outcome) {
            impl_->recordPageHistory(std::move(before).value(),
                                     outcome.value());
        }
        return outcome;
    });
}

auto
NotebookSession::movePageEntry(BlockId entryId, EntryMove movement,
                               std::string authoredText) -> Result<Page>
{
    auto edit = OutlineEditKind::indent;
    if (movement == EntryMove::outdent) {
        edit = OutlineEditKind::outdent;
    } else if (movement == EntryMove::up) {
        edit = OutlineEditKind::up;
    } else if (movement == EntryMove::down) {
        edit = OutlineEditKind::down;
    }
    return runPageCommand([&]() -> Result<Page> {
        if (!impl_->info) {
            return Result<Page>::failure(
                makeError(NotebookErrorCode::notebookNotOpen, {},
                          "a Notebook must be open"));
        }
        if (!validAuthoredText(authoredText)) {
            return Result<Page>::failure(
                makeError(NotebookErrorCode::invalidAuthoredText,
                          impl_->info->path, "invalid Page Entry text"));
        }
        auto pageId = impl_->pageIdForEntry(entryId);
        if (!pageId) {
            return Result<Page>::failure(pageId.error());
        }
        auto before = impl_->readPage(pageId.value());
        auto outcome = impl_->pageAfterOutline(
            impl_->editOutline(entryId, edit, 0, std::move(authoredText)));
        if (before && outcome) {
            impl_->recordPageHistory(std::move(before).value(),
                                     outcome.value());
        }
        return outcome;
    });
}

auto
NotebookSession::deletePageEntry(BlockId entryId) -> Result<Page>
{
    return runPageCommand([&]() -> Result<Page> {
        if (!impl_->info) {
            return Result<Page>::failure(
                makeError(NotebookErrorCode::notebookNotOpen, {},
                          "a Notebook must be open"));
        }
        auto pageId = impl_->pageIdForEntry(entryId);
        if (!pageId) {
            return Result<Page>::failure(pageId.error());
        }
        auto before = impl_->readPage(pageId.value());
        auto outcome = impl_->pageAfterOutline(
            impl_->editOutline(entryId, OutlineEditKind::erase));
        if (before && outcome) {
            impl_->recordPageHistory(std::move(before).value(),
                                     outcome.value());
        }
        return outcome;
    });
}

auto
NotebookSession::deletePageSubtrees(std::vector<BlockId> entryIds)
    -> Result<Page>
{
    return runPageCommand([&]() -> Result<Page> {
        if (!impl_->info) {
            return Result<Page>::failure(
                makeError(NotebookErrorCode::notebookNotOpen, {},
                          "a Notebook must be open"));
        }
        if (entryIds.empty()) {
            return Result<Page>::failure(
                makeError(NotebookErrorCode::invalidStructuralMove,
                          impl_->info->path, "no Page Entry selected"));
        }
        auto pageId = impl_->pageIdForEntry(entryIds.front());
        if (!pageId) {
            return Result<Page>::failure(pageId.error());
        }
        auto before = impl_->readPage(pageId.value());
        auto outcome = impl_->pageAfterOutline(impl_->deleteSubtrees(entryIds));
        if (before && outcome) {
            impl_->recordPageHistory(std::move(before).value(),
                                     outcome.value());
        }
        return outcome;
    });
}

auto
NotebookSession::runPageCommand(std::function<Result<Page>()> command)
    -> Result<Page>
{
    std::vector<std::function<void()>> callbacks;
    auto result = [&]() -> Result<Page> {
        std::scoped_lock lock(impl_->mutex);
        if (!impl_->info) {
            return Result<Page>::failure(
                makeError(NotebookErrorCode::notebookNotOpen, {},
                          "a Notebook must be open"));
        }
        impl_->lastCommandCommitted = false;
        auto outcome = command();
        if (impl_->lastCommandCommitted) {
            callbacks = impl_->committedCallbacks();
        }
        return outcome;
    }();
    notifyCallbacks(callbacks);
    return result;
}

auto
NotebookSession::journalPage(JournalDate date) const -> Result<JournalPage>
{
    std::scoped_lock lock(impl_->mutex);
    if (!impl_->info) {
        return Result<JournalPage>::failure(makeError(
            NotebookErrorCode::notebookNotOpen, {}, "a Notebook must be open"));
    }
    if (!validJournalDate(date)) {
        return Result<JournalPage>::failure(
            makeError(NotebookErrorCode::invalidJournalDate, impl_->info->path,
                      "invalid Journal date"));
    }
    return impl_->readNestedJournalPage(date);
}

auto
NotebookSession::insertJournalEntry(JournalDate date,
                                    std::optional<BlockId> afterEntry,
                                    std::string authoredText)
    -> Result<JournalPage>
{
    std::vector<std::function<void()>> callbacks;
    auto result = [&]() -> Result<JournalPage> {
        std::scoped_lock lock(impl_->mutex);
        if (!impl_->info) {
            return Result<JournalPage>::failure(
                makeError(NotebookErrorCode::notebookNotOpen, {},
                          "a Notebook must be open"));
        }
        if (!validJournalDate(date)) {
            return Result<JournalPage>::failure(
                makeError(NotebookErrorCode::invalidJournalDate,
                          impl_->info->path, "invalid Journal date"));
        }
        if (!validAuthoredText(authoredText)) {
            return Result<JournalPage>::failure(makeError(
                NotebookErrorCode::invalidAuthoredText, impl_->info->path,
                "Journal Entry text must be bounded Unicode text using LF line "
                "breaks"));
        }
        auto outcome =
            impl_->insertNestedEntry(date, afterEntry, std::move(authoredText));
        if (impl_->lastCommandCommitted) {
            callbacks = impl_->committedCallbacks();
        }
        return outcome;
    }();
    notifyCallbacks(callbacks);
    return result;
}

auto
NotebookSession::updateJournalEntry(BlockId entryId, std::string authoredText)
    -> Result<Entry>
{
    std::vector<std::function<void()>> callbacks;
    auto result = [&]() -> Result<Entry> {
        std::scoped_lock lock(impl_->mutex);
        if (!impl_->info) {
            return Result<Entry>::failure(
                makeError(NotebookErrorCode::notebookNotOpen, {},
                          "a Notebook must be open"));
        }
        if (!validAuthoredText(authoredText)) {
            return Result<Entry>::failure(makeError(
                NotebookErrorCode::invalidAuthoredText, impl_->info->path,
                "Journal Entry text must be bounded Unicode text using LF line "
                "breaks"));
        }
        auto outcome = impl_->updateEntry(entryId, std::move(authoredText));
        if (impl_->lastCommandCommitted) {
            callbacks = impl_->committedCallbacks();
        }
        if (!outcome) {
            return Result<Entry>::failure(outcome.error());
        }
        auto entry = std::move(outcome).value();
        return Result<Entry>::success(
            {entry.metadata, std::move(entry.authoredText), entry.parentEntry});
    }();
    notifyCallbacks(callbacks);
    return result;
}

auto
NotebookSession::splitJournalEntry(BlockId entryId, std::string authoredText,
                                   std::size_t cursorByteOffset)
    -> Result<JournalPage>
{
    std::vector<std::function<void()>> callbacks;
    auto result = [&]() -> Result<JournalPage> {
        std::scoped_lock lock(impl_->mutex);
        if (!impl_->info) {
            return Result<JournalPage>::failure(
                makeError(NotebookErrorCode::notebookNotOpen, {},
                          "a Notebook must be open"));
        }
        if (!validAuthoredText(authoredText)) {
            return Result<JournalPage>::failure(makeError(
                NotebookErrorCode::invalidAuthoredText, impl_->info->path,
                "Journal Entry text must be bounded Unicode text using LF line "
                "breaks"));
        }
        auto outcome =
            impl_->editOutline(entryId, OutlineEditKind::split,
                               cursorByteOffset, std::move(authoredText));
        if (impl_->lastCommandCommitted) {
            callbacks = impl_->committedCallbacks();
        }
        return outcome;
    }();
    notifyCallbacks(callbacks);
    return result;
}

auto
NotebookSession::joinJournalEntry(BlockId entryId, std::string authoredText)
    -> Result<JournalPage>
{
    std::vector<std::function<void()>> callbacks;
    auto result = [&]() -> Result<JournalPage> {
        std::scoped_lock lock(impl_->mutex);
        if (!impl_->info) {
            return Result<JournalPage>::failure(
                makeError(NotebookErrorCode::notebookNotOpen, {},
                          "a Notebook must be open"));
        }
        if (!validAuthoredText(authoredText)) {
            return Result<JournalPage>::failure(makeError(
                NotebookErrorCode::invalidAuthoredText, impl_->info->path,
                "Journal Entry text must be bounded Unicode text using LF line "
                "breaks"));
        }
        auto outcome = impl_->editOutline(entryId, OutlineEditKind::join, 0,
                                          std::move(authoredText));
        if (impl_->lastCommandCommitted) {
            callbacks = impl_->committedCallbacks();
        }
        return outcome;
    }();
    notifyCallbacks(callbacks);
    return result;
}

auto
NotebookSession::moveJournalEntry(BlockId entryId, EntryMove movement,
                                  std::string authoredText)
    -> Result<JournalPage>
{
    auto edit = OutlineEditKind::indent;
    switch (movement) {
    case EntryMove::indent:
        edit = OutlineEditKind::indent;
        break;
    case EntryMove::outdent:
        edit = OutlineEditKind::outdent;
        break;
    case EntryMove::up:
        edit = OutlineEditKind::up;
        break;
    case EntryMove::down:
        edit = OutlineEditKind::down;
        break;
    }
    std::vector<std::function<void()>> callbacks;
    auto result = [&]() -> Result<JournalPage> {
        std::scoped_lock lock(impl_->mutex);
        if (!impl_->info) {
            return Result<JournalPage>::failure(
                makeError(NotebookErrorCode::notebookNotOpen, {},
                          "a Notebook must be open"));
        }
        if (!validAuthoredText(authoredText)) {
            return Result<JournalPage>::failure(makeError(
                NotebookErrorCode::invalidAuthoredText, impl_->info->path,
                "Journal Entry text must be bounded Unicode text using LF line "
                "breaks"));
        }
        auto outcome =
            impl_->editOutline(entryId, edit, 0, std::move(authoredText));
        if (impl_->lastCommandCommitted) {
            callbacks = impl_->committedCallbacks();
        }
        return outcome;
    }();
    notifyCallbacks(callbacks);
    return result;
}

auto
NotebookSession::deleteJournalEntry(BlockId entryId) -> Result<JournalPage>
{
    std::vector<std::function<void()>> callbacks;
    auto result = [&]() -> Result<JournalPage> {
        std::scoped_lock lock(impl_->mutex);
        if (!impl_->info) {
            return Result<JournalPage>::failure(
                makeError(NotebookErrorCode::notebookNotOpen, {},
                          "a Notebook must be open"));
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

auto
NotebookSession::deleteJournalSubtrees(std::vector<BlockId> entryIds)
    -> Result<JournalPage>
{
    std::vector<std::function<void()>> callbacks;
    auto result = [&]() -> Result<JournalPage> {
        std::scoped_lock lock(impl_->mutex);
        if (!impl_->info) {
            return Result<JournalPage>::failure(
                makeError(NotebookErrorCode::notebookNotOpen, {},
                          "a Notebook must be open"));
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

namespace {

auto
unifiedPage(const Page& page) -> OutlinePage
{
    OutlinePage result{PageKind::named, page.metadata,     std::nullopt,
                       page.name,       page.displayTitle, {}};
    result.entries.reserve(page.entries.size());
    for (const auto& entry : page.entries) {
        result.entries.push_back(
            {entry.metadata, entry.authoredText, entry.parentEntry});
    }
    return result;
}

auto
unifiedPage(const JournalPage& page) -> OutlinePage
{
    OutlinePage result{PageKind::journal, page.metadata, page.date, {}, {}, {}};
    result.entries.reserve(page.entries.size());
    for (const auto& entry : page.entries) {
        result.entries.push_back(
            {entry.metadata, entry.authoredText, entry.parentEntry});
    }
    return result;
}

auto
unifiedPage(const LoadedOutline& outline) -> OutlinePage
{
    OutlinePage result;
    if (outline.page.pageKind == PageKind::journal) {
        result.kind = PageKind::journal;
        result.journalDate = outline.page.journalDate;
    } else {
        result.kind = PageKind::named;
        result.name = outline.page.pageName;
        result.displayTitle = outline.page.displayTitle;
    }
    result.metadata = outline.page.metadata;
    result.entries.reserve(outline.entries.size());
    for (const auto& entry : outline.entries) {
        result.entries.push_back(
            {entry.metadata, entry.authoredText, entry.parentEntry});
    }
    return result;
}

} // namespace

auto
NotebookSession::outline(PageAddress address) const -> Result<OutlinePage>
{
    if (const auto* pageId = std::get_if<BlockId>(&address)) {
        const auto result = page(*pageId);
        return result
                   ? Result<OutlinePage>::success(unifiedPage(result.value()))
                   : Result<OutlinePage>::failure(result.error());
    }
    const auto result = journalPage(std::get<JournalDate>(address));
    return result ? Result<OutlinePage>::success(unifiedPage(result.value()))
                  : Result<OutlinePage>::failure(result.error());
}

auto
NotebookSession::insertEntry(PageAddress address,
                             std::optional<BlockId> afterEntry,
                             std::string authoredText) -> Result<OutlinePage>
{
    if (const auto* pageId = std::get_if<BlockId>(&address)) {
        auto result =
            insertPageEntry(*pageId, afterEntry, std::move(authoredText));
        return result
                   ? Result<OutlinePage>::success(unifiedPage(result.value()))
                   : Result<OutlinePage>::failure(result.error());
    }
    auto result = insertJournalEntry(std::get<JournalDate>(address), afterEntry,
                                     std::move(authoredText));
    return result ? Result<OutlinePage>::success(unifiedPage(result.value()))
                  : Result<OutlinePage>::failure(result.error());
}

auto
NotebookSession::entryPageKind(BlockId entryId) const -> Result<PageKind>
{
    std::scoped_lock lock(impl_->mutex);
    if (!impl_->info) {
        return Result<PageKind>::failure(makeError(
            NotebookErrorCode::notebookNotOpen, {}, "a Notebook must be open"));
    }
    return impl_->pageKindForEntry(entryId);
}

auto
NotebookSession::updateEntry(BlockId entryId, std::string authoredText)
    -> Result<Entry>
{
    const auto kind = entryPageKind(entryId);
    if (!kind) {
        return Result<Entry>::failure(kind.error());
    }
    if (kind.value() == PageKind::named) {
        auto named = updatePageEntry(entryId, std::move(authoredText));
        if (!named) {
            return Result<Entry>::failure(named.error());
        }
        const auto& entry = named.value();
        return Result<Entry>::success(
            {entry.metadata, entry.authoredText, entry.parentEntry});
    }
    auto journal = updateJournalEntry(entryId, std::move(authoredText));
    if (!journal) {
        return Result<Entry>::failure(journal.error());
    }
    const auto& entry = journal.value();
    return Result<Entry>::success(
        {entry.metadata, entry.authoredText, entry.parentEntry});
}

auto
NotebookSession::splitEntry(BlockId entryId, std::string authoredText,
                            std::size_t cursorByteOffset) -> Result<OutlinePage>
{
    const auto kind = entryPageKind(entryId);
    if (!kind) {
        return Result<OutlinePage>::failure(kind.error());
    }
    if (kind.value() == PageKind::named) {
        auto named =
            splitPageEntry(entryId, std::move(authoredText), cursorByteOffset);
        if (!named) {
            return Result<OutlinePage>::failure(named.error());
        }
        return Result<OutlinePage>::success(unifiedPage(named.value()));
    }
    auto journal =
        splitJournalEntry(entryId, std::move(authoredText), cursorByteOffset);
    return journal ? Result<OutlinePage>::success(unifiedPage(journal.value()))
                   : Result<OutlinePage>::failure(journal.error());
}

auto
NotebookSession::joinEntry(BlockId entryId, std::string authoredText)
    -> Result<OutlinePage>
{
    const auto kind = entryPageKind(entryId);
    if (!kind) {
        return Result<OutlinePage>::failure(kind.error());
    }
    if (kind.value() == PageKind::named) {
        auto named = joinPageEntry(entryId, std::move(authoredText));
        if (!named) {
            return Result<OutlinePage>::failure(named.error());
        }
        return Result<OutlinePage>::success(unifiedPage(named.value()));
    }
    auto journal = joinJournalEntry(entryId, std::move(authoredText));
    return journal ? Result<OutlinePage>::success(unifiedPage(journal.value()))
                   : Result<OutlinePage>::failure(journal.error());
}

auto
NotebookSession::moveEntry(BlockId entryId, EntryMove movement,
                           std::string authoredText) -> Result<OutlinePage>
{
    const auto kind = entryPageKind(entryId);
    if (!kind) {
        return Result<OutlinePage>::failure(kind.error());
    }
    if (kind.value() == PageKind::named) {
        auto named = movePageEntry(entryId, movement, std::move(authoredText));
        if (!named) {
            return Result<OutlinePage>::failure(named.error());
        }
        return Result<OutlinePage>::success(unifiedPage(named.value()));
    }
    auto journal = moveJournalEntry(entryId, movement, std::move(authoredText));
    return journal ? Result<OutlinePage>::success(unifiedPage(journal.value()))
                   : Result<OutlinePage>::failure(journal.error());
}

auto
NotebookSession::moveEntryToPage(BlockId entryId, PageAddress destination,
                                 std::optional<BlockId> afterEntry)
    -> Result<std::vector<OutlinePage>>
{
    std::vector<std::function<void()>> callbacks;
    auto result = [&]() -> Result<std::vector<OutlinePage>> {
        std::scoped_lock lock(impl_->mutex);
        if (!impl_->info) {
            return Result<std::vector<OutlinePage>>::failure(
                makeError(NotebookErrorCode::notebookNotOpen, {},
                          "a Notebook must be open"));
        }
        if (const auto* date = std::get_if<JournalDate>(&destination);
            date != nullptr && !validJournalDate(*date)) {
            return Result<std::vector<OutlinePage>>::failure(
                makeError(NotebookErrorCode::invalidJournalDate,
                          impl_->info->path, "invalid Journal date"));
        }
        auto moved = impl_->moveEntryToPage(entryId, destination, afterEntry);
        if (!moved) {
            return Result<std::vector<OutlinePage>>::failure(moved.error());
        }
        std::vector<OutlinePage> pages;
        pages.reserve(moved.value().size());
        for (const auto& page : moved.value()) {
            pages.push_back(unifiedPage(page));
        }
        callbacks = impl_->committedCallbacks();
        return Result<std::vector<OutlinePage>>::success(std::move(pages));
    }();
    notifyCallbacks(callbacks);
    return result;
}

auto
NotebookSession::deleteEntry(BlockId entryId) -> Result<OutlinePage>
{
    const auto kind = entryPageKind(entryId);
    if (!kind) {
        return Result<OutlinePage>::failure(kind.error());
    }
    if (kind.value() == PageKind::named) {
        auto named = deletePageEntry(entryId);
        if (!named) {
            return Result<OutlinePage>::failure(named.error());
        }
        return Result<OutlinePage>::success(unifiedPage(named.value()));
    }
    auto journal = deleteJournalEntry(entryId);
    return journal ? Result<OutlinePage>::success(unifiedPage(journal.value()))
                   : Result<OutlinePage>::failure(journal.error());
}

auto
NotebookSession::deleteSubtrees(std::vector<BlockId> entryIds)
    -> Result<OutlinePage>
{
    if (entryIds.empty()) {
        return Result<OutlinePage>::failure(
            makeError(NotebookErrorCode::invalidStructuralMove,
                      current().value_or(NotebookInfo{}).path,
                      "at least one Entry must be selected"));
    }
    const auto kind = entryPageKind(entryIds.front());
    if (!kind) {
        return Result<OutlinePage>::failure(kind.error());
    }
    if (kind.value() == PageKind::named) {
        auto named = deletePageSubtrees(std::move(entryIds));
        if (!named) {
            return Result<OutlinePage>::failure(named.error());
        }
        return Result<OutlinePage>::success(unifiedPage(named.value()));
    }
    auto journal = deleteJournalSubtrees(std::move(entryIds));
    return journal ? Result<OutlinePage>::success(unifiedPage(journal.value()))
                   : Result<OutlinePage>::failure(journal.error());
}

auto
NotebookSession::editCapabilities() const -> Result<EditCapabilities>
{
    std::scoped_lock lock(impl_->mutex);
    if (!impl_->info) {
        return Result<EditCapabilities>::failure(makeError(
            NotebookErrorCode::notebookNotOpen, {}, "a Notebook must be open"));
    }
    bool canUndo = !impl_->crossPageUndo.empty();
    bool canRedo = !impl_->crossPageRedo.empty();
    for (const auto& history : impl_->journalHistory) {
        canUndo = canUndo || !history.undo.empty();
        canRedo = canRedo || !history.redo.empty();
    }
    for (const auto& history : impl_->pageHistories) {
        canUndo = canUndo || !history.undo.empty();
        canRedo = canRedo || !history.redo.empty();
    }
    return Result<EditCapabilities>::success({canUndo, canRedo});
}

auto
NotebookSession::undoEdit() -> Result<std::vector<OutlinePage>>
{
    std::vector<std::function<void()>> callbacks;
    auto result = [&]() -> Result<std::vector<OutlinePage>> {
        std::scoped_lock lock(impl_->mutex);
        if (!impl_->info) {
            return Result<std::vector<OutlinePage>>::failure(
                makeError(NotebookErrorCode::notebookNotOpen, {},
                          "a Notebook must be open"));
        }
        enum class Kind : std::uint8_t { none, journal, named, cross };
        Kind kind = Kind::none;
        std::uint64_t sequence = 0;
        JournalDate journalDate{};
        BlockId pageId{};
        for (const auto& history : impl_->journalHistory) {
            if (!history.undo.empty() &&
                history.undo.back().sequence > sequence) {
                kind = Kind::journal;
                sequence = history.undo.back().sequence;
                journalDate = history.date;
            }
        }
        for (const auto& history : impl_->pageHistories) {
            if (!history.undo.empty() &&
                history.undo.back().sequence > sequence) {
                kind = Kind::named;
                sequence = history.undo.back().sequence;
                pageId = history.pageId;
            }
        }
        if (!impl_->crossPageUndo.empty() &&
            impl_->crossPageUndo.back().sequence > sequence) {
            kind = Kind::cross;
        }
        std::vector<OutlinePage> pages;
        if (kind == Kind::journal) {
            auto applied = impl_->applyHistory(journalDate, false);
            if (!applied) {
                return Result<std::vector<OutlinePage>>::failure(
                    applied.error());
            }
            pages.push_back(unifiedPage(applied.value()));
        } else if (kind == Kind::named) {
            auto applied = impl_->applyPageHistory(pageId, false);
            if (!applied) {
                return Result<std::vector<OutlinePage>>::failure(
                    applied.error());
            }
            if (applied.value()) {
                pages.push_back(unifiedPage(*applied.value()));
            }
        } else if (kind == Kind::cross) {
            auto applied = impl_->applyCrossPageHistory(false);
            if (!applied) {
                return Result<std::vector<OutlinePage>>::failure(
                    applied.error());
            }
            for (const auto& page : applied.value()) {
                pages.push_back(unifiedPage(page));
            }
        } else {
            return Result<std::vector<OutlinePage>>::failure(
                makeError(NotebookErrorCode::undoUnavailable, impl_->info->path,
                          "no Notebook edit is available to undo"));
        }
        callbacks = impl_->committedCallbacks();
        return Result<std::vector<OutlinePage>>::success(std::move(pages));
    }();
    notifyCallbacks(callbacks);
    return result;
}

auto
NotebookSession::redoEdit() -> Result<std::vector<OutlinePage>>
{
    std::vector<std::function<void()>> callbacks;
    auto result = [&]() -> Result<std::vector<OutlinePage>> {
        std::scoped_lock lock(impl_->mutex);
        if (!impl_->info) {
            return Result<std::vector<OutlinePage>>::failure(
                makeError(NotebookErrorCode::notebookNotOpen, {},
                          "a Notebook must be open"));
        }
        enum class Kind : std::uint8_t { none, journal, named, cross };
        Kind kind = Kind::none;
        auto sequence = std::numeric_limits<std::uint64_t>::max();
        JournalDate journalDate{};
        BlockId pageId{};
        for (const auto& history : impl_->journalHistory) {
            if (!history.redo.empty() &&
                history.redo.back().sequence < sequence) {
                kind = Kind::journal;
                sequence = history.redo.back().sequence;
                journalDate = history.date;
            }
        }
        for (const auto& history : impl_->pageHistories) {
            if (!history.redo.empty() &&
                history.redo.back().sequence < sequence) {
                kind = Kind::named;
                sequence = history.redo.back().sequence;
                pageId = history.pageId;
            }
        }
        if (!impl_->crossPageRedo.empty() &&
            impl_->crossPageRedo.back().sequence < sequence) {
            kind = Kind::cross;
        }
        std::vector<OutlinePage> pages;
        if (kind == Kind::journal) {
            auto applied = impl_->applyHistory(journalDate, true);
            if (!applied) {
                return Result<std::vector<OutlinePage>>::failure(
                    applied.error());
            }
            pages.push_back(unifiedPage(applied.value()));
        } else if (kind == Kind::named) {
            auto applied = impl_->applyPageHistory(pageId, true);
            if (!applied) {
                return Result<std::vector<OutlinePage>>::failure(
                    applied.error());
            }
            if (applied.value()) {
                pages.push_back(unifiedPage(*applied.value()));
            }
        } else if (kind == Kind::cross) {
            auto applied = impl_->applyCrossPageHistory(true);
            if (!applied) {
                return Result<std::vector<OutlinePage>>::failure(
                    applied.error());
            }
            for (const auto& page : applied.value()) {
                pages.push_back(unifiedPage(page));
            }
        } else {
            return Result<std::vector<OutlinePage>>::failure(
                makeError(NotebookErrorCode::redoUnavailable, impl_->info->path,
                          "no Notebook edit is available to redo"));
        }
        callbacks = impl_->committedCallbacks();
        return Result<std::vector<OutlinePage>>::success(std::move(pages));
    }();
    notifyCallbacks(callbacks);
    return result;
}

auto
NotebookSession::subscribeToChanges(std::function<void()> callback)
    -> NotebookSubscription
{
    const auto state = impl_->subscriptions;
    std::scoped_lock lock(state->mutex);
    const auto identifier = state->nextIdentifier++;
    state->callbacks.emplace(identifier, std::move(callback));
    return NotebookSubscription(
        std::make_unique<NotebookSubscription::Impl>(state, identifier));
}

} // namespace hieda::notebook
