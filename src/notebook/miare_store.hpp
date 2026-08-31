// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <filesystem>
#include <memory>
#include <miare/database.hpp>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace hieda::notebook {

// This private adapter preserves the Notebook implementation's small
// transaction vocabulary while assigning each logical database a stable Miare
// key prefix. It is deliberately not part of the public Notebook interface.
constexpr int storeSuccess = 0;
constexpr int storeNotFound = -1;
constexpr int storeKeyExists = -2;
constexpr int storeInvalid = -3;
constexpr int storeCorrupt = -4;
constexpr int storeVersionMismatch = -5;
constexpr int storeIncompatible = -6;
constexpr int storePanic = -7;
constexpr int storeThreadLimit = -8;
constexpr int storeBadTransaction = -9;
constexpr int storeBadReader = -10;
constexpr int storeBadValue = -11;
constexpr int storeBadDatabase = -12;
constexpr int storeDatabaseLimit = -13;
constexpr int storeCapacityLimit = -14;
constexpr int storeCursorLimit = -15;
constexpr int storePageNotFound = -16;
constexpr unsigned storeReadOnly = 0x01U;
constexpr unsigned storeCreate = 0x02U;
constexpr unsigned storeNoOverwrite = 0x04U;
constexpr unsigned storeFirst = 1U;
constexpr unsigned storeNext = 2U;
constexpr unsigned storeSetRange = 3U;

struct StoreValue {
    std::size_t size{};
    void* data{};
};

using LogicalDatabase = std::uint8_t;
using MiareDatabase = miare::Database<>;
using MiareReadTransaction = MiareDatabase::ReadTransaction;
using MiareWriteTransaction = MiareDatabase::WriteTransaction;

struct StoreEnvironment {
    std::filesystem::path path;
    std::unique_ptr<MiareDatabase> database;
};

struct StoreTransaction {
    StoreEnvironment* environment{};
    bool writable{};
    std::unique_ptr<MiareReadTransaction> read;
    std::unique_ptr<MiareWriteTransaction> write;
    std::deque<std::vector<std::byte>> returnedValues;
};

struct StoreCursor {
    std::vector<std::pair<std::vector<std::byte>, std::vector<std::byte>>> rows;
    std::size_t position{};
    bool positioned{};
};

inline auto
miareErrorCode(const miare::DatabaseError& error) -> int
{
    switch (error.code()) {
    case miare::Errc::InUse:
        return EBUSY;
    case miare::Errc::UnsupportedFormat:
        return storeInvalid;
    case miare::Errc::Corrupt:
        return storeCorrupt;
    case miare::Errc::UnsupportedFeature:
    case miare::Errc::IncompatibleProfile:
        return storeVersionMismatch;
    case miare::Errc::KeyRequired:
    case miare::Errc::UnexpectedKey:
        return storeIncompatible;
    case miare::Errc::ResourceLimit:
        return storeCapacityLimit;
    case miare::Errc::Io:
    case miare::Errc::Durability:
    case miare::Errc::ProviderUnavailable:
    case miare::Errc::RecoveryRequired:
    case miare::Errc::CommitFailed:
    case miare::Errc::CommitOutcomeUnknown:
        return error.nativeCode()
            .value_or(std::make_error_code(std::errc::io_error))
            .value();
    default:
        return storePanic;
    }
}

inline auto
store_strerror(int error) -> const char*
{
    switch (error) {
    case storeSuccess:
        return "success";
    case storeNotFound:
        return "key or logical database not found";
    case storeKeyExists:
        return "key already exists";
    case storeInvalid:
        return "invalid Miare database";
    case storeCorrupt:
        return "corrupt Miare database";
    case storeVersionMismatch:
        return "unsupported Miare format or profile";
    case storeIncompatible:
        return "incompatible Miare database";
    default:
        return std::strerror(error > 0 ? error : EIO);
    }
}

