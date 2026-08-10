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

auto
localPath(const QUrl& url) -> std::filesystem::path
{
#ifdef _WIN32
    return std::filesystem::path(url.toLocalFile().toStdWString());
#else
    const auto encoded = QFile::encodeName(url.toLocalFile());
    return {encoded.constData()};
#endif
}

auto
displayPath(const std::filesystem::path& path) -> QString
{
#ifdef _WIN32
    return QString::fromStdWString(path.native());
#else
    return QFile::decodeName(path.c_str());
#endif
}

auto
domainJournalDate(const QDate& date) -> hieda::notebook::JournalDate
{
    return {date.year(), static_cast<std::uint8_t>(date.month()),
            static_cast<std::uint8_t>(date.day())};
}

auto
blockId(const QString& text) -> std::optional<hieda::notebook::BlockId>
{
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
        id.bytes[static_cast<std::size_t>(index / 2)] =
            static_cast<std::byte>(byte);
    }
    return id;
}

auto
displayId(const hieda::notebook::BlockId& blockIdentifier) -> QString
{
    return QString::fromStdString(blockIdentifier.toString());
}

auto
outlineOutcome(bool succeeded, int row = -1, int cursorPosition = 0)
    -> QVariantMap
{
    return {{QStringLiteral("succeeded"), succeeded},
            {QStringLiteral("row"), row},
            {QStringLiteral("cursorPosition"), cursorPosition}};
}

struct CommittedEntryPosition {
    hieda::notebook::BlockId entryId;
    std::size_t byteOffset{0};
};

auto
committedEntryPosition(const OutlineEntryModel& entries,
                       const QString& entryIdText, int characterOffset,
                       const QString& editorText)
    -> std::optional<CommittedEntryPosition>
{
    const auto entryId = blockId(entryIdText);
    const auto row = entryId ? entries.rowForId(*entryId) : -1;
    if (!entryId || row < 0 || characterOffset < 0 ||
        characterOffset > editorText.size() ||
        editorText != entries.entryText(row)) {
        return std::nullopt;
    }
    return CommittedEntryPosition{
        *entryId, static_cast<std::size_t>(
                      editorText.first(characterOffset).toUtf8().size())};
}

template <typename Occurrence>
auto
linkedReferenceSnippet(std::string_view authoredText,
                       const Occurrence& occurrence) -> QString
{
    const auto lineStart =
        authoredText.rfind('\n', occurrence.sourceByteOffset);
    const auto lineEnd = authoredText.find(
        '\n', occurrence.sourceByteOffset + occurrence.sourceByteLength);
    const auto unboundedStart =
        lineStart == std::string_view::npos ? std::size_t{0} : lineStart + 1;
    auto snippetStart = unboundedStart;
    const auto boundedLineEnd =
        lineEnd == std::string_view::npos ? authoredText.size() : lineEnd;
    if (occurrence.sourceByteOffset > snippetStart + 60) {
        snippetStart = occurrence.sourceByteOffset - 60;
        while (snippetStart < occurrence.sourceByteOffset &&
               (static_cast<unsigned char>(authoredText[snippetStart]) &
                0xC0U) == 0x80U) {
            ++snippetStart;
        }
    }
    auto snippetEnd = std::min(boundedLineEnd, occurrence.sourceByteOffset +
                                                   occurrence.sourceByteLength +
                                                   std::size_t{60});
    while (snippetEnd < boundedLineEnd &&
           (static_cast<unsigned char>(authoredText[snippetEnd]) & 0xC0U) ==
               0x80U) {
        ++snippetEnd;
    }
    auto snippet =
        QString::fromUtf8(authoredText.data() + snippetStart,
                          static_cast<qsizetype>(snippetEnd - snippetStart))
            .toHtmlEscaped();
    if (snippetStart > unboundedStart) {
        snippet.prepend(QStringLiteral("…"));
    }
    if (snippetEnd < boundedLineEnd) {
        snippet.append(QStringLiteral("…"));
    }
    return snippet;
}

template <typename DomainEntry>
auto
asOutlineEntries(const std::vector<DomainEntry>& domainEntries)
    -> std::vector<OutlineEntry>
{
    std::vector<OutlineEntry> entries;
    entries.reserve(domainEntries.size());
    for (const auto& entry : domainEntries) {
        entries.push_back(
            {entry.metadata, entry.authoredText, entry.parentEntry});
    }
    return entries;
}

} // namespace

OutlineEntryModel::OutlineEntryModel(QObject* parent)
    : QAbstractListModel(parent)
{
}

auto
OutlineEntryModel::rowCount(const QModelIndex& parent) const -> int
{
    return parent.isValid() ? 0 : static_cast<int>(entries_.size());
}

