// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "hieda/notebook/notebook_session.hpp"

#include <QAbstractListModel>
#include <QDate>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QUrl>
#include <QVariantMap>

struct OutlineEntry {
    hieda::notebook::BlockMetadata metadata;
    std::string authoredText;
    std::optional<hieda::notebook::BlockId> parentEntry;
};

class OutlineEntryModel final : public QAbstractListModel {
    Q_OBJECT

  public:
    enum Role : std::uint16_t {
        EntryIdRole = Qt::UserRole + 1,
        AuthoredTextRole,
        ParentEntryIdRole,
        DepthRole,
        HasChildrenRole,
        CanIndentRole,
        CanOutdentRole,
        CanMoveUpRole,
        CanMoveDownRole,
        CanDeleteRole,
    };

    explicit OutlineEntryModel(QObject* parent = nullptr);
    [[nodiscard]] auto rowCount(const QModelIndex& parent = {}) const -> int override;
    [[nodiscard]] auto data(const QModelIndex& index, int role) const -> QVariant override;
    [[nodiscard]] auto roleNames() const -> QHash<int, QByteArray> override;
    void setEntries(std::vector<OutlineEntry> entries);
    void insertEntry(int row, OutlineEntry entry);
    void updateEntry(const OutlineEntry& entry);
    [[nodiscard]] auto entryId(int row) const -> QString;
    [[nodiscard]] auto entryText(int row) const -> QString;
    [[nodiscard]] auto entryParentId(int row) const -> QString;
    [[nodiscard]] auto entryDepth(int row) const -> int;
    [[nodiscard]] auto subtreeEnd(int row) const -> int;
    [[nodiscard]] auto rowForId(const hieda::notebook::BlockId& identifier) const -> int;

  private:
    std::vector<OutlineEntry> entries_;
};

class PageHierarchyModel final : public QAbstractItemModel {
    Q_OBJECT

  public:
    enum Role : std::uint16_t {
        PageNameRole = Qt::UserRole + 1,
        LocalSegmentRole,
        DisplayTitleRole,
        MaterializedRole,
        HasChildrenRole,
        ExpandedRole,
        SelectedRole,
        AccessibleDescriptionRole,
    };

    explicit PageHierarchyModel(QObject* parent = nullptr);
    [[nodiscard]] auto index(int row, int column, const QModelIndex& parent = {}) const
        -> QModelIndex override;
    [[nodiscard]] auto parent(const QModelIndex& child) const -> QModelIndex override;
    [[nodiscard]] auto rowCount(const QModelIndex& parent = {}) const -> int override;
    [[nodiscard]] auto hasChildren(const QModelIndex& parent = {}) const -> bool override;
    [[nodiscard]] auto columnCount(const QModelIndex& parent = {}) const -> int override;
    [[nodiscard]] auto data(const QModelIndex& index, int role) const -> QVariant override;
    [[nodiscard]] auto roleNames() const -> QHash<int, QByteArray> override;
    [[nodiscard]] auto canFetchMore(const QModelIndex& parent) const -> bool override;
    void fetchMore(const QModelIndex& parent) override;
    void attach(hieda::notebook::NotebookSession* session);
    void clear();
    [[nodiscard]] auto refresh(const QString& currentName) -> bool;
    [[nodiscard]] Q_INVOKABLE QModelIndex indexForPageName(const QString& pageName) const;
    [[nodiscard]] Q_INVOKABLE static QString pageName(const QModelIndex& index);

  private:
    struct Node {
        hieda::notebook::PageHierarchyNode node;
        Node* parent{nullptr};
        std::vector<std::unique_ptr<Node>> children;
        std::optional<std::string> continuationCursor;
        bool loaded{false};
    };

    [[nodiscard]] static auto node(const QModelIndex& index) -> Node*;
    [[nodiscard]] auto children(Node* parent) -> std::vector<std::unique_ptr<Node>>&;
    [[nodiscard]] auto children(const Node* parent) const
        -> const std::vector<std::unique_ptr<Node>>&;
    [[nodiscard]] auto loadNextBatch(Node* parent) -> bool;
    [[nodiscard]] auto loadCurrentPath() -> bool;
    [[nodiscard]] auto findNode(std::string_view pageName) const -> Node*;

