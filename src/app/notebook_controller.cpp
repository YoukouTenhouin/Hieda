// SPDX-License-Identifier: MPL-2.0
#include "notebook_controller.hpp"

#include <QCoreApplication>
#include <QDateTime>
#include <QEvent>
#include <QFile>
#include <QFileInfo>

#include <algorithm>
#include <limits>
#include <utility>

namespace {

auto localPath(const QUrl& url) -> std::filesystem::path {
#ifdef _WIN32
    return std::filesystem::path(url.toLocalFile().toStdWString());
#else
    const auto encoded = QFile::encodeName(url.toLocalFile());
    return {encoded.constData()};
#endif
}

auto displayPath(const std::filesystem::path& path) -> QString {
#ifdef _WIN32
    return QString::fromStdWString(path.native());
#else
    return QFile::decodeName(path.c_str());
#endif
}

auto domainJournalDate(const QDate& date) -> hieda::notebook::JournalDate {
    return {date.year(), static_cast<std::uint8_t>(date.month()),
            static_cast<std::uint8_t>(date.day())};
}

auto blockId(const QString& text) -> std::optional<hieda::notebook::BlockId> {
    const auto compact = QString(text).remove(QLatin1Char('-'));
    if (compact.size() != 32) {
        return std::nullopt;
    }
    hieda::notebook::BlockId id;
    for (qsizetype index = 0; index < compact.size(); index += 2) {
        bool valid = false;
        const auto byte = compact.mid(index, 2).toUInt(&valid, 16);
        if (!valid) {
            return std::nullopt;
        }
        id.bytes[static_cast<std::size_t>(index / 2)] = static_cast<std::byte>(byte);
    }
    return id;
}

auto displayId(const hieda::notebook::BlockId& blockIdentifier) -> QString {
    return QString::fromStdString(blockIdentifier.toString());
}

auto journalOutcome(bool succeeded, int row = -1, int cursorPosition = 0) -> QVariantMap {
    return {{QStringLiteral("succeeded"), succeeded},
            {QStringLiteral("row"), row},
            {QStringLiteral("cursorPosition"), cursorPosition}};
}

} // namespace

JournalEntryModel::JournalEntryModel(QObject* parent) : QAbstractListModel(parent) {}

auto JournalEntryModel::rowCount(const QModelIndex& parent) const -> int {
    return parent.isValid() ? 0 : static_cast<int>(entries_.size());
}

auto JournalEntryModel::data(const QModelIndex& index, int role) const -> QVariant {
    if (!index.isValid() || index.row() < 0 ||
        static_cast<std::size_t>(index.row()) >= entries_.size()) {
        return {};
    }
    const auto& entry = entries_[static_cast<std::size_t>(index.row())];
    if (role == EntryIdRole) {
        return displayId(entry.metadata.id);
    }
    if (role == AuthoredTextRole) {
        return QString::fromUtf8(entry.authoredText.data(),
                                 static_cast<qsizetype>(entry.authoredText.size()));
    }
    if (role == ParentEntryIdRole) {
        return entry.parentEntry ? displayId(*entry.parentEntry) : QString{};
    }
    const auto parent = entry.parentEntry;
    const auto hasChildren = std::ranges::any_of(entries_, [&](const auto& candidate) -> bool {
        return candidate.parentEntry == entry.metadata.id;
    });
    auto depth = 0;
    auto ancestor = parent;
    while (ancestor) {
        ++depth;
        const auto found = std::ranges::find_if(entries_, [&](const auto& candidate) -> bool {
            return candidate.metadata.id == *ancestor;
        });
        ancestor = found == entries_.end() ? std::nullopt : found->parentEntry;
    }
    bool hasPreviousSibling = false;
    bool hasNextSibling = false;
    for (std::size_t row = 0; row < entries_.size(); ++row) {
        if (entries_[row].parentEntry != parent || entries_[row].metadata.id == entry.metadata.id) {
            continue;
        }
        if (std::cmp_less(row, index.row())) {
            hasPreviousSibling = true;
        } else {
            hasNextSibling = true;
        }
    }
    if (role == DepthRole) {
        return depth;
    }
    if (role == HasChildrenRole) {
        return hasChildren;
    }
    if (role == CanIndentRole || role == CanMoveUpRole) {
        return hasPreviousSibling;
    }
    if (role == CanOutdentRole) {
        return parent.has_value();
    }
    if (role == CanMoveDownRole) {
        return hasNextSibling;
    }
    if (role == CanDeleteRole) {
        return !hasChildren;
    }
    return {};
}