auto
OutlineEntryModel::data(const QModelIndex& index, int role) const -> QVariant
{
    if (!index.isValid() || index.row() < 0 ||
        static_cast<std::size_t>(index.row()) >= entries_.size()) {
        return {};
    }
    const auto& entry = entries_[static_cast<std::size_t>(index.row())];
    if (role == EntryIdRole) {
        return displayId(entry.metadata.id);
    }
    if (role == AuthoredTextRole) {
        return QString::fromUtf8(
            entry.authoredText.data(),
            static_cast<qsizetype>(entry.authoredText.size()));
    }
    if (role == ParentEntryIdRole) {
        return entry.parentEntry ? displayId(*entry.parentEntry) : QString{};
    }
    if (role == LinkedReferenceContextRole) {
        return entry.linkedReferenceContext;
    }
    if (role == LinkedReferenceGroupRole) {
        return entry.linkedReferenceGroup;
    }
    if (role == LinkedReferencePresentationRole) {
        return entry.linkedReferencePresentation;
    }
    if (role == LinkedReferenceOccurrenceCountRole) {
        return entry.linkedReferenceOccurrenceCount;
    }
    if (role == LinkedReferenceHasMoreOccurrencesRole) {
        return entry.linkedReferenceHasMoreOccurrences;
    }
    if (role == QueryHasIntentRole) {
        return entry.queryHasIntent;
    }
    if (role == QueryErrorRole) {
        return entry.queryError;
    }
    if (role == QueryResultsRole) {
        return entry.queryResults;
    }
    if (role == QueryHasMoreRole) {
        return entry.queryHasMore;
    }
    const auto parent = entry.parentEntry;
    const auto hasChildren =
        std::ranges::any_of(entries_, [&](const auto& candidate) -> bool {
            return candidate.parentEntry == entry.metadata.id;
        });
    const auto depth = entryDepth(index.row());
    bool hasPreviousSibling = false;
    bool hasNextSibling = false;
    for (std::size_t row = 0; row < entries_.size(); ++row) {
        if (entries_[row].parentEntry != parent ||
            entries_[row].metadata.id == entry.metadata.id) {
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

auto
OutlineEntryModel::roleNames() const -> QHash<int, QByteArray>
{
    return {
        {EntryIdRole, "entryId"},
        {AuthoredTextRole, "authoredText"},
        {ParentEntryIdRole, "parentEntryId"},
        {DepthRole, "depth"},
        {HasChildrenRole, "hasChildren"},
        {CanIndentRole, "canIndent"},
        {CanOutdentRole, "canOutdent"},
        {CanMoveUpRole, "canMoveUp"},
        {CanMoveDownRole, "canMoveDown"},
        {CanDeleteRole, "canDelete"},
        {LinkedReferenceContextRole, "linkedReferenceContext"},
        {LinkedReferenceGroupRole, "linkedReferenceGroup"},
        {LinkedReferencePresentationRole, "linkedReferencePresentation"},
        {LinkedReferenceOccurrenceCountRole, "linkedReferenceOccurrenceCount"},
        {LinkedReferenceHasMoreOccurrencesRole,
         "linkedReferenceHasMoreOccurrences"},
        {QueryHasIntentRole, "queryHasIntent"},
        {QueryErrorRole, "queryError"},
        {QueryResultsRole, "queryResults"},
        {QueryHasMoreRole, "queryHasMore"}};
}

void
OutlineEntryModel::setEntries(std::vector<OutlineEntry> entries)
{
    beginResetModel();
    entries_ = std::move(entries);
    endResetModel();
}

void
OutlineEntryModel::appendEntries(std::vector<OutlineEntry> entries)
{
    if (entries.empty()) {
        return;
    }
    const auto first = static_cast<int>(entries_.size());
    const auto last = first + static_cast<int>(entries.size()) - 1;
    beginInsertRows({}, first, last);
    entries_.insert(entries_.end(), std::make_move_iterator(entries.begin()),
                    std::make_move_iterator(entries.end()));
    endInsertRows();
}

void
OutlineEntryModel::insertEntry(int row, OutlineEntry entry)
{
    if (row < 0 || row > rowCount()) {
        return;
    }
    beginInsertRows({}, row, row);
    entries_.insert(entries_.begin() + row, std::move(entry));
    endInsertRows();
    if (!entries_.empty()) {
        emit dataChanged(index(0), index(rowCount() - 1),
                         {DepthRole, HasChildrenRole, CanIndentRole,
                          CanOutdentRole, CanMoveUpRole, CanMoveDownRole,
                          CanDeleteRole});
    }
}

void
OutlineEntryModel::updateEntry(const OutlineEntry& entry)
{
    const auto found =
        std::ranges::find_if(entries_, [&](const auto& current) -> bool {
            return current.metadata.id == entry.metadata.id;
        });
    if (found == entries_.end()) {
        return;
    }
    auto updated = entry;
    updated.queryHasIntent = found->queryHasIntent;
    updated.queryError = found->queryError;
    updated.queryResults = found->queryResults;
    updated.queryHasMore = found->queryHasMore;
    *found = std::move(updated);
    const auto row = static_cast<int>(std::distance(entries_.begin(), found));
    const auto changed = index(row);
    emit dataChanged(changed, changed, {AuthoredTextRole});
}

void
OutlineEntryModel::setQueryResults(const hieda::notebook::BlockId& entryId,
                                   bool hasIntent, QString error,
                                   QVariantList results, bool hasMore,
                                   bool append)
{
    const auto found =
        std::ranges::find_if(entries_, [&](const auto& current) -> bool {
            return current.metadata.id == entryId;
        });
    if (found == entries_.end()) {
        return;
    }
    found->queryHasIntent = hasIntent;
    found->queryError = std::move(error);
    if (append) {
        found->queryResults.append(results);
    } else {
        found->queryResults = std::move(results);
    }
    found->queryHasMore = hasMore;
    const auto row = static_cast<int>(std::distance(entries_.begin(), found));
    const auto changed = index(row);
    emit dataChanged(changed, changed,
                     {QueryHasIntentRole, QueryErrorRole, QueryResultsRole,
                      QueryHasMoreRole});
}

void
OutlineEntryModel::appendLinkedReferencePresentation(
    const hieda::notebook::BlockId& entryId, const QString& presentation,
    bool hasMore)
{
    const auto found =
        std::ranges::find_if(entries_, [&](const auto& current) -> bool {
            return current.metadata.id == entryId;
        });
    if (found == entries_.end()) {
        return;
    }
    if (!presentation.isEmpty()) {
        if (!found->linkedReferencePresentation.isEmpty()) {
            found->linkedReferencePresentation.append(QStringLiteral("<br>"));
        }
        found->linkedReferencePresentation.append(presentation);
    }
    found->linkedReferenceHasMoreOccurrences = hasMore;
    const auto row = static_cast<int>(std::distance(entries_.begin(), found));
    const auto changed = index(row);
    emit dataChanged(changed, changed,
                     {LinkedReferencePresentationRole,
                      LinkedReferenceHasMoreOccurrencesRole});
}

auto
OutlineEntryModel::entryId(int row) const -> QString
{
    if (row < 0 || static_cast<std::size_t>(row) >= entries_.size()) {
        return {};
    }
    return displayId(entries_[static_cast<std::size_t>(row)].metadata.id);
}

auto
OutlineEntryModel::entryText(int row) const -> QString
{
    if (row < 0 || static_cast<std::size_t>(row) >= entries_.size()) {
        return {};
    }
    const auto& text = entries_[static_cast<std::size_t>(row)].authoredText;
    return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));
}

auto
OutlineEntryModel::entryParentId(int row) const -> QString
{
    if (row < 0 || static_cast<std::size_t>(row) >= entries_.size()) {
        return {};
    }
    const auto parent = entries_[static_cast<std::size_t>(row)].parentEntry;
    return parent ? displayId(*parent) : QString{};
}

auto
OutlineEntryModel::entryDepth(int row) const -> int
{
    if (row < 0 || static_cast<std::size_t>(row) >= entries_.size()) {
        return 0;
    }
    auto depth = 0;
    auto ancestor = entries_[static_cast<std::size_t>(row)].parentEntry;
    while (ancestor) {
        ++depth;
        const auto found =
            std::ranges::find_if(entries_, [&](const auto& candidate) -> bool {
                return candidate.metadata.id == *ancestor;
            });
        ancestor = found == entries_.end() ? std::nullopt : found->parentEntry;
    }
    return depth;
}

auto
OutlineEntryModel::subtreeEnd(int row) const -> int
{
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

auto
OutlineEntryModel::rowForId(const hieda::notebook::BlockId& identifier) const
    -> int
{
    const auto found =
        std::ranges::find_if(entries_, [&](const auto& entry) -> bool {
            return entry.metadata.id == identifier;
        });
    return found == entries_.end()
               ? -1
               : static_cast<int>(std::distance(entries_.begin(), found));
}

PageHierarchyModel::PageHierarchyModel(QObject* parent)
    : QAbstractItemModel(parent)
{
}

auto
PageHierarchyModel::node(const QModelIndex& index) -> Node*
{
    return index.isValid() ? static_cast<Node*>(index.internalPointer())
                           : nullptr;
}

auto
PageHierarchyModel::children(Node* parent)
    -> std::vector<std::unique_ptr<Node>>&
{
    return parent == nullptr ? roots_ : parent->children;
}

auto
PageHierarchyModel::children(const Node* parent) const
    -> const std::vector<std::unique_ptr<Node>>&
{
    return parent == nullptr ? roots_ : parent->children;
}

auto
PageHierarchyModel::index(int row, int column,
                          const QModelIndex& parentIndex) const -> QModelIndex
{
    if (row < 0 || column != 0) {
        return {};
    }
    const auto& siblings = children(node(parentIndex));
    return static_cast<std::size_t>(row) < siblings.size()
               ? createIndex(row, column,
                             siblings[static_cast<std::size_t>(row)].get())
               : QModelIndex{};
}

auto
PageHierarchyModel::parent(const QModelIndex& child) const -> QModelIndex
{
    const auto* childNode = node(child);
    if (childNode == nullptr || childNode->parent == nullptr) {
        return {};
    }
    const auto* parentNode = childNode->parent;
    const auto& siblings = children(parentNode->parent);
    const auto found =
        std::ranges::find_if(siblings, [&](const auto& candidate) -> bool {
            return candidate.get() == parentNode;
        });
    return found == siblings.end()
               ? QModelIndex{}
               : createIndex(
                     static_cast<int>(std::distance(siblings.begin(), found)),
                     0, const_cast<Node*>(parentNode));
}

auto
PageHierarchyModel::rowCount(const QModelIndex& parentIndex) const -> int
{
    return parentIndex.column() > 0
               ? 0
               : static_cast<int>(children(node(parentIndex)).size());
}

auto
PageHierarchyModel::hasChildren(const QModelIndex& parentIndex) const -> bool
{
    if (!parentIndex.isValid()) {
        return !roots_.empty();
    }
    const auto* parentNode = node(parentIndex);
    return parentIndex.column() == 0 && parentNode != nullptr &&
           parentNode->node.hasChildren;
}

auto
PageHierarchyModel::columnCount(const QModelIndex& /*parent*/) const -> int
{
    return 1;
}

auto
PageHierarchyModel::data(const QModelIndex& index, int role) const -> QVariant
{
    const auto* item = node(index);
    if (item == nullptr) {
        return {};
    }
    const auto& hierarchyNode = item->node;
    const auto name = QString::fromUtf8(hierarchyNode.name);
    const auto segment = QString::fromUtf8(hierarchyNode.localSegment);
    const auto title = hierarchyNode.page
                           ? QString::fromUtf8(hierarchyNode.page->displayTitle)
                           : QString{};
    switch (role) {
    case Qt::DisplayRole:
        return hierarchyNode.page ? tr("%1 — %2").arg(title, segment)
                                  : tr("%1 (Page Preview)").arg(segment);
    case PageNameRole:
        return name;
    case LocalSegmentRole:
        return segment;
    case DisplayTitleRole:
        return title;
    case MaterializedRole:
        return hierarchyNode.page.has_value();
    case HasChildrenRole:
        return hierarchyNode.hasChildren;
    case ExpandedRole:
        return currentName_.starts_with(hierarchyNode.name + '/');
    case SelectedRole:
        return currentName_ == hierarchyNode.name;
    case AccessibleDescriptionRole:
        return hierarchyNode.page
                   ? tr("Page %1, local segment %2, full Page Name %3")
                         .arg(title, segment, name)
                   : tr("Page Preview %1, full Page Name %2, not materialized")
                         .arg(segment, name);
    default:
        return {};
    }
}

auto
PageHierarchyModel::roleNames() const -> QHash<int, QByteArray>
{
    return {{Qt::DisplayRole, "display"},
            {PageNameRole, "pageName"},
            {LocalSegmentRole, "localSegment"},
            {DisplayTitleRole, "displayTitle"},
            {MaterializedRole, "materialized"},
            {HasChildrenRole, "hasChildren"},
            {ExpandedRole, "revealExpanded"},
            {SelectedRole, "currentDestination"},
            {AccessibleDescriptionRole, "accessibleDescription"}};
}

void
PageHierarchyModel::attach(hieda::notebook::NotebookSession* session)
{
    session_ = session;
}

void
PageHierarchyModel::clear()
{
    beginResetModel();
    roots_.clear();
    rootContinuationCursor_.reset();
    rootsLoaded_ = false;
    currentName_.clear();
    endResetModel();
}

auto
PageHierarchyModel::loadNextBatch(Node* parentNode) -> bool
{
    auto& loaded = parentNode == nullptr ? rootsLoaded_ : parentNode->loaded;
    auto& cursor = parentNode == nullptr ? rootContinuationCursor_
                                         : parentNode->continuationCursor;
    const auto result = session_->pageHierarchyChildren(
        parentNode == nullptr
            ? std::optional<std::string>{}
            : std::optional<std::string>{parentNode->node.name},
        cursor);
    if (!result) {
        return false;
    }
    auto& destination = children(parentNode);
    for (const auto& hierarchyNode : result.value().nodes) {
        auto child = std::make_unique<Node>();
        child->node = hierarchyNode;
        child->parent = parentNode;
        child->loaded = !hierarchyNode.hasChildren;
        destination.push_back(std::move(child));
    }
    loaded = true;
    cursor = result.value().continuationCursor;
    return true;
}

auto
PageHierarchyModel::canFetchMore(const QModelIndex& parentIndex) const -> bool
{
    const auto* parentNode = node(parentIndex);
    return session_ != nullptr && session_->isOpen() &&
           (parentNode == nullptr
                ? !rootsLoaded_ || rootContinuationCursor_.has_value()
                : !parentNode->loaded ||
                      parentNode->continuationCursor.has_value());
}

void
PageHierarchyModel::fetchMore(const QModelIndex& parentIndex)
{
    if (session_ == nullptr || !session_->isOpen()) {
        return;
    }
    auto* parentNode = node(parentIndex);
    const auto parentName =
        parentNode == nullptr
            ? std::optional<std::string>{}
            : std::optional<std::string>{parentNode->node.name};
    auto& cursor = parentNode == nullptr ? rootContinuationCursor_
                                         : parentNode->continuationCursor;
    const auto batch = session_->pageHierarchyChildren(parentName, cursor);
    if (!batch) {
        if (batch.error().code ==
            hieda::notebook::NotebookErrorCode::staleHierarchyCursor) {
            QMetaObject::invokeMethod(
                this,
                [this]() -> void {
                    static_cast<void>(refresh(QString::fromUtf8(currentName_)));
                },
                Qt::QueuedConnection);
        }
        return;
    }
    if (batch.value().nodes.empty()) {
        if (parentNode == nullptr) {
            rootsLoaded_ = true;
        } else {
            parentNode->loaded = true;
        }
        cursor.reset();
        return;
    }
    auto& destination = children(parentNode);
    const auto first = static_cast<int>(destination.size());
    const auto last = first + static_cast<int>(batch.value().nodes.size()) - 1;
    beginInsertRows(parentIndex, first, last);
    for (const auto& hierarchyNode : batch.value().nodes) {
        auto child = std::make_unique<Node>();
        child->node = hierarchyNode;
        child->parent = parentNode;
        child->loaded = !hierarchyNode.hasChildren;
        destination.push_back(std::move(child));
    }
    if (parentNode == nullptr) {
        rootsLoaded_ = true;
    } else {
        parentNode->loaded = true;
    }
    cursor = batch.value().continuationCursor;
    endInsertRows();
}

auto
PageHierarchyModel::findNode(std::string_view pageName) const -> Node*
{
    const auto findIn = [&](const auto& self, const auto& candidates) -> Node* {
        for (const auto& candidate : candidates) {
            if (candidate->node.name == pageName) {
                return candidate.get();
            }
            if (auto* found = self(self, candidate->children);
                found != nullptr) {
                return found;
            }
        }
        return nullptr;
    };
    return findIn(findIn, roots_);
}

auto
PageHierarchyModel::loadCurrentPath() -> bool
{
    Node* parentNode = nullptr;
    std::size_t segmentEnd = currentName_.find('/');
    while (!currentName_.empty()) {
        const auto targetName = currentName_.substr(0, segmentEnd);
        const auto hasMore = [&]() -> bool {
            return parentNode == nullptr
                       ? !rootsLoaded_ || rootContinuationCursor_.has_value()
                       : !parentNode->loaded ||
                             parentNode->continuationCursor.has_value();
        };
        while (findNode(targetName) == nullptr && hasMore()) {
            if (!loadNextBatch(parentNode)) {
                return false;
            }
        }
        parentNode = findNode(targetName);
        if (parentNode == nullptr || segmentEnd == std::string::npos) {
            return parentNode != nullptr;
        }
        segmentEnd = currentName_.find('/', segmentEnd + 1);
    }
    return true;
}

auto
PageHierarchyModel::refresh(const QString& currentName) -> bool
{
    const auto utf8 = currentName.toUtf8();
    currentName_.assign(utf8.constData(),
                        static_cast<std::size_t>(utf8.size()));
    beginResetModel();
    roots_.clear();
    rootContinuationCursor_.reset();
    rootsLoaded_ = false;
    auto succeeded =
        session_ == nullptr || !session_->isOpen() || loadNextBatch(nullptr);
    if (succeeded && !currentName_.empty()) {
        const auto exact = session_->pageHierarchyNode(currentName_);
        succeeded = exact && (!exact.value() || loadCurrentPath());
    }
    endResetModel();
    return succeeded;
}

auto
PageHierarchyModel::indexForPageName(const QString& pageName) const
    -> QModelIndex
{
    const auto utf8 = pageName.toUtf8();
    auto* item =
        findNode({utf8.constData(), static_cast<std::size_t>(utf8.size())});
    if (item == nullptr) {
        return {};
    }
    const auto& siblings = children(item->parent);
    const auto found =
        std::ranges::find_if(siblings, [&](const auto& candidate) -> bool {
            return candidate.get() == item;
        });
    return found == siblings.end()
               ? QModelIndex{}
               : createIndex(
                     static_cast<int>(std::distance(siblings.begin(), found)),
                     0, item);
}

auto
PageHierarchyModel::pageName(const QModelIndex& index) -> QString
{
    const auto* item = node(index);
    return item == nullptr ? QString{} : QString::fromUtf8(item->node.name);
}

NotebookController::NotebookController(QObject* parent)
    : QObject(parent), outlineEntries_(this), pagePreviewSources_(this),
      linkedReferenceSources_(this), blockLinkedReferenceSources_(this),
      pageHierarchy_(this), midnightTimer_(this)
{
    pageHierarchy_.attach(&session_);
    subscription_ = session_.subscribeToChanges([this]() -> void {
        refreshLinkedReferences();
        refreshBlockLinkedReferences();
        refreshQueries();
    });
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

auto
NotebookController::hasOpenNotebook() const -> bool
{
    return session_.isOpen();
}
auto
NotebookController::notebookPath() const -> QString
{
    return path_;
}
auto
NotebookController::notebookName() const -> QString
{
    return name_;
}
auto
NotebookController::errorMessage() const -> QString
{
    return error_;
}
auto
NotebookController::journalDate() const -> QDate
{
    return journalDate_;
}
auto
NotebookController::outlineEntries() -> QAbstractItemModel*
{
    return &outlineEntries_;
}
auto
NotebookController::pagePreviewSources() -> QAbstractItemModel*
{
    return &pagePreviewSources_;
}
auto
NotebookController::pagePreviewUnresolvedPageLinkSourceTotal() const
    -> qsizetype
{
    return pagePreviewUnresolvedPageLinkSourceTotal_;
}
auto
NotebookController::hasMorePagePreviewUnresolvedPageLinkSources() const -> bool
{
    return pagePreviewUnresolvedPageLinkSourcesCursor_.has_value();
}

auto
NotebookController::linkedReferenceSources() -> QAbstractItemModel*
{
    return &linkedReferenceSources_;
}
auto
NotebookController::blockLinkedReferenceSources() -> QAbstractItemModel*
{
    return &blockLinkedReferenceSources_;
}
auto
NotebookController::pageHierarchy() -> QAbstractItemModel*
{
    return &pageHierarchy_;
}
auto
NotebookController::canUndo() const -> bool
{
    if (!session_.isOpen()) {
        return false;
    }
    const auto capabilities = session_.editCapabilities();
    return capabilities && capabilities.value().canUndo;
}
auto
NotebookController::canRedo() const -> bool
{
    if (!session_.isOpen()) {
        return false;
    }
    const auto capabilities = session_.editCapabilities();
    return capabilities && capabilities.value().canRedo;
}
auto
NotebookController::isJournalPage() const -> bool
{
    return !currentPageId_.has_value() && !currentPagePreview_;
}
auto
NotebookController::currentPageId() const -> QString
{
    return currentPageId_ ? displayId(*currentPageId_) : QString{};
}
auto
NotebookController::currentPageName() const -> QString
{
    return currentPageName_;
}
auto
NotebookController::currentPageTitle() const -> QString
{
    return currentPageTitle_;
}
auto
NotebookController::currentPagePreview() const -> bool
{
    return currentPagePreview_;
}
auto
NotebookController::pageChoices() const -> QStringList
{
    return pageChoices_;
}
auto
NotebookController::selectedBlockReferenceTargetId() const -> QString
{
    return selectedBlockReferenceTargetId_
               ? displayId(*selectedBlockReferenceTargetId_)
               : QString{};
}
auto
NotebookController::linkedReferenceTargetId() const -> QString
{
    return pageLinkedReferences_.targetId
               ? displayId(*pageLinkedReferences_.targetId)
               : QString{};
}
auto
NotebookController::linkedReferenceTotal() const -> qsizetype
{
    return pageLinkedReferences_.total;
}
auto
NotebookController::hasMoreLinkedReferences() const -> bool
{
    return pageLinkedReferences_.cursor.has_value();
}
auto
NotebookController::blockLinkedReferenceTargetId() const -> QString
{
    return blockLinkedReferences_.targetId
               ? displayId(*blockLinkedReferences_.targetId)
               : QString{};
}
auto
NotebookController::blockLinkedReferenceTotal() const -> qsizetype
{
    return blockLinkedReferences_.total;
}
auto
NotebookController::hasMoreBlockLinkedReferences() const -> bool
{
    return blockLinkedReferences_.cursor.has_value();
}
auto
NotebookController::identifiedBlockId() const -> QString
{
    return identifiedBlockId_ ? displayId(*identifiedBlockId_) : QString{};
}
auto
NotebookController::pageIdAt(qsizetype index) const -> QString
{
    return index >= 0 && static_cast<std::size_t>(index) < pageIds_.size()
               ? displayId(pageIds_[static_cast<std::size_t>(index)])
               : QString{};
}
auto
NotebookController::pageIdForChoice(const QString& choice) const -> QString
{
    return pageIdAt(pageChoices_.indexOf(choice));
}

void
NotebookController::createNotebook(const QUrl& url)
{
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

void
NotebookController::openNotebook(const QUrl& url)
{
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

void
NotebookController::closeNotebook()
{
    session_.close();
    path_.clear();
    name_.clear();
    error_.clear();
    currentPageId_.reset();
    currentPagePreview_ = false;
    currentPageName_.clear();
    currentPageTitle_.clear();
    pagePreviewUnresolvedPageLinkSourceTotal_ = 0;
    pagePreviewUnresolvedPageLinkSourcesCursor_.reset();
    pagePreviewUnresolvedPageLinkOccurrenceCursors_.clear();
    queryCursors_.clear();
    pageChoices_.clear();
    pageIds_.clear();
    selectedBlockReferenceTargetId_.reset();
    pageLinkedReferences_ = {};
    blockLinkedReferences_ = {};
    identifiedBlockId_.reset();
    outlineEntries_.setEntries({});
    pagePreviewSources_.setEntries({});
    linkedReferenceSources_.setEntries({});
    blockLinkedReferenceSources_.setEntries({});
    pageHierarchy_.clear();
    emit stateChanged();
    emit destinationChanged();
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
auto
NotebookController::insertOutlineEntry(const QString& authoredText,
                                       const QString& afterEntryId) -> int
{
    if (currentPagePreview_) {
        error_ = tr("Create this Page before adding Entries.");
        emit stateChanged();
        return -1;
    }
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
        existingIds.reserve(
            static_cast<std::size_t>(outlineEntries_.rowCount()));
        for (int row = 0; row < outlineEntries_.rowCount(); ++row) {
            existingIds.push_back(outlineEntries_.entryId(row));
        }
        if (after) {
            if (std::ranges::find(existingIds, afterEntryId) ==
                existingIds.end()) {
                error_ = tr("The selected Entry is no longer on this Page.");
                emit stateChanged();
                return -1;
            }
        }
        const auto text = std::string(utf8.constData(),
                                      static_cast<std::size_t>(utf8.size()));
        const auto result =
            session_.insertEntry(currentPageAddress(), after, text);
        if (!result) {
            rejectSave(result.error());
            return -1;
        }
        auto entries = asOutlineEntries(result.value().entries);
        const auto inserted =
            std::ranges::find_if(entries, [&](const auto& entry) -> bool {
                return std::ranges::find(existingIds,
                                         displayId(entry.metadata.id)) ==
                       existingIds.end();
            });
        if (inserted == entries.end()) {
            error_ = tr("Hieda encountered an unexpected Notebook error.");
            emit stateChanged();
            loadJournalDate(journalDate_);
            return -1;
        }
        const auto insertedRow =
            static_cast<int>(std::distance(entries.begin(), inserted));
        outlineEntries_.insertEntry(insertedRow, *inserted);
        refreshQueries();
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

auto
NotebookController::outlineEntryId(int row) const -> QString
{
    return outlineEntries_.entryId(row);
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
auto
NotebookController::updateOutlineEntry(const QString& entryId,
                                       const QString& authoredText) -> bool
{
    const auto id = blockId(entryId);
    if (!id) {
        error_ = tr("That Entry is no longer available.");
        emit stateChanged();
        return false;
    }
    const auto utf8 = authoredText.toUtf8();
    try {
        const auto text = std::string(utf8.constData(),
                                      static_cast<std::size_t>(utf8.size()));
        const auto result = session_.updateEntry(*id, text);
        if (!result) {
            rejectSave(result.error());
            currentPageId_ ? loadPage(*currentPageId_)
                           : loadJournalDate(journalDate_);
            return false;
        }
        outlineEntries_.updateEntry({result.value().metadata,
                                     result.value().authoredText,
                                     result.value().parentEntry});
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
auto
NotebookController::splitOutlineEntry(const QString& entryId,
                                      const QString& authoredText,
                                      int cursorPosition) -> QVariantMap
{
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
        const auto text = std::string(utf8.constData(),
                                      static_cast<std::size_t>(utf8.size()));
        const auto result = session_.splitEntry(
            *id, text, static_cast<std::size_t>(prefix.size()));
        if (!result) {
            rejectSave(result.error());
            return outlineOutcome(false);
        }
        auto entries = asOutlineEntries(result.value().entries);
        outlineEntries_.setEntries(std::move(entries));
        refreshQueries();
        auto insertedRow = -1;
        for (int current = 0; current < outlineEntries_.rowCount(); ++current) {
            const auto candidate = outlineEntries_.entryId(current);
            if (std::ranges::find(existingIds, candidate) ==
                existingIds.end()) {
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
auto
NotebookController::joinOutlineEntry(const QString& entryId,
                                     const QString& authoredText) -> QVariantMap
{
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
    const auto cursor = static_cast<int>(
        std::min<qsizetype>(outlineEntries_.entryText(row - 1).size(),
                            std::numeric_limits<int>::max()));
    const auto utf8 = authoredText.toUtf8();
    try {
        const auto text = std::string(utf8.constData(),
                                      static_cast<std::size_t>(utf8.size()));
        const auto result = session_.joinEntry(*id, text);
        if (!result) {
            rejectSave(result.error());
            return outlineOutcome(false);
        }
        auto entries = asOutlineEntries(result.value().entries);
        outlineEntries_.setEntries(std::move(entries));
        refreshQueries();
        const auto targetRow =
            targetId ? outlineEntries_.rowForId(*targetId) : -1;
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
auto
NotebookController::moveOutlineEntry(const QString& entryId,
                                     const QString& authoredText,
                                     OutlineEntryMove movement,
                                     int cursorPosition) -> QVariantMap
{
    const auto id = blockId(entryId);
    if (!id) {
        error_ = tr("That Entry is no longer available.");
        emit stateChanged();
        return outlineOutcome(false);
    }
    const auto utf8 = authoredText.toUtf8();
    try {
        const auto text = std::string(utf8.constData(),
                                      static_cast<std::size_t>(utf8.size()));
        const auto entryMove =
            static_cast<hieda::notebook::EntryMove>(movement);
        const auto result = session_.moveEntry(*id, entryMove, text);
        if (!result) {
            rejectSave(result.error());
            return outlineOutcome(false);
        }
        auto entries = asOutlineEntries(result.value().entries);
        outlineEntries_.setEntries(std::move(entries));
        refreshQueries();
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
auto
NotebookController::indentOutlineEntry(const QString& entryId,
                                       const QString& authoredText,
                                       int cursorPosition) -> QVariantMap
{
    return moveOutlineEntry(entryId, authoredText, OutlineEntryMove::indent,
                            cursorPosition);
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
auto
NotebookController::outdentOutlineEntry(const QString& entryId,
                                        const QString& authoredText,
                                        int cursorPosition) -> QVariantMap
{
    return moveOutlineEntry(entryId, authoredText, OutlineEntryMove::outdent,
                            cursorPosition);
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
auto
NotebookController::moveOutlineEntryUp(const QString& entryId,
                                       const QString& authoredText,
                                       int cursorPosition) -> QVariantMap
{
    return moveOutlineEntry(entryId, authoredText, OutlineEntryMove::up,
                            cursorPosition);
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
auto
NotebookController::moveOutlineEntryDown(const QString& entryId,
                                         const QString& authoredText,
                                         int cursorPosition) -> QVariantMap
{
    return moveOutlineEntry(entryId, authoredText, OutlineEntryMove::down,
                            cursorPosition);
}

auto
NotebookController::deleteOutlineEntry(const QString& entryId) -> QVariantMap
{
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
        refreshQueries();
        const auto focusRow = std::min(oldRow, outlineEntries_.rowCount() - 1);
        const auto cursor =
            focusRow >= 0 ? static_cast<int>(std::min<qsizetype>(
                                outlineEntries_.entryText(focusRow).size(),
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

auto
NotebookController::outlineSelectionText(const QStringList& entryIds) const
    -> QString
{
    if (entryIds.empty()) {
        return {};
    }
    std::vector<bool> selected(
        static_cast<std::size_t>(outlineEntries_.rowCount()), false);
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
        for (auto candidate = row + 1; candidate < outlineEntries_.rowCount();
             ++candidate) {
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
        const auto indentation = QString(
            static_cast<qsizetype>(depth - minimumDepth) * 2, QLatin1Char(' '));
        const auto continuationIndent = indentation + QStringLiteral("  ");
        const auto lines = outlineEntries_.entryText(row).split(
            QLatin1Char('\n'), Qt::KeepEmptyParts);
        output.push_back(indentation + QStringLiteral("\u2022 ") +
                         lines.front());
        for (qsizetype line = 1; line < lines.size(); ++line) {
            output.push_back(continuationIndent + lines[line]);
        }
    }
    return output.join(QLatin1Char('\n'));
}

auto
NotebookController::outlineEntrySelection(int anchorRow, int extentRow) const
    -> QVariantMap
{
    if (anchorRow < 0 || extentRow < 0 ||
        anchorRow >= outlineEntries_.rowCount() ||
        extentRow >= outlineEntries_.rowCount()) {
        return {{QStringLiteral("roots"), QStringList{}},
                {QStringLiteral("entries"), QStringList{}}};
    }
    const auto first = std::min(anchorRow, extentRow);
    auto end = std::max(outlineEntries_.subtreeEnd(anchorRow),
                        outlineEntries_.subtreeEnd(extentRow));
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
    return {{QStringLiteral("roots"), roots},
            {QStringLiteral("entries"), entries}};
}

void
NotebookController::copyTextToClipboard(const QString& text)
{
    if (auto* clipboard = QGuiApplication::clipboard(); clipboard != nullptr) {
        clipboard->setText(text);
    }
}

auto
NotebookController::deleteOutlineSubtrees(const QStringList& entryIds)
    -> QVariantMap
{
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
        refreshQueries();
        auto focusRow = -1;
        auto cursor = 0;
        if (outlineEntries_.rowCount() > 0) {
            focusRow = std::min(firstRow, outlineEntries_.rowCount() - 1);
            if (focusRow < firstRow) {
                cursor = static_cast<int>(std::min<qsizetype>(
                    outlineEntries_.entryText(focusRow).size(),
                    std::numeric_limits<int>::max()));
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

auto
NotebookController::undoOutlineEdit(const QString& preferredEntryId,
                                    int cursorPosition) -> QVariantMap
{
    return applyOutlineHistory(OutlineHistoryDirection::undo, preferredEntryId,
                               cursorPosition);
}

auto
NotebookController::redoOutlineEdit(const QString& preferredEntryId,
                                    int cursorPosition) -> QVariantMap
{
    return applyOutlineHistory(OutlineHistoryDirection::redo, preferredEntryId,
                               cursorPosition);
}

auto
NotebookController::applyOutlineHistory(OutlineHistoryDirection direction,
                                        const QString& preferredEntryId,
                                        int cursorPosition) -> QVariantMap
{
    struct EntrySnapshot {
        QString id;
        QString text;
    };
    std::vector<EntrySnapshot> oldEntries;
    oldEntries.reserve(static_cast<std::size_t>(outlineEntries_.rowCount()));
    for (int row = 0; row < outlineEntries_.rowCount(); ++row) {
        oldEntries.push_back(
            {outlineEntries_.entryId(row), outlineEntries_.entryText(row)});
    }
    try {
        auto result = direction == OutlineHistoryDirection::redo
                          ? session_.redoEdit()
                          : session_.undoEdit();
        if (!result) {
            rejectSave(result.error());
            return outlineOutcome(false);
        }
        if (currentPagePreview_) {
            const auto restored = std::ranges::find_if(
                result.value(), [&](const auto& page) -> bool {
                    return page.kind == hieda::notebook::PageKind::named &&
                           QString::fromUtf8(page.name) == currentPageName_ &&
                           page.metadata;
                });
            if (restored != result.value().end()) {
                if (const auto restoredMetadata = restored->metadata) {
                    currentPageId_ = restoredMetadata->id;
                    currentPagePreview_ = false;
                    currentPageTitle_ =
                        QString::fromUtf8(restored->displayTitle);
                }
            }
            if (currentPagePreview_) {
                loadPagePreview(currentPageName_);
                refreshPages();
                return outlineOutcome(true);
            }
        }
        auto current = session_.outline(currentPageAddress());
        if (!current && currentPageId_ &&
            current.error().code ==
                hieda::notebook::NotebookErrorCode::pageNotFound) {
            currentPageId_.reset();
            currentPagePreview_ = true;
            currentPageTitle_.clear();
            loadPagePreview(currentPageName_);
            refreshPages();
            return outlineOutcome(true);
        }
        if (!current) {
            rejectSave(current.error());
            return outlineOutcome(false);
        }
        auto entries = asOutlineEntries(current.value().entries);
        auto focusRow = -1;
        auto preferredSurvived = false;
        const auto preferredId = blockId(preferredEntryId);
        if (preferredId) {
            const auto preferred =
                std::ranges::find_if(entries, [&](const auto& entry) -> auto {
                    return entry.metadata.id == *preferredId;
                });
            if (preferred != entries.end()) {
                focusRow =
                    static_cast<int>(std::distance(entries.begin(), preferred));
                preferredSurvived = true;
            }
        }
        for (std::size_t row = 0; row < entries.size(); ++row) {
            if (focusRow >= 0) {
                break;
            }
            const auto id = displayId(entries[row].metadata.id);
            const auto old = std::ranges::find_if(
                oldEntries,
                [&](const auto& entry) -> bool { return entry.id == id; });
            if (old == oldEntries.end()) {
                focusRow = static_cast<int>(row);
                break;
            }
            const auto oldRow = static_cast<std::size_t>(
                std::distance(oldEntries.begin(), old));
            const auto text = QString::fromUtf8(
                entries[row].authoredText.data(),
                static_cast<qsizetype>(entries[row].authoredText.size()));
            if (oldRow != row || old->text != text) {
                focusRow = static_cast<int>(row);
                break;
            }
        }
        if (focusRow < 0 && !entries.empty()) {
            focusRow =
                std::min(static_cast<int>(entries.size()) - 1,
                         std::max(0, static_cast<int>(oldEntries.size()) - 1));
        }
        outlineEntries_.setEntries(entries);
        refreshQueries();
        refreshPages();
        auto cursor = 0;
        if (focusRow >= 0) {
            const auto desiredCursor =
                preferredSurvived
                    ? std::max(0, cursorPosition)
                    : static_cast<int>(std::min<qsizetype>(
                          outlineEntries_.entryText(focusRow).size(),
                          std::numeric_limits<int>::max()));
            cursor = static_cast<int>(std::min<qsizetype>(
                desiredCursor, outlineEntries_.entryText(focusRow).size()));
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

void
NotebookController::requestJournalDateRollover(const QDate& date)
{
    if (hasOpenNotebook() && date.isValid()) {
        pendingJournalDate_ = date;
        emit journalDateRolloverRequested();
    }
}

void
NotebookController::completeJournalDateRollover()
{
    if (hasOpenNotebook() && pendingJournalDate_.isValid()) {
        loadJournalDate(pendingJournalDate_);
        pendingJournalDate_ = {};
    }
}

auto
NotebookController::eventFilter(QObject* watched, QEvent* event) -> bool
{
    if (watched == QCoreApplication::instance() &&
        event->type() == QEvent::ApplicationStateChange) {
        const auto currentDate = QDate::currentDate();
        if (hasOpenNotebook() && isJournalPage() &&
            currentDate != journalDate_) {
            requestJournalDateRollover(currentDate);
        }
        scheduleMidnightRefresh();
    }
    return QObject::eventFilter(watched, event);
}

void
NotebookController::clearError()
{
    if (!error_.isEmpty()) {
        error_.clear();
        emit stateChanged();
    }
}

auto
NotebookController::createPage(const QString& name, const QString& displayTitle)
    -> bool
{
    const auto nameUtf8 = name.toUtf8();
    const auto titleUtf8 = displayTitle.toUtf8();
    const auto result = session_.createPage(
        {nameUtf8.constData(), static_cast<std::size_t>(nameUtf8.size())},
        {titleUtf8.constData(), static_cast<std::size_t>(titleUtf8.size())});
    if (!result) {
        reject(result.error());
        return false;
    }
    refreshPages();
    loadPage(result.value().metadata.id);
    return true;
}

auto
NotebookController::createCurrentPage(const QString& displayTitle) -> bool
{
    if (!currentPagePreview_) {
        error_ = tr("Open a Page Preview before creating it.");
        emit stateChanged();
        return false;
    }
    return createPage(currentPageName_, displayTitle);
}

auto
NotebookController::deleteCurrentPage() -> bool
{
    if (!currentPageId_) {
        error_ = tr("Select a materialized Page before deleting it.");
        emit stateChanged();
        return false;
    }
    const auto name = currentPageName_;
    const auto result = session_.deletePage(*currentPageId_);
    if (!result) {
        rejectSave(result.error());
        return false;
    }
    loadPagePreview(name);
    refreshPages();
    return true;
}

auto
NotebookController::renameCurrentPage(const QString& name,
                                      const QString& displayTitle) -> bool
{
    if (!currentPageId_) {
        error_ = tr("Select an ordinary Page before renaming it.");
        emit stateChanged();
        return false;
    }
    const auto nameUtf8 = name.toUtf8();
    const auto titleUtf8 = displayTitle.toUtf8();
    const auto result = session_.renamePage(
        *currentPageId_,
        {nameUtf8.constData(), static_cast<std::size_t>(nameUtf8.size())},
        {titleUtf8.constData(), static_cast<std::size_t>(titleUtf8.size())});
    if (!result) {
        reject(result.error());
        return false;
    }
    refreshPages();
    currentPageName_ = QString::fromUtf8(result.value().name);
    currentPageTitle_ = QString::fromUtf8(result.value().displayTitle);
    for (const auto& entry : result.value().entries) {
        outlineEntries_.updateEntry(
            {entry.metadata, entry.authoredText, entry.parentEntry});
    }
    static_cast<void>(pageHierarchy_.refresh(currentPageName_));
    error_.clear();
    emit destinationChanged();
    emit stateChanged();
    return true;
}

void
NotebookController::navigateToPage(const QString& pageId)
{
    const auto id = blockId(pageId);
    if (!id) {
        error_ = tr("That Page is no longer available.");
        emit stateChanged();
        return;
    }
    loadPage(*id);
}

void
NotebookController::navigateToPageName(const QString& pageName)
{
    const auto utf8 = pageName.toUtf8();
    const auto node = session_.pageHierarchyNode(
        {utf8.constData(), static_cast<std::size_t>(utf8.size())});
    if (!node) {
        reject(node.error());
        return;
    }
    if (node.value() && node.value()->page) {
        loadPage(node.value()->page->metadata.id);
        return;
    }
    loadPagePreview(pageName);
}

auto
NotebookController::followPageLink(const QString& entryId, int characterOffset,
                                   const QString& editorText) -> bool
{
    const auto position = committedEntryPosition(outlineEntries_, entryId,
                                                 characterOffset, editorText);
    if (!position) {
        return false;
    }
    const auto destination =
        session_.followPageLink(position->entryId, position->byteOffset);
    if (!destination) {
        if (destination.error().code !=
            hieda::notebook::NotebookErrorCode::pageLinkNotFound) {
            reject(destination.error());
        }
        return false;
    }
    if (const auto* page =
            std::get_if<hieda::notebook::PageSummary>(&destination.value())) {
        loadPage(page->metadata.id);
    } else {
        loadPagePreview(QString::fromUtf8(
            std::get<hieda::notebook::PagePreview>(destination.value()).name));
    }
    return true;
}

auto
NotebookController::followBlockReference(const QString& entryId,
                                         int characterOffset,
                                         const QString& editorText) -> bool
{
    const auto position = committedEntryPosition(outlineEntries_, entryId,
                                                 characterOffset, editorText);
    if (!position) {
        return false;
    }
    const auto destination =
        session_.followBlockReference(position->entryId, position->byteOffset);
    if (!destination) {
        if (destination.error().code ==
            hieda::notebook::NotebookErrorCode::blockNotFound) {
            error_ = tr("Block not found.");
            emit stateChanged();
        } else if (destination.error().code !=
                   hieda::notebook::NotebookErrorCode::blockReferenceNotFound) {
            reject(destination.error());
        }
        return false;
    }
    static_cast<void>(
        navigateToOutlinePage(destination.value().structuralPage));
    identifiedBlockId_ = destination.value().target.id;
    emit stateChanged();
    return true;
}

auto
NotebookController::followSemanticReference(const QString& entryId,
                                            int characterOffset,
                                            const QString& editorText) -> bool
{
    identifiedBlockId_.reset();
    return followPageLink(entryId, characterOffset, editorText) ||
           followBlockReference(entryId, characterOffset, editorText);
}

auto
NotebookController::insertBlockReference(const QString& sourceEntryId,
                                         int characterOffset,
                                         const QString& editorText,
                                         const QString& targetIdText) -> bool
{
    const auto sourcePosition = committedEntryPosition(
        outlineEntries_, sourceEntryId, characterOffset, editorText);
    const auto targetId = blockId(targetIdText);
    if (!sourcePosition || !targetId) {
        return false;
    }
    const auto result = session_.insertBlockReference(
        sourcePosition->entryId, sourcePosition->byteOffset, *targetId);
    if (!result) {
        rejectSave(result.error());
        return false;
    }
    outlineEntries_.updateEntry({result.value().metadata,
                                 result.value().authoredText,
                                 result.value().parentEntry});
    error_.clear();
    emit stateChanged();
    return true;
}

auto
NotebookController::blockReferenceNotation(const QString& targetId) -> QString
{
    return blockId(targetId)
               ? QStringLiteral("[[block:%1]]").arg(targetId.toLower())
               : QString{};
}

auto
NotebookController::selectBlockReferenceTarget(const QString& targetIdText)
    -> bool
{
    const auto targetId = blockId(targetIdText);
    if (!targetId) {
        return false;
    }
    const auto linked = session_.linkedReferences(*targetId);
    if (!linked) {
        reject(linked.error());
        return false;
    }
    selectedBlockReferenceTargetId_ = targetId;
    error_.clear();
    emit stateChanged();
    return true;
}

auto
NotebookController::insertSelectedBlockReference(const QString& sourceEntryId,
                                                 int characterOffset,
                                                 const QString& editorText)
    -> bool
{
    return selectedBlockReferenceTargetId_ &&
           insertBlockReference(sourceEntryId, characterOffset, editorText,
                                displayId(*selectedBlockReferenceTargetId_));
}

auto
NotebookController::browseLinkedReferences(const QString& targetIdText) -> bool
{
    const auto targetId = blockId(targetIdText);
    if (!targetId) {
        return false;
    }
    const auto linked = session_.linkedReferences(*targetId);
    if (!linked) {
        reject(linked.error());
        return false;
    }
    blockLinkedReferences_.targetId = targetId;
    refreshBlockLinkedReferences();
    emit stateChanged();
    return true;
}

auto
NotebookController::followLinkedReferenceSource(const QString& sourceEntryId)
    -> bool
{
    return followLinkedReferenceSourceFor(sourceEntryId, pageLinkedReferences_);
}

auto
NotebookController::followBlockLinkedReferenceSource(
    const QString& sourceEntryId) -> bool
{
    return followLinkedReferenceSourceFor(sourceEntryId,
                                          blockLinkedReferences_);
}

auto
NotebookController::followLinkedReferenceSourceFor(
    const QString& sourceEntryId, const IncomingSourcesViewState& view) -> bool
{
    const auto sourceId = blockId(sourceEntryId);
    if (!sourceId || !view.targetId) {
        return false;
    }
    std::optional<std::string> cursor;
    do {
        const auto linked = session_.linkedReferences(*view.targetId, cursor);
        if (!linked) {
            reject(linked.error());
            return false;
        }
        const auto found = std::ranges::find_if(
            linked.value().sources, [&](const auto& source) {
                return source.source.metadata.id == *sourceId;
            });
        if (found != linked.value().sources.end()) {
            return navigateToOutlinePage(found->structuralPage);
        }
        cursor = linked.value().continuationCursor;
    } while (cursor);
    return false;
}

auto
NotebookController::linkedReferenceContext(const QString& sourceEntryId) const
    -> QString
{
    return pageLinkedReferences_.contexts.value(sourceEntryId);
}

auto
NotebookController::followPagePreviewSource(const QString& sourceEntryId)
    -> bool
{
    const auto sourceId = blockId(sourceEntryId);
    if (!sourceId || !currentPagePreview_) {
        return false;
    }
    const auto destination = session_.locateBlock(*sourceId);
    if (!destination) {
        reject(destination.error());
        return false;
    }
    static_cast<void>(
        navigateToOutlinePage(destination.value().structuralPage));
    identifiedBlockId_ = *sourceId;
    emit stateChanged();
    return true;
}

auto
NotebookController::navigateToOutlinePage(
    const hieda::notebook::OutlinePage& page) -> bool
{
    if (page.kind == hieda::notebook::PageKind::journal) {
        const auto date =
            page.journalDate.value_or(hieda::notebook::JournalDate{});
        loadJournalDate(QDate(date.year, date.month, date.day));
        return true;
    }
    if (page.metadata) {
        loadPage(page.metadata->id);
        return true;
    }
    return false;
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
auto
NotebookController::committedEntryPresentation(
    const QString& entryId, const QString& authoredText) const -> QString
{
    const auto decodeText = [](std::string_view text) -> QString {
        return QString::fromUtf8(text.data(),
                                 static_cast<qsizetype>(text.size()));
    };
    const auto escapeText = [](const QString& text) -> QString {
        auto escaped = text.toHtmlEscaped();
        return escaped.replace(QLatin1Char('\n'), QStringLiteral("<br>"));
    };
    const auto id = blockId(entryId);
    if (!id) {
        return authoredText.toHtmlEscaped();
    }
    const auto utf8 = authoredText.toUtf8();
    const std::string_view source{utf8.constData(),
                                  static_cast<std::size_t>(utf8.size())};
    const auto pageLinks = session_.pageLinks(*id);
    const auto blockReferences = session_.blockReferences(*id);
    if (!pageLinks || !blockReferences) {
        return escapeText(decodeText(source));
    }
    struct SemanticReferencePresentation {
        std::size_t offset{0};
        std::size_t length{0};
        QString label;
    };
    std::vector<SemanticReferencePresentation> references;
    references.reserve(pageLinks.value().size() +
                       blockReferences.value().size());
    for (const auto& link : pageLinks.value()) {
        references.push_back(
            {link.sourceByteOffset, link.sourceByteLength,
             QString::fromUtf8(link.target ? link.target->displayTitle
                                           : link.pageName)});
    }
    for (const auto& reference : blockReferences.value()) {
        const auto prefix =
            QString::fromStdString(reference.targetId.toString()).first(8);
        references.push_back(
            {reference.sourceByteOffset, reference.sourceByteLength,
             reference.target ? tr("Block %1").arg(prefix)
                              : tr("Missing Block %1").arg(prefix)});
    }
    std::ranges::sort(references, {}, &SemanticReferencePresentation::offset);
    QString presentation;
    std::size_t sourceOffset = 0;
    qsizetype characterOffset = 0;
    for (const auto& reference : references) {
        const auto prefix = decodeText(
            source.substr(sourceOffset, reference.offset - sourceOffset));
        presentation += escapeText(prefix);
        characterOffset += prefix.size();
        presentation += QStringLiteral("<a href=\"%1\">%2</a>")
                            .arg(characterOffset)
                            .arg(reference.label.toHtmlEscaped());
        characterOffset +=
            decodeText(source.substr(reference.offset, reference.length))
                .size();
        sourceOffset = reference.offset + reference.length;
    }
    presentation += escapeText(decodeText(source.substr(sourceOffset)));
    return presentation;
}

void
NotebookController::navigateToJournalDate(const QDate& date)
{
    if (!date.isValid()) {
        error_ = tr("Choose a valid Journal date.");
        emit stateChanged();
        return;
    }
    loadJournalDate(date);
}
void
NotebookController::navigateToJournalDateText(const QString& isoDate)
{
    navigateToJournalDate(QDate::fromString(isoDate, Qt::ISODate));
}
void
NotebookController::navigateToToday()
{
    loadJournalDate(QDate::currentDate());
}
void
NotebookController::navigateToPreviousJournalDate()
{
    loadJournalDate(
        (journalDate_.isValid() ? journalDate_ : QDate::currentDate())
            .addDays(-1));
}
void
NotebookController::navigateToNextJournalDate()
{
    loadJournalDate(
        (journalDate_.isValid() ? journalDate_ : QDate::currentDate())
            .addDays(1));
}

void
NotebookController::accept(const hieda::notebook::NotebookInfo& info)
{
    path_ = displayPath(info.path);
    name_ = QFileInfo(path_).completeBaseName();
    error_.clear();
    refreshPages();
    loadJournalDate(QDate::currentDate());
    emit stateChanged();
}

void
NotebookController::reject(const hieda::notebook::NotebookError& error)
{
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
        error_ =
            tr("That Notebook was created by an unsupported Hieda version.");
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
        error_ = tr("An Entry must contain at most 1 MiB of valid Unicode text "
                    "and no control "
                    "characters other than line feeds.");
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
        error_ = tr("A Page name must use slash-separated 1–64 character "
                    "lowercase ASCII segments "
                    "and be at most 255 bytes.");
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
    case NotebookErrorCode::staleHierarchyCursor:
        error_ =
            tr("The Page Hierarchy changed; reload it from the beginning.");
        break;
    case NotebookErrorCode::pageLinkNotFound:
        error_ = tr("Place the cursor inside a committed Page Link before "
                    "following it.");
        break;
    case NotebookErrorCode::blockReferenceNotFound:
        error_ = tr("Place the cursor inside a committed Block Reference "
                    "before following it.");
        break;
    case NotebookErrorCode::staleLinkedReferencesCursor:
        error_ =
            tr("Linked References changed; reload them from the beginning.");
        break;
    case NotebookErrorCode::staleQueryCursor:
        error_ = tr("Query results changed; reload them from the beginning.");
        break;
    }
    emit stateChanged();
}

void
NotebookController::rejectSave(const hieda::notebook::NotebookError& error)
{
    if (error.code == hieda::notebook::NotebookErrorCode::ioFailure) {
        error_ = tr("Hieda could not safely save that outline change.");
        emit stateChanged();
        return;
    }
    reject(error);
}

void
NotebookController::loadJournalDate(const QDate& date)
{
    try {
        const auto result = session_.outline(domainJournalDate(date));
        if (!result) {
            reject(result.error());
            return;
        }
        journalDate_ = date;
        currentPageId_.reset();
        currentPagePreview_ = false;
        currentPageName_.clear();
        currentPageTitle_.clear();
        outlineEntries_.setEntries(asOutlineEntries(result.value().entries));
        refreshQueries();
        pagePreviewSources_.setEntries({});
        pageLinkedReferences_.targetId =
            result.value().metadata
                ? std::optional<hieda::notebook::BlockId>{result.value()
                                                              .metadata->id}
                : std::nullopt;
        refreshLinkedReferences();
        static_cast<void>(pageHierarchy_.refresh({}));
        emit destinationChanged();
        emit stateChanged();
    } catch (const hieda::notebook::NotebookException&) {
        error_ = tr("Hieda encountered an unexpected Notebook error.");
        emit stateChanged();
    }
}

void
NotebookController::loadPage(const hieda::notebook::BlockId& pageId)
{
    try {
        const auto result = session_.outline(pageId);
        if (!result) {
            reject(result.error());
            return;
        }
        currentPageId_ = pageId;
        currentPagePreview_ = false;
        journalDate_ = {};
        currentPageName_ = QString::fromUtf8(result.value().name);
        currentPageTitle_ = QString::fromUtf8(result.value().displayTitle);
        outlineEntries_.setEntries(asOutlineEntries(result.value().entries));
        refreshQueries();
        pagePreviewSources_.setEntries({});
        pageLinkedReferences_.targetId = pageId;
        refreshLinkedReferences();
        static_cast<void>(pageHierarchy_.refresh(currentPageName_));
        error_.clear();
        emit destinationChanged();
        emit stateChanged();
    } catch (const hieda::notebook::NotebookException&) {
        error_ = tr("Hieda encountered an unexpected Notebook error.");
        emit stateChanged();
    }
}

void
NotebookController::loadPagePreview(const QString& pageName)
{
    const auto utf8 = pageName.toUtf8();
    const std::string name{utf8.constData(),
                           static_cast<std::size_t>(utf8.size())};
    const auto preview = session_.unresolvedPageLinkSources(name);
    if (!preview) {
        if (preview.error().code !=
            hieda::notebook::NotebookErrorCode::pageNameConflict) {
            reject(preview.error());
            return;
        }
    }
    currentPageId_.reset();
    currentPagePreview_ = true;
    journalDate_ = {};
    currentPageName_ = pageName;
    currentPageTitle_.clear();
    outlineEntries_.setEntries({});
    pageLinkedReferences_.targetId.reset();
    linkedReferenceSources_.setEntries({});
    pageLinkedReferences_.contexts.clear();
    pageLinkedReferences_.total = 0;
    pagePreviewSources_.setEntries({});
    pagePreviewUnresolvedPageLinkOccurrenceCursors_.clear();
    pagePreviewUnresolvedPageLinkSourceTotal_ = 0;
    pagePreviewUnresolvedPageLinkSourcesCursor_.reset();
    if (preview) {
        appendPagePreviewUnresolvedPageLinkSourcesBatch(preview.value());
    }
    error_.clear();
    static_cast<void>(pageHierarchy_.refresh(currentPageName_));
    emit destinationChanged();
    emit stateChanged();
}

void
NotebookController::appendPagePreviewUnresolvedPageLinkSourcesBatch(
    const hieda::notebook::UnresolvedPageLinkSourcesBatch& batch)
{
    IncomingSourcesViewState view;
    appendIncomingSourcesBatch(batch, view, pagePreviewSources_);
    pagePreviewUnresolvedPageLinkSourceTotal_ = view.total;
    pagePreviewUnresolvedPageLinkSourcesCursor_ = view.cursor;
    const auto utf8 = currentPageName_.toUtf8();
    const std::string name{utf8.constData(),
                           static_cast<std::size_t>(utf8.size())};
    for (const auto& source : batch.sources) {
        if (source.occurrenceCount <= source.occurrences.size()) {
            continue;
        }
        const auto first = session_.unresolvedPageLinkOccurrences(
            name, source.source.metadata.id);
        if (first && first.value().continuationCursor) {
            pagePreviewUnresolvedPageLinkOccurrenceCursors_.insert(
                displayId(source.source.metadata.id),
                *first.value().continuationCursor);
        }
    }
}

auto
NotebookController::loadMorePagePreviewUnresolvedPageLinkSources() -> bool
{
    if (!currentPagePreview_ || !pagePreviewUnresolvedPageLinkSourcesCursor_) {
        return false;
    }
    const auto utf8 = currentPageName_.toUtf8();
    auto batch = session_.unresolvedPageLinkSources(
        {utf8.constData(), static_cast<std::size_t>(utf8.size())},
        pagePreviewUnresolvedPageLinkSourcesCursor_);
    if (!batch &&
        batch.error().code ==
            hieda::notebook::NotebookErrorCode::staleLinkedReferencesCursor) {
        loadPagePreview(currentPageName_);
        return true;
    }
    if (!batch) {
        reject(batch.error());
        return false;
    }
    appendPagePreviewUnresolvedPageLinkSourcesBatch(batch.value());
    emit stateChanged();
    return true;
}

auto
NotebookController::loadMorePagePreviewUnresolvedPageLinkOccurrences(
    const QString& sourceEntryId) -> bool
{
    const auto sourceId = blockId(sourceEntryId);
    const auto foundCursor =
        pagePreviewUnresolvedPageLinkOccurrenceCursors_.find(sourceEntryId);
    if (!currentPagePreview_ || !sourceId ||
        foundCursor == pagePreviewUnresolvedPageLinkOccurrenceCursors_.end()) {
        return false;
    }
    const auto utf8 = currentPageName_.toUtf8();
    const auto batch = session_.unresolvedPageLinkOccurrences(
        {utf8.constData(), static_cast<std::size_t>(utf8.size())}, *sourceId,
        foundCursor.value());
    if (!batch) {
        if (batch.error().code ==
            hieda::notebook::NotebookErrorCode::staleLinkedReferencesCursor) {
            loadPagePreview(currentPageName_);
            return true;
        }
        reject(batch.error());
        return false;
    }
    const auto row = pagePreviewSources_.rowForId(*sourceId);
    const auto authored = pagePreviewSources_.entryText(row).toUtf8();
    QStringList snippets;
    for (const auto& occurrence : batch.value().occurrences) {
        snippets.push_back(linkedReferenceSnippet(
            {authored.constData(), static_cast<std::size_t>(authored.size())},
            occurrence));
    }
    if (batch.value().continuationCursor) {
        foundCursor.value() = *batch.value().continuationCursor;
    } else {
        pagePreviewUnresolvedPageLinkOccurrenceCursors_.erase(foundCursor);
    }
    pagePreviewSources_.appendLinkedReferencePresentation(
        *sourceId, snippets.join(QStringLiteral("<br>")),
        batch.value().continuationCursor.has_value());
    emit stateChanged();
    return true;
}

auto
NotebookController::currentPageAddress() const -> hieda::notebook::PageAddress
{
    return currentPageId_
               ? hieda::notebook::PageAddress{*currentPageId_}
               : hieda::notebook::PageAddress{domainJournalDate(journalDate_)};
}

void
NotebookController::applyQueryBatch(
    const hieda::notebook::BlockId& queryEntryId,
    const hieda::notebook::QueryResultsBatch& batch, bool append)
{
    QVariantList rows;
    rows.reserve(static_cast<qsizetype>(batch.rows.size()));
    for (const auto& row : batch.rows) {
        QString presentation;
        if (row.type == hieda::notebook::QueryResultBlockType::page) {
            if (row.pageKind == hieda::notebook::PageKind::journal &&
                row.journalDate) {
                presentation =
                    QDate(row.journalDate->year, row.journalDate->month,
                          row.journalDate->day)
                        .toString(Qt::ISODate);
            } else {
                presentation = QStringLiteral("%1 — %2").arg(
                    QString::fromUtf8(row.displayTitle),
                    QString::fromUtf8(row.pageName));
            }
        } else {
            presentation = QString::fromUtf8(
                row.authoredText.data(),
                static_cast<qsizetype>(row.authoredText.size()));
        }
        QString context;
        if (row.pageKind == hieda::notebook::PageKind::journal &&
            row.journalDate) {
            context = QDate(row.journalDate->year, row.journalDate->month,
                            row.journalDate->day)
                          .toString(Qt::ISODate);
        } else {
            context = QString::fromUtf8(row.displayTitle);
        }
        rows.push_back(QVariantMap{
            {QStringLiteral("blockId"), displayId(row.metadata.id)},
            {QStringLiteral("presentation"), presentation},
            {QStringLiteral("context"), context},
            {QStringLiteral("isPage"),
             row.type == hieda::notebook::QueryResultBlockType::page},
        });
    }
    QString error;
    if (batch.error) {
        error = tr("%1 (byte %2)")
                    .arg(QString::fromUtf8(batch.error->message),
                         QString::number(batch.error->sourceByteOffset));
    }
    const auto idText = displayId(queryEntryId);
    if (batch.continuationCursor) {
        queryCursors_.insert(idText, *batch.continuationCursor);
    } else {
        queryCursors_.remove(idText);
    }
    outlineEntries_.setQueryResults(
        queryEntryId, batch.hasQueryIntent, std::move(error), std::move(rows),
        batch.continuationCursor.has_value(), append);
}

void
NotebookController::refreshQueries()
{
    queryCursors_.clear();
    for (int row = 0; row < outlineEntries_.rowCount(); ++row) {
        const auto id = blockId(outlineEntries_.entryId(row));
        if (!id) {
            continue;
        }
        const auto result = session_.evaluateQuery(*id);
        if (result) {
            applyQueryBatch(*id, result.value(), false);
        }
    }
}

auto
NotebookController::loadMoreQueryResults(const QString& queryEntryId) -> bool
{
    const auto id = blockId(queryEntryId);
    const auto cursor = queryCursors_.find(queryEntryId);
    if (!id || cursor == queryCursors_.end()) {
        return false;
    }
    const auto result = session_.evaluateQuery(*id, cursor.value());
    if (!result && result.error().code ==
                       hieda::notebook::NotebookErrorCode::staleQueryCursor) {
        refreshQueries();
        return true;
    }
    if (!result) {
        reject(result.error());
        return false;
    }
    applyQueryBatch(*id, result.value(), true);
    return true;
}

auto
NotebookController::followQueryResult(const QString& blockIdText) -> bool
{
    const auto id = blockId(blockIdText);
    if (!id) {
        return false;
    }
    const auto destination = session_.locateBlock(*id);
    if (!destination) {
        reject(destination.error());
        return false;
    }
    if (!navigateToOutlinePage(destination.value().structuralPage)) {
        return false;
    }
    identifiedBlockId_ = *id;
    emit stateChanged();
    return true;
}

void
NotebookController::refreshLinkedReferences()
{
    linkedReferenceSources_.setEntries({});
    pageLinkedReferences_.total = 0;
    pageLinkedReferences_.cursor.reset();
    pageLinkedReferences_.contexts.clear();
    pageLinkedReferences_.occurrenceCursors.clear();
    if (!session_.isOpen() || !pageLinkedReferences_.targetId) {
        return;
    }
    const auto batch =
        session_.linkedReferences(*pageLinkedReferences_.targetId);
    if (!batch) {
        pageLinkedReferences_.targetId.reset();
        return;
    }
    appendIncomingSourcesBatch(batch.value(), pageLinkedReferences_,
                               linkedReferenceSources_);
}

template <typename Batch>
void
NotebookController::appendIncomingSourcesBatch(const Batch& batch,
                                               IncomingSourcesViewState& view,
                                               OutlineEntryModel& model)
{
    view.total = static_cast<qsizetype>(batch.totalSourceCount);
    view.cursor = batch.continuationCursor;
    std::vector<OutlineEntry> entries;
    entries.reserve(batch.sources.size());
    for (const auto& source : batch.sources) {
        QStringList pathParts;
        if (source.structuralPage.kind == hieda::notebook::PageKind::journal) {
            const auto date = source.structuralPage.journalDate.value_or(
                hieda::notebook::JournalDate{});
            pathParts.push_back(
                QDate(date.year, date.month, date.day).toString(Qt::ISODate));
        } else {
            pathParts.push_back(QStringLiteral("%1 — %2").arg(
                QString::fromUtf8(source.structuralPage.displayTitle),
                QString::fromUtf8(source.structuralPage.name)));
        }
        for (const auto& ancestorId : source.containmentPath) {
            if (ancestorId == source.source.metadata.id) {
                break;
            }
            const auto ancestor = std::ranges::find_if(
                source.structuralPage.entries, [&](const auto& entry) -> bool {
                    return entry.metadata.id == ancestorId;
                });
            if (ancestor != source.structuralPage.entries.end()) {
                auto label = QString::fromUtf8(ancestor->authoredText)
                                 .section('\n', 0, 0);
                if (label.size() > 40) {
                    label = label.first(37) + QStringLiteral("…");
                }
                pathParts.push_back(label);
            }
        }
        const auto group = pathParts.takeFirst();
        const auto context = pathParts.isEmpty()
                                 ? tr("Top level")
                                 : pathParts.join(QStringLiteral(" › "));
        QStringList snippets;
        for (const auto& occurrence : source.occurrences) {
            snippets.push_back(
                linkedReferenceSnippet(source.source.authoredText, occurrence));
        }
        const auto presentation = snippets.join(QStringLiteral("<br>"));
        const auto sourceId = displayId(source.source.metadata.id);
        view.contexts.insert(sourceId, context);
        if (view.targetId &&
            source.occurrenceCount > source.occurrences.size()) {
            const auto first = session_.linkedReferenceOccurrences(
                *view.targetId, source.source.metadata.id);
            if (first && first.value().continuationCursor) {
                view.occurrenceCursors.insert(
                    sourceId, *first.value().continuationCursor);
            }
        }
        entries.push_back({source.source.metadata, source.source.authoredText,
                           source.source.parentEntry, context, group,
                           presentation,
                           static_cast<qsizetype>(source.occurrenceCount),
                           source.occurrenceCount > source.occurrences.size()});
    }
    model.appendEntries(std::move(entries));
}

auto
NotebookController::loadMoreLinkedReferences() -> bool
{
    if (!pageLinkedReferences_.targetId || !pageLinkedReferences_.cursor) {
        return false;
    }
    auto batch = session_.linkedReferences(*pageLinkedReferences_.targetId,
                                           pageLinkedReferences_.cursor);
    if (!batch &&
        batch.error().code ==
            hieda::notebook::NotebookErrorCode::staleLinkedReferencesCursor) {
        refreshLinkedReferences();
        emit stateChanged();
        return true;
    }
    if (!batch) {
        reject(batch.error());
        return false;
    }
    appendIncomingSourcesBatch(batch.value(), pageLinkedReferences_,
                               linkedReferenceSources_);
    emit stateChanged();
    return true;
}

void
NotebookController::refreshBlockLinkedReferences()
{
    blockLinkedReferenceSources_.setEntries({});
    blockLinkedReferences_.total = 0;
    blockLinkedReferences_.cursor.reset();
    blockLinkedReferences_.contexts.clear();
    blockLinkedReferences_.occurrenceCursors.clear();
    if (!session_.isOpen() || !blockLinkedReferences_.targetId) {
        return;
    }
    const auto batch =
        session_.linkedReferences(*blockLinkedReferences_.targetId);
    if (!batch) {
        blockLinkedReferences_.targetId.reset();
        return;
    }
    appendIncomingSourcesBatch(batch.value(), blockLinkedReferences_,
                               blockLinkedReferenceSources_);
}

auto
NotebookController::loadMoreBlockLinkedReferences() -> bool
{
    if (!blockLinkedReferences_.targetId || !blockLinkedReferences_.cursor) {
        return false;
    }
    auto batch = session_.linkedReferences(*blockLinkedReferences_.targetId,
                                           blockLinkedReferences_.cursor);
    if (!batch &&
        batch.error().code ==
            hieda::notebook::NotebookErrorCode::staleLinkedReferencesCursor) {
        refreshBlockLinkedReferences();
        emit stateChanged();
        return true;
    }
    if (!batch) {
        reject(batch.error());
        return false;
    }
    appendIncomingSourcesBatch(batch.value(), blockLinkedReferences_,
                               blockLinkedReferenceSources_);
    emit stateChanged();
    return true;
}

auto
NotebookController::loadMoreLinkedReferenceOccurrencesFor(
    const hieda::notebook::BlockId& targetId, const QString& sourceEntryId,
    OutlineEntryModel& model, QHash<QString, std::string>& occurrenceCursors)
    -> bool
{
    const auto sourceId = blockId(sourceEntryId);
    const auto foundCursor = occurrenceCursors.find(sourceEntryId);
    if (!sourceId || foundCursor == occurrenceCursors.end()) {
        return false;
    }
    const auto batch = session_.linkedReferenceOccurrences(targetId, *sourceId,
                                                           foundCursor.value());
    if (!batch) {
        if (batch.error().code ==
            hieda::notebook::NotebookErrorCode::staleLinkedReferencesCursor) {
            refreshLinkedReferences();
            refreshBlockLinkedReferences();
            emit stateChanged();
            return true;
        }
        reject(batch.error());
        return false;
    }
    const auto row = model.rowForId(*sourceId);
    if (row < 0) {
        return false;
    }
    const auto authored = model.entryText(row).toUtf8();
    QStringList snippets;
    for (const auto& occurrence : batch.value().occurrences) {
        snippets.push_back(linkedReferenceSnippet(
            {authored.constData(), static_cast<std::size_t>(authored.size())},
            occurrence));
    }
    if (batch.value().continuationCursor) {
        foundCursor.value() = *batch.value().continuationCursor;
    } else {
        occurrenceCursors.erase(foundCursor);
    }
    model.appendLinkedReferencePresentation(
        *sourceId, snippets.join(QStringLiteral("<br>")),
        batch.value().continuationCursor.has_value());
    emit stateChanged();
    return true;
}

auto
NotebookController::loadMoreLinkedReferenceOccurrences(
    const QString& sourceEntryId) -> bool
{
    return pageLinkedReferences_.targetId &&
           loadMoreLinkedReferenceOccurrencesFor(
               *pageLinkedReferences_.targetId, sourceEntryId,
               linkedReferenceSources_,
               pageLinkedReferences_.occurrenceCursors);
}

auto
NotebookController::loadMoreBlockLinkedReferenceOccurrences(
    const QString& sourceEntryId) -> bool
{
    return blockLinkedReferences_.targetId &&
           loadMoreLinkedReferenceOccurrencesFor(
               *blockLinkedReferences_.targetId, sourceEntryId,
               blockLinkedReferenceSources_,
               blockLinkedReferences_.occurrenceCursors);
}

void
NotebookController::refreshPages()
{
    const auto result = session_.pages();
    if (!result) {
        reject(result.error());
        return;
    }
    pageChoices_.clear();
    pageIds_.clear();
    for (const auto& page : result.value()) {
        pageIds_.push_back(page.metadata.id);
        pageChoices_.push_back(
            QStringLiteral("%1 — %2").arg(QString::fromUtf8(page.displayTitle),
                                          QString::fromUtf8(page.name)));
    }
    static_cast<void>(
        pageHierarchy_.refresh(isJournalPage() ? QString{} : currentPageName_));
    emit stateChanged();
}

void
NotebookController::scheduleMidnightRefresh()
{
    const auto now = QDateTime::currentDateTime();
    const auto nextMidnight = QDateTime(now.date().addDays(1).startOfDay());
    const auto milliseconds = std::max<qint64>(1, now.msecsTo(nextMidnight));
    midnightTimer_.start(static_cast<int>(
        std::min<qint64>(milliseconds, std::numeric_limits<int>::max())));
}