inline auto
logicalDatabaseId(std::string_view name) -> std::optional<LogicalDatabase>
{
    constexpr std::array<std::string_view, 11> names{"metadata",
                                                     "blocks",
                                                     "blocks_by_type",
                                                     "containment_by_parent",
                                                     "containment_by_child",
                                                     "references_by_source",
                                                     "references_by_target",
                                                     "properties_by_block",
                                                     "pages_by_title",
                                                     "journal_by_date",
                                                     "settings"};
    for (std::size_t index = 0; index < std::size(names); ++index) {
        if (names[index] == name) {
            return static_cast<LogicalDatabase>(index + 1U);
        }
    }
    return std::nullopt;
}

inline auto
byteView(const StoreValue& value) -> miare::ByteView
{
    static constexpr std::byte emptyValue{};
    if (value.size == 0) {
        return {&emptyValue, 0};
    }
    return {static_cast<const std::byte*>(value.data), value.size};
}

inline auto
logicalPrefix(LogicalDatabase database) -> std::vector<std::byte>
{
    return {std::byte{0x48}, static_cast<std::byte>(database)};
}

inline auto
logicalKey(LogicalDatabase database, const StoreValue& key)
    -> std::vector<std::byte>
{
    auto result = logicalPrefix(database);
    const auto raw = byteView(key);
    result.insert(result.end(), raw.begin(), raw.end());
    return result;
}

inline auto
schemaKey(LogicalDatabase database) -> std::vector<std::byte>
{
    return {std::byte{0x48}, std::byte{0}, static_cast<std::byte>(database)};
}

inline auto
transactionGet(StoreTransaction* transaction, miare::ByteView key)
    -> std::optional<MiareDatabase::OwnedBytes>
{
    return transaction->writable ? transaction->write->get(key)
                                 : transaction->read->get(key);
}

inline auto
transactionContains(StoreTransaction* transaction, miare::ByteView key) -> bool
{
    return transaction->writable ? transaction->write->contains(key)
                                 : transaction->read->contains(key);
}

// NOLINTBEGIN(clang-analyzer-cplusplus.NewDeleteLeaks)
inline auto
store_env_create(StoreEnvironment** output) -> int
{
    *output = new StoreEnvironment{};
    return storeSuccess;
}
// NOLINTEND(clang-analyzer-cplusplus.NewDeleteLeaks)

inline auto
store_env_open(StoreEnvironment* environment, const std::filesystem::path& path)
    -> int
{
    try {
        environment->path = path;
        std::error_code error;
        const auto exists = std::filesystem::exists(environment->path, error);
        const auto empty =
            exists && std::filesystem::file_size(environment->path, error) == 0;
        if (empty) {
            std::filesystem::remove(environment->path, error);
            if (error) {
                return error.value();
            }
        }
        auto providers = miare::ProviderSet::systemCompression();
        if (!exists || empty) {
            miare::UnencryptedCreateOptions options;
            options.compression = miare::Compression::ZStd;
            environment->database = std::make_unique<MiareDatabase>(
                MiareDatabase::createUnencrypted(environment->path, options,
                                                 std::move(providers)));
        } else {
            environment->database =
                std::make_unique<MiareDatabase>(MiareDatabase::openUnencrypted(
                    environment->path, std::move(providers)));
        }
        return storeSuccess;
    } catch (const miare::DatabaseError& error) {
        if (error.code() == miare::Errc::Io && !error.nativeCode()) {
            return storeInvalid;
        }
        return miareErrorCode(error);
    } catch (const miare::ContractError&) {
        return storePanic;
    } catch (const std::filesystem::filesystem_error& error) {
        return error.code().value();
    } catch (...) {
        return EIO;
    }
}

inline auto
store_env_try_close(StoreEnvironment* environment) -> int
{
    if (environment == nullptr) {
        return storeSuccess;
    }
    try {
        if (environment->database) {
            environment->database->close();
            environment->database.reset();
        }
        return storeSuccess;
    } catch (const miare::DatabaseError& error) {
        return miareErrorCode(error);
    } catch (...) {
        return EIO;
    }
}