auto JournalEntryModel::roleNames() const -> QHash<int, QByteArray> {
    return {{EntryIdRole, "entryId"},
            {AuthoredTextRole, "authoredText"},
            {ParentEntryIdRole, "parentEntryId"},
            {DepthRole, "depth"},
            {HasChildrenRole, "hasChildren"},
            {CanIndentRole, "canIndent"},
            {CanOutdentRole, "canOutdent"},
            {CanMoveUpRole, "canMoveUp"},
            {CanMoveDownRole, "canMoveDown"},
            {CanDeleteRole, "canDelete"}};
}

void JournalEntryModel::setEntries(std::vector<hieda::notebook::JournalEntry> entries) {
    beginResetModel();
    entries_ = std::move(entries);
    endResetModel();
}

void JournalEntryModel::insertEntry(int row, hieda::notebook::JournalEntry entry) {
    if (row < 0 || row > rowCount()) {
        return;
    }
    beginInsertRows({}, row, row);
    entries_.insert(entries_.begin() + row, std::move(entry));
    endInsertRows();
    if (!entries_.empty()) {
        emit dataChanged(index(0), index(rowCount() - 1),
                         {DepthRole, HasChildrenRole, CanIndentRole, CanOutdentRole, CanMoveUpRole,
                          CanMoveDownRole, CanDeleteRole});
    }
}

void JournalEntryModel::updateEntry(const hieda::notebook::JournalEntry& entry) {
    const auto found = std::ranges::find_if(entries_, [&](const auto& current) -> bool {
        return current.metadata.id == entry.metadata.id;
    });
    if (found == entries_.end()) {
        return;
    }
    *found = entry;
    const auto row = static_cast<int>(std::distance(entries_.begin(), found));
    const auto changed = index(row);
    emit dataChanged(changed, changed, {AuthoredTextRole});
}

auto JournalEntryModel::entryId(int row) const -> QString {
    if (row < 0 || static_cast<std::size_t>(row) >= entries_.size()) {
        return {};
    }
    return displayId(entries_[static_cast<std::size_t>(row)].metadata.id);
}

auto JournalEntryModel::entryText(int row) const -> QString {
    if (row < 0 || static_cast<std::size_t>(row) >= entries_.size()) {
        return {};
    }
    const auto& text = entries_[static_cast<std::size_t>(row)].authoredText;
    return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));
}

auto JournalEntryModel::rowForId(const hieda::notebook::BlockId& identifier) const -> int {
    const auto found = std::ranges::find_if(
        entries_, [&](const auto& entry) -> bool { return entry.metadata.id == identifier; });
    return found == entries_.end() ? -1 : static_cast<int>(std::distance(entries_.begin(), found));
}

NotebookController::NotebookController(QObject* parent)
    : QObject(parent), journalEntries_(this), midnightTimer_(this) {
    midnightTimer_.setSingleShot(true);
    connect(&midnightTimer_, &QTimer::timeout, this, [this]() -> void {
        if (hasOpenNotebook()) {
            requestJournalDateRollover(QDate::currentDate());
        }
        scheduleMidnightRefresh();
    });
    if (QCoreApplication::instance() != nullptr) {
        QCoreApplication::instance()->installEventFilter(this);
    }
    scheduleMidnightRefresh();
}