    hieda::notebook::NotebookSession* session_{nullptr};
    std::vector<std::unique_ptr<Node>> roots_;
    std::optional<std::string> rootContinuationCursor_;
    bool rootsLoaded_{false};
    std::string currentName_;
};

class NotebookController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool hasOpenNotebook READ hasOpenNotebook NOTIFY stateChanged)
    Q_PROPERTY(QString notebookPath READ notebookPath NOTIFY stateChanged)
    Q_PROPERTY(QString notebookName READ notebookName NOTIFY stateChanged)
    Q_PROPERTY(QString errorMessage READ errorMessage NOTIFY stateChanged)
    Q_PROPERTY(QDate journalDate READ journalDate NOTIFY destinationChanged)
    Q_PROPERTY(QAbstractItemModel* outlineEntries READ outlineEntries CONSTANT)
    Q_PROPERTY(QAbstractItemModel* pageHierarchy READ pageHierarchy CONSTANT)
    Q_PROPERTY(bool canUndo READ canUndo NOTIFY stateChanged)
    Q_PROPERTY(bool canRedo READ canRedo NOTIFY stateChanged)
    Q_PROPERTY(bool isJournalPage READ isJournalPage NOTIFY destinationChanged)
    Q_PROPERTY(QString currentPageId READ currentPageId NOTIFY destinationChanged)
    Q_PROPERTY(QString currentPageName READ currentPageName NOTIFY destinationChanged)
    Q_PROPERTY(QString currentPageTitle READ currentPageTitle NOTIFY destinationChanged)
    Q_PROPERTY(bool currentPagePreview READ currentPagePreview NOTIFY destinationChanged)
    Q_PROPERTY(QStringList pageChoices READ pageChoices NOTIFY stateChanged)

  public:
    explicit NotebookController(QObject* parent = nullptr);

    [[nodiscard]] auto hasOpenNotebook() const -> bool;
    [[nodiscard]] auto notebookPath() const -> QString;
    [[nodiscard]] auto notebookName() const -> QString;
    [[nodiscard]] auto errorMessage() const -> QString;
    [[nodiscard]] auto journalDate() const -> QDate;
    [[nodiscard]] auto outlineEntries() -> QAbstractItemModel*;
    [[nodiscard]] auto pageHierarchy() -> QAbstractItemModel*;
    [[nodiscard]] auto canUndo() const -> bool;
    [[nodiscard]] auto canRedo() const -> bool;
    [[nodiscard]] auto isJournalPage() const -> bool;
    [[nodiscard]] auto currentPageId() const -> QString;
    [[nodiscard]] auto currentPageName() const -> QString;
    [[nodiscard]] auto currentPageTitle() const -> QString;
    [[nodiscard]] auto currentPagePreview() const -> bool;
    [[nodiscard]] auto pageChoices() const -> QStringList;
    [[nodiscard]] Q_INVOKABLE QString pageIdAt(qsizetype index) const;
    [[nodiscard]] Q_INVOKABLE QString pageIdForChoice(const QString& choice) const;

    Q_INVOKABLE void createNotebook(const QUrl& url);
    Q_INVOKABLE void openNotebook(const QUrl& url);
    Q_INVOKABLE void closeNotebook();
    Q_INVOKABLE void clearError();
    Q_INVOKABLE bool createPage(const QString& name, const QString& displayTitle);
    Q_INVOKABLE bool createCurrentPage(const QString& displayTitle);
    Q_INVOKABLE bool deleteCurrentPage();
    Q_INVOKABLE bool renameCurrentPage(const QString& name, const QString& displayTitle);
    Q_INVOKABLE void navigateToPage(const QString& pageId);
    Q_INVOKABLE void navigateToPageName(const QString& pageName);
    Q_INVOKABLE bool followPageLink(const QString& entryId, int characterOffset,
                                    const QString& editorText);
    Q_INVOKABLE void navigateToJournalDate(const QDate& date);
    Q_INVOKABLE void navigateToJournalDateText(const QString& isoDate);
    Q_INVOKABLE void navigateToToday();
    Q_INVOKABLE void navigateToPreviousJournalDate();
    Q_INVOKABLE void navigateToNextJournalDate();
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
    Q_INVOKABLE int insertOutlineEntry(const QString& authoredText,
                                       const QString& afterEntryId = {});
    [[nodiscard]] Q_INVOKABLE QString outlineEntryId(int row) const;
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
    [[nodiscard]] Q_INVOKABLE QString committedEntryPresentation(const QString& entryId,
                                                                 const QString& authoredText) const;
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
    Q_INVOKABLE bool updateOutlineEntry(const QString& entryId, const QString& authoredText);
    Q_INVOKABLE QVariantMap splitOutlineEntry(const QString& entryId, const QString& authoredText,
                                              int cursorPosition);
    Q_INVOKABLE QVariantMap joinOutlineEntry(const QString& entryId, const QString& authoredText);
    Q_INVOKABLE QVariantMap indentOutlineEntry(const QString& entryId, const QString& authoredText,
                                               int cursorPosition);
    Q_INVOKABLE QVariantMap outdentOutlineEntry(const QString& entryId, const QString& authoredText,
                                                int cursorPosition);
    Q_INVOKABLE QVariantMap moveOutlineEntryUp(const QString& entryId, const QString& authoredText,
                                               int cursorPosition);
    Q_INVOKABLE QVariantMap moveOutlineEntryDown(const QString& entryId,
                                                 const QString& authoredText, int cursorPosition);
    Q_INVOKABLE QVariantMap deleteOutlineEntry(const QString& entryId);
    [[nodiscard]] Q_INVOKABLE QString outlineSelectionText(const QStringList& entryIds) const;
    [[nodiscard]] Q_INVOKABLE QVariantMap outlineEntrySelection(int anchorRow, int extentRow) const;
    Q_INVOKABLE static void copyTextToClipboard(const QString& text);
    Q_INVOKABLE QVariantMap deleteOutlineSubtrees(const QStringList& entryIds);
    Q_INVOKABLE QVariantMap undoOutlineEdit(const QString& preferredEntryId = {},
                                            int cursorPosition = 0);
    Q_INVOKABLE QVariantMap redoOutlineEdit(const QString& preferredEntryId = {},
                                            int cursorPosition = 0);
    void requestJournalDateRollover(const QDate& date);
    Q_INVOKABLE void completeJournalDateRollover();

  signals:
    void stateChanged();
    void destinationChanged();
    void journalDateRolloverRequested();

  protected:
    auto eventFilter(QObject* watched, QEvent* event) -> bool override;

  private:
    enum class OutlineHistoryDirection : std::uint8_t { undo, redo };
    enum class OutlineEntryMove : std::uint8_t { indent, outdent, up, down };

    void accept(const hieda::notebook::NotebookInfo& info);
    void reject(const hieda::notebook::NotebookError& error);
    void rejectSave(const hieda::notebook::NotebookError& error);
    void loadJournalDate(const QDate& date);
    void loadPage(const hieda::notebook::BlockId& pageId);
    void loadPagePreview(const QString& pageName);
    [[nodiscard]] auto currentPageAddress() const -> hieda::notebook::PageAddress;
    void refreshPages();
    auto moveOutlineEntry(const QString& entryId, const QString& authoredText,
                          OutlineEntryMove movement, int cursorPosition) -> QVariantMap;
    auto applyOutlineHistory(OutlineHistoryDirection direction, const QString& preferredEntryId,
                             int cursorPosition) -> QVariantMap;
    void scheduleMidnightRefresh();

    hieda::notebook::NotebookSession session_;
    QString path_;
    QString name_;
    QString error_;
    QDate journalDate_;
    QDate pendingJournalDate_;
    std::optional<hieda::notebook::BlockId> currentPageId_;
    bool currentPagePreview_{false};
    QString currentPageName_;
    QString currentPageTitle_;
    QStringList pageChoices_;
    std::vector<hieda::notebook::BlockId> pageIds_;
    OutlineEntryModel outlineEntries_;
    PageHierarchyModel pageHierarchy_;
    QTimer midnightTimer_;
};