inline void
store_env_close(StoreEnvironment* environment)
{
    if (environment == nullptr) {
        return;
    }
    (void)store_env_try_close(environment);
    delete environment;
}

inline auto
store_txn_begin(StoreEnvironment* environment,
                [[maybe_unused]] StoreTransaction* parent, unsigned flags,
                StoreTransaction** output) -> int
{
    try {
        auto transaction = std::make_unique<StoreTransaction>();
        transaction->environment = environment;
        transaction->writable = (flags & storeReadOnly) == 0;
        if (transaction->writable) {
            transaction->write = std::make_unique<MiareWriteTransaction>(
                environment->database->beginWrite());
        } else {
            transaction->read = std::make_unique<MiareReadTransaction>(
                environment->database->beginRead());
        }
        *output = transaction.release();
        return storeSuccess;
    } catch (const miare::DatabaseError& error) {
        return miareErrorCode(error);
    } catch (...) {
        return storeBadTransaction;
    }
}

inline void
store_txn_abort(StoreTransaction* transaction)
{
    if (transaction == nullptr) {
        return;
    }
    if (transaction->write) {
        transaction->write->rollback();
    }
    if (transaction->read) {
        transaction->read->end();
    }
    delete transaction;
}

inline auto
store_txn_commit(StoreTransaction* transaction) -> int
{
    try {
        transaction->write->commit();
        delete transaction;
        return storeSuccess;
    } catch (const miare::DatabaseError& error) {
        const auto code = miareErrorCode(error);
        delete transaction;
        return code;
    } catch (...) {
        delete transaction;
        return storeBadTransaction;
    }
}

inline auto
store_dbi_open(StoreTransaction* transaction, const char* name, unsigned flags,
               LogicalDatabase* output) -> int
{
    const auto database = logicalDatabaseId(name);
    if (!database) {
        return storeBadDatabase;
    }
    const auto marker = schemaKey(*database);
    try {
        if (!transactionContains(transaction, marker)) {
            if ((flags & storeCreate) == 0 || !transaction->writable) {
                return storeNotFound;
            }
            constexpr std::byte present{1};
            transaction->write->put(marker, {&present, 1});
        }
        *output = *database;
        return storeSuccess;
    } catch (const miare::DatabaseError& error) {
        return miareErrorCode(error);
    } catch (...) {
        return storeBadDatabase;
    }
}

// The adapter mirrors the established transaction call shape used throughout
// the Notebook implementation; the key/value and database/drop pairs are
// intentionally positional here.
// NOLINTBEGIN(bugprone-easily-swappable-parameters)
inline auto
store_get(StoreTransaction* transaction, LogicalDatabase database,
          StoreValue* key, StoreValue* value) -> int
{
    try {
        const auto namespaced = logicalKey(database, *key);
        auto found = transactionGet(transaction, namespaced);
        if (!found) {
            return storeNotFound;
        }
        transaction->returnedValues.emplace_back(found->begin(), found->end());
        auto& returned = transaction->returnedValues.back();
        value->size = returned.size();
        value->data = returned.data();
        return storeSuccess;
    } catch (const miare::DatabaseError& error) {
        return miareErrorCode(error);
    } catch (...) {
        return storeBadTransaction;
    }
}

inline auto
store_put(StoreTransaction* transaction, LogicalDatabase database,
          StoreValue* key, StoreValue* value, unsigned flags) -> int
{
    try {
        const auto namespaced = logicalKey(database, *key);
        if ((flags & storeNoOverwrite) != 0 &&
            transaction->write->contains(namespaced)) {
            return storeKeyExists;
        }
        transaction->write->put(namespaced, byteView(*value));
        return storeSuccess;
    } catch (const miare::DatabaseError& error) {
        return miareErrorCode(error);
    } catch (const miare::ContractError&) {
        return storeBadValue;
    } catch (...) {
        return storeBadTransaction;
    }
}