auto NotebookController::hasOpenNotebook() const -> bool {
    return session_.isOpen();
}
auto NotebookController::notebookPath() const -> QString {
    return path_;
}
auto NotebookController::notebookName() const -> QString {
    return name_;
}
auto NotebookController::errorMessage() const -> QString {
    return error_;
}
auto NotebookController::journalDate() const -> QDate {
    return journalDate_;
}
auto NotebookController::journalEntries() -> QAbstractItemModel* {
    return &journalEntries_;
}

void NotebookController::createNotebook(const QUrl& url) {
    const auto path = localPath(url);
    try {
        const auto result = session_.create(path);
        if (result) {
            accept(result.value());
        } else {
            reject(result.error());
        }
    } catch (const hieda::notebook::NotebookException&) {
        error_ = tr("Hieda encountered an unexpected Notebook error.");
        emit stateChanged();
    }
}

void NotebookController::openNotebook(const QUrl& url) {
    const auto path = localPath(url);
    try {
        const auto result = session_.open(path);
        if (result) {
            accept(result.value());
        } else {
            reject(result.error());
        }
    } catch (const hieda::notebook::NotebookException&) {
        error_ = tr("Hieda encountered an unexpected Notebook error.");
        emit stateChanged();
    }
}

void NotebookController::closeNotebook() {
    session_.close();
    path_.clear();
    name_.clear();
    error_.clear();
    journalEntries_.setEntries({});
    emit stateChanged();
    emit journalChanged();
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
auto NotebookController::insertJournalEntry(const QString& authoredText,
                                            const QString& afterEntryId) -> int {
    std::optional<hieda::notebook::BlockId> after;
    if (!afterEntryId.isEmpty()) {
        after = blockId(afterEntryId);
        if (!after) {
            error_ = tr("The selected Journal Entry is no longer available.");
            emit stateChanged();
            return -1;
        }
    }
    const auto utf8 = authoredText.toUtf8();
    try {
        std::vector<QString> existingIds;
        existingIds.reserve(static_cast<std::size_t>(journalEntries_.rowCount()));
        for (int row = 0; row < journalEntries_.rowCount(); ++row) {
            existingIds.push_back(journalEntries_.entryId(row));
        }
        if (after) {
            if (std::ranges::find(existingIds, afterEntryId) == existingIds.end()) {
                error_ = tr("The selected Journal Entry is no longer on this Page.");
                emit stateChanged();
                return -1;
            }
        }
        const auto result = session_.insertJournalEntry(
            domainJournalDate(journalDate_), after,
            std::string(utf8.constData(), static_cast<std::size_t>(utf8.size())));
        if (!result) {
            rejectSave(result.error());
            return -1;
        }
        const auto& entries = result.value().entries;
        const auto inserted = std::ranges::find_if(entries, [&](const auto& entry) -> bool {
            return std::ranges::find(existingIds, displayId(entry.metadata.id)) ==
                   existingIds.end();
        });
        if (inserted == entries.end()) {
            error_ = tr("Hieda encountered an unexpected Notebook error.");
            emit stateChanged();
            loadJournalDate(journalDate_);
            return -1;
        }
        const auto insertedRow = static_cast<int>(std::distance(entries.begin(), inserted));
        journalEntries_.insertEntry(insertedRow, *inserted);
        error_.clear();
        emit stateChanged();
        emit journalChanged();
        return insertedRow;
    } catch (const hieda::notebook::NotebookException&) {
        error_ = tr("Hieda encountered an unexpected Notebook error.");
        emit stateChanged();
    }
    return -1;
}

auto NotebookController::journalEntryId(int row) const -> QString {
    return journalEntries_.entryId(row);
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
auto NotebookController::updateJournalEntry(const QString& entryId, const QString& authoredText)
    -> bool {
    const auto id = blockId(entryId);
    if (!id) {
        error_ = tr("That Journal Entry is no longer available.");
        emit stateChanged();
        return false;
    }
    const auto utf8 = authoredText.toUtf8();
    try {
        const auto result = session_.updateJournalEntry(
            *id, std::string(utf8.constData(), static_cast<std::size_t>(utf8.size())));
        if (!result) {
            rejectSave(result.error());
            loadJournalDate(journalDate_);
            return false;
        }
        journalEntries_.updateEntry(result.value());
        error_.clear();
        emit stateChanged();
        return true;
    } catch (const hieda::notebook::NotebookException&) {
        error_ = tr("Hieda encountered an unexpected Notebook error.");
        emit stateChanged();
        return false;
    }
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
auto NotebookController::splitJournalEntry(const QString& entryId, const QString& authoredText,
                                           int cursorPosition) -> QVariantMap {
    const auto id = blockId(entryId);
    if (!id) {
        error_ = tr("That Journal Entry is no longer available.");
        emit stateChanged();
        return journalOutcome(false);
    }
    const auto row = journalEntries_.rowForId(*id);
    if (row < 0 || cursorPosition < 0 || cursorPosition > authoredText.size()) {
        error_ = tr("The split cursor is no longer valid.");
        emit stateChanged();
        return journalOutcome(false);
    }
    std::vector<QString> existingIds;
    existingIds.reserve(static_cast<std::size_t>(journalEntries_.rowCount()));
    for (int current = 0; current < journalEntries_.rowCount(); ++current) {
        existingIds.push_back(journalEntries_.entryId(current));
    }
    const auto prefix = authoredText.left(cursorPosition).toUtf8();
    const auto utf8 = authoredText.toUtf8();
    try {
        const auto result = session_.splitJournalEntry(
            *id, std::string(utf8.constData(), static_cast<std::size_t>(utf8.size())),
            static_cast<std::size_t>(prefix.size()));
        if (!result) {
            rejectSave(result.error());
            return journalOutcome(false);
        }
        journalEntries_.setEntries(result.value().entries);
        auto insertedRow = -1;
        for (int current = 0; current < journalEntries_.rowCount(); ++current) {
            const auto candidate = journalEntries_.entryId(current);
            if (std::ranges::find(existingIds, candidate) == existingIds.end()) {
                insertedRow = current;
                break;
            }
        }
        error_.clear();
        emit stateChanged();
        emit journalChanged();
        return journalOutcome(insertedRow >= 0, insertedRow, 0);
    } catch (const hieda::notebook::NotebookException&) {
        error_ = tr("Hieda encountered an unexpected Notebook error.");
        emit stateChanged();
        return journalOutcome(false);
    }
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
auto NotebookController::joinJournalEntry(const QString& entryId, const QString& authoredText)
    -> QVariantMap {
    const auto id = blockId(entryId);
    if (!id) {
        error_ = tr("That Journal Entry is no longer available.");
        emit stateChanged();
        return journalOutcome(false);
    }
    const auto row = journalEntries_.rowForId(*id);
    if (row <= 0) {
        error_ = tr("That Journal Entry cannot be joined.");
        emit stateChanged();
        return journalOutcome(false);
    }
    const auto targetIdText = journalEntries_.entryId(row - 1);
    const auto targetId = blockId(targetIdText);
    const auto cursor = static_cast<int>(std::min<qsizetype>(
        journalEntries_.entryText(row - 1).size(), std::numeric_limits<int>::max()));
    const auto utf8 = authoredText.toUtf8();
    try {
        const auto result = session_.joinJournalEntry(
            *id, std::string(utf8.constData(), static_cast<std::size_t>(utf8.size())));
        if (!result) {
            rejectSave(result.error());
            return journalOutcome(false);
        }
        journalEntries_.setEntries(result.value().entries);
        const auto targetRow = targetId ? journalEntries_.rowForId(*targetId) : -1;
        error_.clear();
        emit stateChanged();
        emit journalChanged();
        return journalOutcome(targetRow >= 0, targetRow, cursor);
    } catch (const hieda::notebook::NotebookException&) {
        error_ = tr("Hieda encountered an unexpected Notebook error.");
        emit stateChanged();
        return journalOutcome(false);
    }
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
auto NotebookController::moveJournalEntry(const QString& entryId, const QString& authoredText,
                                          hieda::notebook::JournalEntryMove movement,
                                          int cursorPosition) -> QVariantMap {
    const auto id = blockId(entryId);
    if (!id) {
        error_ = tr("That Journal Entry is no longer available.");
        emit stateChanged();
        return journalOutcome(false);
    }
    const auto utf8 = authoredText.toUtf8();
    try {
        const auto result = session_.moveJournalEntry(
            *id, movement, std::string(utf8.constData(), static_cast<std::size_t>(utf8.size())));
        if (!result) {
            rejectSave(result.error());
            return journalOutcome(false);
        }
        journalEntries_.setEntries(result.value().entries);
        const auto row = journalEntries_.rowForId(*id);
        error_.clear();
        emit stateChanged();
        emit journalChanged();
        return journalOutcome(row >= 0, row, cursorPosition);
    } catch (const hieda::notebook::NotebookException&) {
        error_ = tr("Hieda encountered an unexpected Notebook error.");
        emit stateChanged();
        return journalOutcome(false);
    }
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
auto NotebookController::indentJournalEntry(const QString& entryId, const QString& authoredText,
                                            int cursorPosition) -> QVariantMap {
    return moveJournalEntry(entryId, authoredText, hieda::notebook::JournalEntryMove::indent,
                            cursorPosition);
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
auto NotebookController::outdentJournalEntry(const QString& entryId, const QString& authoredText,
                                             int cursorPosition) -> QVariantMap {
    return moveJournalEntry(entryId, authoredText, hieda::notebook::JournalEntryMove::outdent,
                            cursorPosition);
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
auto NotebookController::moveJournalEntryUp(const QString& entryId, const QString& authoredText,
                                            int cursorPosition) -> QVariantMap {
    return moveJournalEntry(entryId, authoredText, hieda::notebook::JournalEntryMove::up,
                            cursorPosition);
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
auto NotebookController::moveJournalEntryDown(const QString& entryId, const QString& authoredText,
                                              int cursorPosition) -> QVariantMap {
    return moveJournalEntry(entryId, authoredText, hieda::notebook::JournalEntryMove::down,
                            cursorPosition);
}

auto NotebookController::deleteJournalEntry(const QString& entryId) -> QVariantMap {
    const auto id = blockId(entryId);
    if (!id) {
        error_ = tr("That Journal Entry is no longer available.");
        emit stateChanged();
        return journalOutcome(false);
    }
    const auto oldRow = journalEntries_.rowForId(*id);
    try {
        const auto result = session_.deleteJournalEntry(*id);
        if (!result) {
            rejectSave(result.error());
            return journalOutcome(false);
        }
        journalEntries_.setEntries(result.value().entries);
        const auto focusRow = std::min(oldRow, journalEntries_.rowCount() - 1);
        const auto cursor =
            focusRow >= 0
                ? static_cast<int>(std::min<qsizetype>(journalEntries_.entryText(focusRow).size(),
                                                       std::numeric_limits<int>::max()))
                : 0;
        error_.clear();
        emit stateChanged();
        emit journalChanged();
        return journalOutcome(true, focusRow, cursor);
    } catch (const hieda::notebook::NotebookException&) {
        error_ = tr("Hieda encountered an unexpected Notebook error.");
        emit stateChanged();
        return journalOutcome(false);
    }
}

void NotebookController::requestJournalDateRollover(const QDate& date) {
    if (hasOpenNotebook() && date.isValid()) {
        pendingJournalDate_ = date;
        emit journalDateRolloverRequested();
    }
}

void NotebookController::completeJournalDateRollover() {
    if (hasOpenNotebook() && pendingJournalDate_.isValid()) {
        loadJournalDate(pendingJournalDate_);
        pendingJournalDate_ = {};
    }
}

auto NotebookController::eventFilter(QObject* watched, QEvent* event) -> bool {
    if (watched == QCoreApplication::instance() &&
        event->type() == QEvent::ApplicationStateChange) {
        const auto currentDate = QDate::currentDate();
        if (hasOpenNotebook() && currentDate != journalDate_) {
            requestJournalDateRollover(currentDate);
        }
        scheduleMidnightRefresh();
    }
    return QObject::eventFilter(watched, event);
}

void NotebookController::clearError() {
    if (!error_.isEmpty()) {
        error_.clear();
        emit stateChanged();
    }
}

void NotebookController::accept(const hieda::notebook::NotebookInfo& info) {
    path_ = displayPath(info.path);
    name_ = QFileInfo(path_).completeBaseName();
    error_.clear();
    loadJournalDate(QDate::currentDate());
    emit stateChanged();
}

void NotebookController::reject(const hieda::notebook::NotebookError& error) {
    using hieda::notebook::NotebookErrorCode;
    switch (error.code) {
    case NotebookErrorCode::pathNotFound:
        error_ = tr("That Notebook could not be found.");
        break;
    case NotebookErrorCode::pathExists:
        error_ = tr("A file already exists at that location.");
        break;
    case NotebookErrorCode::invalidPath:
        error_ = tr("Choose a valid Notebook file path.");
        break;
    case NotebookErrorCode::invalidNotebook:
        error_ = tr("That file is not a valid Hieda Notebook.");
        break;
    case NotebookErrorCode::unsupportedVersion:
        error_ = tr("That Notebook was created by an unsupported Hieda version.");
        break;
    case NotebookErrorCode::alreadyOpen:
        error_ = tr("Close the current Notebook before opening another.");
        break;
    case NotebookErrorCode::alreadyInUse:
        error_ = tr("That Notebook is already open in another Hieda process.");
        break;
    case NotebookErrorCode::permissionDenied:
        error_ = tr("Hieda does not have permission to use that location.");
        break;
    case NotebookErrorCode::ioFailure:
        error_ = tr("Hieda could not safely open that Notebook.");
        break;
    case NotebookErrorCode::notebookNotOpen:
        error_ = tr("Open a Notebook before editing the Journal.");
        break;
    case NotebookErrorCode::invalidJournalDate:
        error_ = tr("Choose a valid Journal date.");
        break;
    case NotebookErrorCode::invalidAuthoredText:
        error_ = tr("A Journal Entry must contain one line of Unicode text.");
        break;
    case NotebookErrorCode::blockNotFound:
        error_ = tr("That Journal Entry is no longer available.");
        break;
    case NotebookErrorCode::invalidInsertionPoint:
        error_ = tr("The selected Journal Entry is no longer on this Page.");
        break;
    case NotebookErrorCode::invalidCursorPosition:
        error_ = tr("The split cursor is no longer valid.");
        break;
    case NotebookErrorCode::invalidStructuralMove:
        error_ = tr("That Journal Entry cannot move in that direction.");
        break;
    case NotebookErrorCode::blockHasChildren:
        error_ = tr("Move or delete an Entry's children first.");
        break;
    }
    emit stateChanged();
}

void NotebookController::rejectSave(const hieda::notebook::NotebookError& error) {
    if (error.code == hieda::notebook::NotebookErrorCode::ioFailure) {
        error_ = tr("Hieda could not safely save that Journal change.");
        emit stateChanged();
        return;
    }
    reject(error);
}

void NotebookController::loadJournalDate(const QDate& date) {
    try {
        const auto result = session_.journalPage(domainJournalDate(date));
        if (!result) {
            reject(result.error());
            return;
        }
        journalDate_ = date;
        journalEntries_.setEntries(result.value().entries);
        emit journalChanged();
    } catch (const hieda::notebook::NotebookException&) {
        error_ = tr("Hieda encountered an unexpected Notebook error.");
        emit stateChanged();
    }
}

void NotebookController::scheduleMidnightRefresh() {
    const auto now = QDateTime::currentDateTime();
    const auto nextMidnight = QDateTime(now.date().addDays(1).startOfDay());
    const auto milliseconds = std::max<qint64>(1, now.msecsTo(nextMidnight));
    midnightTimer_.start(
        static_cast<int>(std::min<qint64>(milliseconds, std::numeric_limits<int>::max())));
}
