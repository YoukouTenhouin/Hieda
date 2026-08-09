// SPDX-License-Identifier: MPL-2.0
#include "notebook_controller.hpp"

#include <QClipboard>
#include <QCoreApplication>
#include <QDateTime>
#include <QEvent>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>

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

auto outlineOutcome(bool succeeded, int row = -1, int cursorPosition = 0) -> QVariantMap {
    return {{QStringLiteral("succeeded"), succeeded},
            {QStringLiteral("row"), row},
            {QStringLiteral("cursorPosition"), cursorPosition}};
}

template <typename DomainEntry>
auto asOutlineEntries(const std::vector<DomainEntry>& domainEntries) -> std::vector<OutlineEntry> {
    std::vector<OutlineEntry> entries;
    entries.reserve(domainEntries.size());
    for (const auto& entry : domainEntries) {
        entries.push_back({entry.metadata, entry.authoredText, entry.parentEntry});
    }
    return entries;
}

} // namespace

OutlineEntryModel::OutlineEntryModel(QObject* parent) : QAbstractListModel(parent) {}

auto OutlineEntryModel::rowCount(const QModelIndex& parent) const -> int {
    return parent.isValid() ? 0 : static_cast<int>(entries_.size());
}

auto OutlineEntryModel::data(const QModelIndex& index, int role) const -> QVariant {
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
    const auto depth = entryDepth(index.row());
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

auto OutlineEntryModel::roleNames() const -> QHash<int, QByteArray> {
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

void OutlineEntryModel::setEntries(std::vector<OutlineEntry> entries) {
    beginResetModel();
    entries_ = std::move(entries);
    endResetModel();
}

void OutlineEntryModel::insertEntry(int row, OutlineEntry entry) {
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

void OutlineEntryModel::updateEntry(const OutlineEntry& entry) {
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

auto OutlineEntryModel::entryId(int row) const -> QString {
    if (row < 0 || static_cast<std::size_t>(row) >= entries_.size()) {
        return {};
    }
    return displayId(entries_[static_cast<std::size_t>(row)].metadata.id);
}

auto OutlineEntryModel::entryText(int row) const -> QString {
    if (row < 0 || static_cast<std::size_t>(row) >= entries_.size()) {
        return {};
    }
    const auto& text = entries_[static_cast<std::size_t>(row)].authoredText;
    return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));
}

auto OutlineEntryModel::entryParentId(int row) const -> QString {
    if (row < 0 || static_cast<std::size_t>(row) >= entries_.size()) {
        return {};
    }
    const auto parent = entries_[static_cast<std::size_t>(row)].parentEntry;
    return parent ? displayId(*parent) : QString{};
}

auto OutlineEntryModel::entryDepth(int row) const -> int {
    if (row < 0 || static_cast<std::size_t>(row) >= entries_.size()) {
        return 0;
    }
    auto depth = 0;
    auto ancestor = entries_[static_cast<std::size_t>(row)].parentEntry;
    while (ancestor) {
        ++depth;
        const auto found = std::ranges::find_if(entries_, [&](const auto& candidate) -> bool {
            return candidate.metadata.id == *ancestor;
        });
        ancestor = found == entries_.end() ? std::nullopt : found->parentEntry;
    }
    return depth;
}

auto OutlineEntryModel::subtreeEnd(int row) const -> int {
    if (row < 0 || static_cast<std::size_t>(row) >= entries_.size()) {
        return row;
    }
    const auto depth = entryDepth(row);
    auto end = row + 1;
    while (end < rowCount() && entryDepth(end) > depth) {
        ++end;
    }
    return end;
}

auto OutlineEntryModel::rowForId(const hieda::notebook::BlockId& identifier) const -> int {
    const auto found = std::ranges::find_if(
        entries_, [&](const auto& entry) -> bool { return entry.metadata.id == identifier; });
    return found == entries_.end() ? -1 : static_cast<int>(std::distance(entries_.begin(), found));
}

NotebookController::NotebookController(QObject* parent)
    : QObject(parent), outlineEntries_(this), midnightTimer_(this) {
    midnightTimer_.setSingleShot(true);
    connect(&midnightTimer_, &QTimer::timeout, this, [this]() -> void {
        if (hasOpenNotebook() && isJournalPage()) {
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
auto NotebookController::outlineEntries() -> QAbstractItemModel* {
    return &outlineEntries_;
}
auto NotebookController::canUndo() const -> bool {
    if (!session_.isOpen() || !journalDate_.isValid()) {
        return false;
    }
    const auto capabilities = session_.editCapabilities();
    return capabilities && capabilities.value().canUndo;
}
auto NotebookController::canRedo() const -> bool {
    if (!session_.isOpen() || !journalDate_.isValid()) {
        return false;
    }
    const auto capabilities = session_.editCapabilities();
    return capabilities && capabilities.value().canRedo;
}
auto NotebookController::isJournalPage() const -> bool {
    return !currentPageId_.has_value();
}
auto NotebookController::currentPageId() const -> QString {
    return currentPageId_ ? displayId(*currentPageId_) : QString{};
}
auto NotebookController::currentPageName() const -> QString {
    return currentPageName_;
}
auto NotebookController::currentPageTitle() const -> QString {
    return currentPageTitle_;
}
auto NotebookController::pageChoices() const -> QStringList {
    return pageChoices_;
}
auto NotebookController::pageIdAt(qsizetype index) const -> QString {
    return index >= 0 && static_cast<std::size_t>(index) < pageIds_.size()
               ? displayId(pageIds_[static_cast<std::size_t>(index)])
               : QString{};
}
auto NotebookController::pageIdForChoice(const QString& choice) const -> QString {
    return pageIdAt(pageChoices_.indexOf(choice));
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
    currentPageId_.reset();
    currentPageName_.clear();
    currentPageTitle_.clear();
    pageChoices_.clear();
    pageIds_.clear();
    outlineEntries_.setEntries({});
    emit stateChanged();
    emit destinationChanged();
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
auto NotebookController::insertOutlineEntry(const QString& authoredText,
                                            const QString& afterEntryId) -> int {
    std::optional<hieda::notebook::BlockId> after;
    if (!afterEntryId.isEmpty()) {
        after = blockId(afterEntryId);
        if (!after) {
            error_ = tr("The selected Entry is no longer available.");
            emit stateChanged();
            return -1;
        }
    }
    const auto utf8 = authoredText.toUtf8();
    try {
        std::vector<QString> existingIds;
        existingIds.reserve(static_cast<std::size_t>(outlineEntries_.rowCount()));
        for (int row = 0; row < outlineEntries_.rowCount(); ++row) {
            existingIds.push_back(outlineEntries_.entryId(row));
        }
        if (after) {
            if (std::ranges::find(existingIds, afterEntryId) == existingIds.end()) {
                error_ = tr("The selected Entry is no longer on this Page.");
                emit stateChanged();
                return -1;
            }
        }
        const auto text = std::string(utf8.constData(), static_cast<std::size_t>(utf8.size()));
        const auto result = session_.insertEntry(currentPageAddress(), after, text);
        if (!result) {
            rejectSave(result.error());
            return -1;
        }
        auto entries = asOutlineEntries(result.value().entries);
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
        outlineEntries_.insertEntry(insertedRow, *inserted);
        error_.clear();
        emit stateChanged();
        emit destinationChanged();
        return insertedRow;
    } catch (const hieda::notebook::NotebookException&) {
        error_ = tr("Hieda encountered an unexpected Notebook error.");
        emit stateChanged();
    }
    return -1;
}

auto NotebookController::outlineEntryId(int row) const -> QString {
    return outlineEntries_.entryId(row);
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
auto NotebookController::updateOutlineEntry(const QString& entryId, const QString& authoredText)
    -> bool {
    const auto id = blockId(entryId);
    if (!id) {
        error_ = tr("That Entry is no longer available.");
        emit stateChanged();
        return false;
    }
    const auto utf8 = authoredText.toUtf8();
    try {
        const auto text = std::string(utf8.constData(), static_cast<std::size_t>(utf8.size()));
        const auto result = session_.updateEntry(*id, text);
        if (!result) {
            rejectSave(result.error());
            currentPageId_ ? loadPage(*currentPageId_) : loadJournalDate(journalDate_);
            return false;
        }
        outlineEntries_.updateEntry(
            {result.value().metadata, result.value().authoredText, result.value().parentEntry});
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
auto NotebookController::splitOutlineEntry(const QString& entryId, const QString& authoredText,
                                           int cursorPosition) -> QVariantMap {
    const auto id = blockId(entryId);
    if (!id) {
        error_ = tr("That Entry is no longer available.");
        emit stateChanged();
        return outlineOutcome(false);
    }
    const auto row = outlineEntries_.rowForId(*id);
    if (row < 0 || cursorPosition < 0 || cursorPosition > authoredText.size()) {
        error_ = tr("The split cursor is no longer valid.");
        emit stateChanged();
        return outlineOutcome(false);
    }
    std::vector<QString> existingIds;
    existingIds.reserve(static_cast<std::size_t>(outlineEntries_.rowCount()));
    for (int current = 0; current < outlineEntries_.rowCount(); ++current) {
        existingIds.push_back(outlineEntries_.entryId(current));
    }
    const auto prefix = authoredText.left(cursorPosition).toUtf8();
    const auto utf8 = authoredText.toUtf8();
    try {
        const auto text = std::string(utf8.constData(), static_cast<std::size_t>(utf8.size()));
        const auto result = session_.splitEntry(*id, text, static_cast<std::size_t>(prefix.size()));
        if (!result) {
            rejectSave(result.error());
            return outlineOutcome(false);
        }
        auto entries = asOutlineEntries(result.value().entries);
        outlineEntries_.setEntries(std::move(entries));
        auto insertedRow = -1;
        for (int current = 0; current < outlineEntries_.rowCount(); ++current) {
            const auto candidate = outlineEntries_.entryId(current);
            if (std::ranges::find(existingIds, candidate) == existingIds.end()) {
                insertedRow = current;
                break;
            }
        }
        error_.clear();
        emit stateChanged();
        emit destinationChanged();
        return outlineOutcome(insertedRow >= 0, insertedRow, 0);
    } catch (const hieda::notebook::NotebookException&) {
        error_ = tr("Hieda encountered an unexpected Notebook error.");
        emit stateChanged();
        return outlineOutcome(false);
    }
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
auto NotebookController::joinOutlineEntry(const QString& entryId, const QString& authoredText)
    -> QVariantMap {
    const auto id = blockId(entryId);
    if (!id) {
        error_ = tr("That Entry is no longer available.");
        emit stateChanged();
        return outlineOutcome(false);
    }
    const auto row = outlineEntries_.rowForId(*id);
    if (row <= 0) {
        error_ = tr("That Entry cannot be joined.");
        emit stateChanged();
        return outlineOutcome(false);
    }
    const auto targetIdText = outlineEntries_.entryId(row - 1);
    const auto targetId = blockId(targetIdText);
    const auto cursor = static_cast<int>(std::min<qsizetype>(
        outlineEntries_.entryText(row - 1).size(), std::numeric_limits<int>::max()));
    const auto utf8 = authoredText.toUtf8();
    try {
        const auto text = std::string(utf8.constData(), static_cast<std::size_t>(utf8.size()));
        const auto result = session_.joinEntry(*id, text);
        if (!result) {
            rejectSave(result.error());
            return outlineOutcome(false);
        }
        auto entries = asOutlineEntries(result.value().entries);
        outlineEntries_.setEntries(std::move(entries));
        const auto targetRow = targetId ? outlineEntries_.rowForId(*targetId) : -1;
        error_.clear();
        emit stateChanged();
        emit destinationChanged();
        return outlineOutcome(targetRow >= 0, targetRow, cursor);
    } catch (const hieda::notebook::NotebookException&) {
        error_ = tr("Hieda encountered an unexpected Notebook error.");
        emit stateChanged();
        return outlineOutcome(false);
    }
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
auto NotebookController::moveOutlineEntry(const QString& entryId, const QString& authoredText,
                                          OutlineEntryMove movement, int cursorPosition)
    -> QVariantMap {
    const auto id = blockId(entryId);
    if (!id) {
        error_ = tr("That Entry is no longer available.");
        emit stateChanged();
        return outlineOutcome(false);
    }
    const auto utf8 = authoredText.toUtf8();
    try {
        const auto text = std::string(utf8.constData(), static_cast<std::size_t>(utf8.size()));
        const auto entryMove = static_cast<hieda::notebook::EntryMove>(movement);
        const auto result = session_.moveEntry(*id, entryMove, text);
        if (!result) {
            rejectSave(result.error());
            return outlineOutcome(false);
        }
        auto entries = asOutlineEntries(result.value().entries);
        outlineEntries_.setEntries(std::move(entries));
        const auto row = outlineEntries_.rowForId(*id);
        error_.clear();
        emit stateChanged();
        emit destinationChanged();
        return outlineOutcome(row >= 0, row, cursorPosition);
    } catch (const hieda::notebook::NotebookException&) {
        error_ = tr("Hieda encountered an unexpected Notebook error.");
        emit stateChanged();
        return outlineOutcome(false);
    }
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
auto NotebookController::indentOutlineEntry(const QString& entryId, const QString& authoredText,
                                            int cursorPosition) -> QVariantMap {
    return moveOutlineEntry(entryId, authoredText, OutlineEntryMove::indent, cursorPosition);
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
auto NotebookController::outdentOutlineEntry(const QString& entryId, const QString& authoredText,
                                             int cursorPosition) -> QVariantMap {
    return moveOutlineEntry(entryId, authoredText, OutlineEntryMove::outdent, cursorPosition);
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
auto NotebookController::moveOutlineEntryUp(const QString& entryId, const QString& authoredText,
                                            int cursorPosition) -> QVariantMap {
    return moveOutlineEntry(entryId, authoredText, OutlineEntryMove::up, cursorPosition);
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
auto NotebookController::moveOutlineEntryDown(const QString& entryId, const QString& authoredText,
                                              int cursorPosition) -> QVariantMap {
    return moveOutlineEntry(entryId, authoredText, OutlineEntryMove::down, cursorPosition);
}

auto NotebookController::deleteOutlineEntry(const QString& entryId) -> QVariantMap {
    const auto id = blockId(entryId);
    if (!id) {
        error_ = tr("That Entry is no longer available.");
        emit stateChanged();
        return outlineOutcome(false);
    }
    const auto oldRow = outlineEntries_.rowForId(*id);
    try {
        const auto result = session_.deleteEntry(*id);
        if (!result) {
            rejectSave(result.error());
            return outlineOutcome(false);
        }
        auto entries = asOutlineEntries(result.value().entries);
        outlineEntries_.setEntries(std::move(entries));
        const auto focusRow = std::min(oldRow, outlineEntries_.rowCount() - 1);
        const auto cursor =
            focusRow >= 0
                ? static_cast<int>(std::min<qsizetype>(outlineEntries_.entryText(focusRow).size(),
                                                       std::numeric_limits<int>::max()))
                : 0;
        error_.clear();
        emit stateChanged();
        emit destinationChanged();
        return outlineOutcome(true, focusRow, cursor);
    } catch (const hieda::notebook::NotebookException&) {
        error_ = tr("Hieda encountered an unexpected Notebook error.");
        emit stateChanged();
        return outlineOutcome(false);
    }
}

auto NotebookController::outlineSelectionText(const QStringList& entryIds) const -> QString {
    if (entryIds.empty()) {
        return {};
    }
    std::vector<bool> selected(static_cast<std::size_t>(outlineEntries_.rowCount()), false);
    auto minimumDepth = std::numeric_limits<int>::max();
    for (const auto& entryId : entryIds) {
        const auto id = blockId(entryId);
        const auto row = id ? outlineEntries_.rowForId(*id) : -1;
        if (row < 0) {
            return {};
        }
        const auto depth = outlineEntries_.entryDepth(row);
        minimumDepth = std::min(minimumDepth, depth);
        selected[static_cast<std::size_t>(row)] = true;
        for (auto candidate = row + 1; candidate < outlineEntries_.rowCount(); ++candidate) {
            const auto candidateDepth = outlineEntries_.entryDepth(candidate);
            if (candidateDepth <= depth) {
                break;
            }
            selected[static_cast<std::size_t>(candidate)] = true;
        }
    }

    QStringList output;
    for (auto row = 0; row < outlineEntries_.rowCount(); ++row) {
        if (!selected[static_cast<std::size_t>(row)]) {
            continue;
        }
        const auto depth = outlineEntries_.entryDepth(row);
        const auto indentation =
            QString(static_cast<qsizetype>(depth - minimumDepth) * 2, QLatin1Char(' '));
        const auto continuationIndent = indentation + QStringLiteral("  ");
        const auto lines =
            outlineEntries_.entryText(row).split(QLatin1Char('\n'), Qt::KeepEmptyParts);
        output.push_back(indentation + QStringLiteral("\u2022 ") + lines.front());
        for (qsizetype line = 1; line < lines.size(); ++line) {
            output.push_back(continuationIndent + lines[line]);
        }
    }
    return output.join(QLatin1Char('\n'));
}

auto NotebookController::outlineEntrySelection(int anchorRow, int extentRow) const -> QVariantMap {
    if (anchorRow < 0 || extentRow < 0 || anchorRow >= outlineEntries_.rowCount() ||
        extentRow >= outlineEntries_.rowCount()) {
        return {{QStringLiteral("roots"), QStringList{}},
                {QStringLiteral("entries"), QStringList{}}};
    }
    const auto first = std::min(anchorRow, extentRow);
    auto end =
        std::max(outlineEntries_.subtreeEnd(anchorRow), outlineEntries_.subtreeEnd(extentRow));
    for (auto row = first; row < end; ++row) {
        end = std::max(end, outlineEntries_.subtreeEnd(row));
    }

    QStringList entries;
    entries.reserve(end - first);
    for (auto row = first; row < end; ++row) {
        entries.push_back(outlineEntries_.entryId(row));
    }
    QStringList roots;
    for (auto row = first; row < end; ++row) {
        const auto parent = outlineEntries_.entryParentId(row);
        if (parent.isEmpty() || !entries.contains(parent)) {
            roots.push_back(outlineEntries_.entryId(row));
        }
    }
    return {{QStringLiteral("roots"), roots}, {QStringLiteral("entries"), entries}};
}

void NotebookController::copyTextToClipboard(const QString& text) {
    if (auto* clipboard = QGuiApplication::clipboard(); clipboard != nullptr) {
        clipboard->setText(text);
    }
}

auto NotebookController::deleteOutlineSubtrees(const QStringList& entryIds) -> QVariantMap {
    std::vector<hieda::notebook::BlockId> ids;
    ids.reserve(static_cast<std::size_t>(entryIds.size()));
    auto firstRow = outlineEntries_.rowCount();
    for (const auto& entryId : entryIds) {
        const auto id = blockId(entryId);
        const auto row = id ? outlineEntries_.rowForId(*id) : -1;
        if (!id || row < 0) {
            error_ = tr("A selected Entry is no longer available.");
            emit stateChanged();
            return outlineOutcome(false);
        }
        ids.push_back(*id);
        firstRow = std::min(firstRow, row);
    }
    if (ids.empty()) {
        return outlineOutcome(false);
    }
    try {
        const auto result = session_.deleteSubtrees(std::move(ids));
        if (!result) {
            rejectSave(result.error());
            return outlineOutcome(false);
        }
        auto entries = asOutlineEntries(result.value().entries);
        outlineEntries_.setEntries(std::move(entries));
        auto focusRow = -1;
        auto cursor = 0;
        if (outlineEntries_.rowCount() > 0) {
            focusRow = std::min(firstRow, outlineEntries_.rowCount() - 1);
            if (focusRow < firstRow) {
                cursor = static_cast<int>(std::min<qsizetype>(
                    outlineEntries_.entryText(focusRow).size(), std::numeric_limits<int>::max()));
            }
        }
        error_.clear();
        emit stateChanged();
        emit destinationChanged();
        return outlineOutcome(true, focusRow, cursor);
    } catch (const hieda::notebook::NotebookException&) {
        error_ = tr("Hieda encountered an unexpected Notebook error.");
        emit stateChanged();
        return outlineOutcome(false);
    }
}

auto NotebookController::undoOutlineEdit(const QString& preferredEntryId, int cursorPosition)
    -> QVariantMap {
    return applyOutlineHistory(OutlineHistoryDirection::undo, preferredEntryId, cursorPosition);
}

auto NotebookController::redoOutlineEdit(const QString& preferredEntryId, int cursorPosition)
    -> QVariantMap {
    return applyOutlineHistory(OutlineHistoryDirection::redo, preferredEntryId, cursorPosition);
}

auto NotebookController::applyOutlineHistory(OutlineHistoryDirection direction,
                                             const QString& preferredEntryId, int cursorPosition)
    -> QVariantMap {
    struct EntrySnapshot {
        QString id;
        QString text;
    };
    std::vector<EntrySnapshot> oldEntries;
    oldEntries.reserve(static_cast<std::size_t>(outlineEntries_.rowCount()));
    for (int row = 0; row < outlineEntries_.rowCount(); ++row) {
        oldEntries.push_back({outlineEntries_.entryId(row), outlineEntries_.entryText(row)});
    }
    try {
        auto result =
            direction == OutlineHistoryDirection::redo ? session_.redoEdit() : session_.undoEdit();
        if (!result) {
            rejectSave(result.error());
            return outlineOutcome(false);
        }
        const auto current = session_.outline(currentPageAddress());
        if (!current) {
            rejectSave(current.error());
            return outlineOutcome(false);
        }
        auto entries = asOutlineEntries(current.value().entries);
        auto focusRow = -1;
        auto preferredSurvived = false;
        const auto preferredId = blockId(preferredEntryId);
        if (preferredId) {
            const auto preferred = std::ranges::find_if(entries, [&](const auto& entry) -> auto {
                return entry.metadata.id == *preferredId;
            });
            if (preferred != entries.end()) {
                focusRow = static_cast<int>(std::distance(entries.begin(), preferred));
                preferredSurvived = true;
            }
        }
        for (std::size_t row = 0; row < entries.size(); ++row) {
            if (focusRow >= 0) {
                break;
            }
            const auto id = displayId(entries[row].metadata.id);
            const auto old = std::ranges::find_if(
                oldEntries, [&](const auto& entry) -> bool { return entry.id == id; });
            if (old == oldEntries.end()) {
                focusRow = static_cast<int>(row);
                break;
            }
            const auto oldRow = static_cast<std::size_t>(std::distance(oldEntries.begin(), old));
            const auto text =
                QString::fromUtf8(entries[row].authoredText.data(),
                                  static_cast<qsizetype>(entries[row].authoredText.size()));
            if (oldRow != row || old->text != text) {
                focusRow = static_cast<int>(row);
                break;
            }
        }
        if (focusRow < 0 && !entries.empty()) {
            focusRow = std::min(static_cast<int>(entries.size()) - 1,
                                std::max(0, static_cast<int>(oldEntries.size()) - 1));
        }
        outlineEntries_.setEntries(entries);
        auto cursor = 0;
        if (focusRow >= 0) {
            const auto desiredCursor = preferredSurvived
                                           ? std::max(0, cursorPosition)
                                           : static_cast<int>(std::min<qsizetype>(
                                                 outlineEntries_.entryText(focusRow).size(),
                                                 std::numeric_limits<int>::max()));
            cursor = static_cast<int>(
                std::min<qsizetype>(desiredCursor, outlineEntries_.entryText(focusRow).size()));
        }
        error_.clear();
        emit stateChanged();
        emit destinationChanged();
        return outlineOutcome(true, focusRow, cursor);
    } catch (const hieda::notebook::NotebookException&) {
        error_ = tr("Hieda encountered an unexpected Notebook error.");
        emit stateChanged();
        return outlineOutcome(false);
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
        if (hasOpenNotebook() && isJournalPage() && currentDate != journalDate_) {
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

auto NotebookController::createPage(const QString& name, const QString& displayTitle) -> bool {
    const auto nameUtf8 = name.toUtf8();
    const auto titleUtf8 = displayTitle.toUtf8();
    const auto result =
        session_.createPage({nameUtf8.constData(), static_cast<std::size_t>(nameUtf8.size())},
                            {titleUtf8.constData(), static_cast<std::size_t>(titleUtf8.size())});
    if (!result) {
        reject(result.error());
        return false;
    }
    refreshPages();
    loadPage(result.value().metadata.id);
    return true;
}

auto NotebookController::renameCurrentPage(const QString& name, const QString& displayTitle)
    -> bool {
    if (!currentPageId_) {
        error_ = tr("Select an ordinary Page before renaming it.");
        emit stateChanged();
        return false;
    }
    const auto nameUtf8 = name.toUtf8();
    const auto titleUtf8 = displayTitle.toUtf8();
    const auto result = session_.renamePage(
        *currentPageId_, {nameUtf8.constData(), static_cast<std::size_t>(nameUtf8.size())},
        {titleUtf8.constData(), static_cast<std::size_t>(titleUtf8.size())});
    if (!result) {
        reject(result.error());
        return false;
    }
    refreshPages();
    currentPageName_ = QString::fromUtf8(result.value().name);
    currentPageTitle_ = QString::fromUtf8(result.value().displayTitle);
    error_.clear();
    emit destinationChanged();
    emit stateChanged();
    return true;
}

void NotebookController::navigateToPage(const QString& pageId) {
    const auto id = blockId(pageId);
    if (!id) {
        error_ = tr("That Page is no longer available.");
        emit stateChanged();
        return;
    }
    loadPage(*id);
}

void NotebookController::navigateToJournalDate(const QDate& date) {
    if (!date.isValid()) {
        error_ = tr("Choose a valid Journal date.");
        emit stateChanged();
        return;
    }
    loadJournalDate(date);
}
void NotebookController::navigateToJournalDateText(const QString& isoDate) {
    navigateToJournalDate(QDate::fromString(isoDate, Qt::ISODate));
}
void NotebookController::navigateToToday() {
    loadJournalDate(QDate::currentDate());
}
void NotebookController::navigateToPreviousJournalDate() {
    loadJournalDate((journalDate_.isValid() ? journalDate_ : QDate::currentDate()).addDays(-1));
}
void NotebookController::navigateToNextJournalDate() {
    loadJournalDate((journalDate_.isValid() ? journalDate_ : QDate::currentDate()).addDays(1));
}

void NotebookController::accept(const hieda::notebook::NotebookInfo& info) {
    path_ = displayPath(info.path);
    name_ = QFileInfo(path_).completeBaseName();
    error_.clear();
    refreshPages();
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
        error_ = tr("A Entry must contain one line of Unicode text.");
        break;
    case NotebookErrorCode::blockNotFound:
        error_ = tr("That Entry is no longer available.");
        break;
    case NotebookErrorCode::invalidInsertionPoint:
        error_ = tr("The selected Entry is no longer on this Page.");
        break;
    case NotebookErrorCode::invalidCursorPosition:
        error_ = tr("The split cursor is no longer valid.");
        break;
    case NotebookErrorCode::invalidStructuralMove:
        error_ = tr("That Entry cannot move in that direction.");
        break;
    case NotebookErrorCode::blockHasChildren:
        error_ = tr("Move or delete an Entry's children first.");
        break;
    case NotebookErrorCode::undoUnavailable:
        error_ = tr("There is no outline edit to undo.");
        break;
    case NotebookErrorCode::redoUnavailable:
        error_ = tr("There is no outline edit to redo.");
        break;
    case NotebookErrorCode::invalidPageName:
        error_ = tr("A Page name must use 1–64 lowercase letters, digits, '-' or '_'.");
        break;
    case NotebookErrorCode::invalidPageTitle:
        error_ = tr("A Page title must be non-empty single-line Unicode text.");
        break;
    case NotebookErrorCode::pageNameConflict:
        error_ = tr("That Page name is already in use.");
        break;
    case NotebookErrorCode::pageNotFound:
        error_ = tr("That Page is no longer available.");
        break;
    }
    emit stateChanged();
}

void NotebookController::rejectSave(const hieda::notebook::NotebookError& error) {
    if (error.code == hieda::notebook::NotebookErrorCode::ioFailure) {
        error_ = tr("Hieda could not safely save that outline change.");
        emit stateChanged();
        return;
    }
    reject(error);
}

void NotebookController::loadJournalDate(const QDate& date) {
    try {
        const auto result = session_.outline(domainJournalDate(date));
        if (!result) {
            reject(result.error());
            return;
        }
        journalDate_ = date;
        currentPageId_.reset();
        currentPageName_.clear();
        currentPageTitle_.clear();
        outlineEntries_.setEntries(asOutlineEntries(result.value().entries));
        emit destinationChanged();
        emit stateChanged();
    } catch (const hieda::notebook::NotebookException&) {
        error_ = tr("Hieda encountered an unexpected Notebook error.");
        emit stateChanged();
    }
}

void NotebookController::loadPage(const hieda::notebook::BlockId& pageId) {
    try {
        const auto result = session_.outline(pageId);
        if (!result) {
            reject(result.error());
            return;
        }
        currentPageId_ = pageId;
        currentPageName_ = QString::fromUtf8(result.value().name);
        currentPageTitle_ = QString::fromUtf8(result.value().displayTitle);
        outlineEntries_.setEntries(asOutlineEntries(result.value().entries));
        error_.clear();
        emit destinationChanged();
        emit stateChanged();
    } catch (const hieda::notebook::NotebookException&) {
        error_ = tr("Hieda encountered an unexpected Notebook error.");
        emit stateChanged();
    }
}

auto NotebookController::currentPageAddress() const -> hieda::notebook::PageAddress {
    return currentPageId_ ? hieda::notebook::PageAddress{*currentPageId_}
                          : hieda::notebook::PageAddress{domainJournalDate(journalDate_)};
}

void NotebookController::refreshPages() {
    const auto result = session_.pages();
    if (!result) {
        reject(result.error());
        return;
    }
    pageChoices_.clear();
    pageIds_.clear();
    for (const auto& page : result.value()) {
        pageIds_.push_back(page.metadata.id);
        pageChoices_.push_back(QStringLiteral("%1 — %2").arg(QString::fromUtf8(page.displayTitle),
                                                             QString::fromUtf8(page.name)));
    }
    emit stateChanged();
}

void NotebookController::scheduleMidnightRefresh() {
    const auto now = QDateTime::currentDateTime();
    const auto nextMidnight = QDateTime(now.date().addDays(1).startOfDay());
    const auto milliseconds = std::max<qint64>(1, now.msecsTo(nextMidnight));
    midnightTimer_.start(
        static_cast<int>(std::min<qint64>(milliseconds, std::numeric_limits<int>::max())));
}