inline auto
store_del(StoreTransaction* transaction, LogicalDatabase database,
          StoreValue* key, [[maybe_unused]] StoreValue* value) -> int
{
    try {
        return transaction->write->erase(logicalKey(database, *key))
                   ? storeSuccess
                   : storeNotFound;
    } catch (const miare::DatabaseError& error) {
        return miareErrorCode(error);
    } catch (...) {
        return storeBadTransaction;
    }
}

inline auto
store_cursor_open(StoreTransaction* transaction, LogicalDatabase database,
                  StoreCursor** output) -> int
{
    try {
        auto cursor = std::make_unique<StoreCursor>();
        const auto prefix = logicalPrefix(database);
        if (transaction->writable) {
            auto scan =
                transaction->write->scan(miare::KeyRangeView::prefix(prefix));
            for (auto found = scan.first(); found; found = scan.next()) {
                const auto key = scan.key();
                cursor->rows.emplace_back(
                    std::vector<std::byte>{
                        key.begin() +
                            static_cast<std::ptrdiff_t>(prefix.size()),
                        key.end()},
                    std::vector<std::byte>{scan.value().begin(),
                                           scan.value().end()});
            }
        } else {
            auto scan =
                transaction->read->scan(miare::KeyRangeView::prefix(prefix));
            for (auto found = scan.first(); found; found = scan.next()) {
                const auto key = scan.key();
                cursor->rows.emplace_back(
                    std::vector<std::byte>{
                        key.begin() +
                            static_cast<std::ptrdiff_t>(prefix.size()),
                        key.end()},
                    std::vector<std::byte>{scan.value().begin(),
                                           scan.value().end()});
            }
        }
        *output = cursor.release();
        return storeSuccess;
    } catch (const miare::DatabaseError& error) {
        return miareErrorCode(error);
    } catch (...) {
        return storeCursorLimit;
    }
}

inline auto
store_cursor_get(StoreCursor* cursor, StoreValue* key, StoreValue* value,
                 unsigned operation) -> int
{
    if (operation == storeFirst) {
        cursor->position = 0;
        cursor->positioned = true;
    } else if (operation == storeNext) {
        if (!cursor->positioned) {
            cursor->position = 0;
            cursor->positioned = true;
        } else {
            ++cursor->position;
        }
    } else if (operation == storeSetRange) {
        const auto sought = byteView(*key);
        cursor->position = static_cast<std::size_t>(std::distance(
            cursor->rows.begin(),
            std::ranges::lower_bound(
                cursor->rows, sought,
                [](const auto& left, const auto& right) -> bool {
                    return std::ranges::lexicographical_compare(left, right);
                },
                &std::pair<std::vector<std::byte>,
                           std::vector<std::byte>>::first)));
        cursor->positioned = true;
    } else {
        return storeBadValue;
    }
    if (cursor->position >= cursor->rows.size()) {
        return storeNotFound;
    }
    auto& row = cursor->rows[cursor->position];
    key->size = row.first.size();
    key->data = row.first.data();
    value->size = row.second.size();
    value->data = row.second.data();
    return storeSuccess;
}

inline void
store_cursor_close(StoreCursor* cursor)
{
    delete cursor;
}

inline auto
store_drop(StoreTransaction* transaction, LogicalDatabase database,
           int removeDatabase) -> int
{
    try {
        const auto prefix = logicalPrefix(database);
        std::vector<std::vector<std::byte>> keys;
        {
            auto cursor =
                transaction->write->scan(miare::KeyRangeView::prefix(prefix));
            for (auto found = cursor.first(); found; found = cursor.next()) {
                keys.emplace_back(cursor.key().begin(), cursor.key().end());
            }
        }
        for (const auto& key : keys) {
            (void)transaction->write->erase(key);
        }
        if (removeDatabase != 0) {
            (void)transaction->write->erase(schemaKey(database));
        }
        return storeSuccess;
    } catch (const miare::DatabaseError& error) {
        return miareErrorCode(error);
    } catch (...) {
        return storeBadTransaction;
    }
}
// NOLINTEND(bugprone-easily-swappable-parameters)

} // namespace hieda::notebook
