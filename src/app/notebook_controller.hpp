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

class JournalEntryModel final : public QAbstractListModel {
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

    explicit JournalEntryModel(QObject* parent = nullptr);
    [[nodiscard]] auto rowCount(const QModelIndex& parent = {}) const -> int override;
    [[nodiscard]] auto data(const QModelIndex& index, int role) const -> QVariant override;
    [[nodiscard]] auto roleNames() const -> QHash<int, QByteArray> override;
    void setEntries(std::vector<hieda::notebook::JournalEntry> entries);
    void insertEntry(int row, hieda::notebook::JournalEntry entry);
    void updateEntry(const hieda::notebook::JournalEntry& entry);
    [[nodiscard]] auto entryId(int row) const -> QString;
    [[nodiscard]] auto entryText(int row) const -> QString;
    [[nodiscard]] auto entryParentId(int row) const -> QString;
    [[nodiscard]] auto entryDepth(int row) const -> int;
    [[nodiscard]] auto subtreeEnd(int row) const -> int;
    [[nodiscard]] auto rowForId(const hieda::notebook::BlockId& identifier) const -> int;

  private:
    std::vector<hieda::notebook::JournalEntry> entries_;
};

class NotebookController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool hasOpenNotebook READ hasOpenNotebook NOTIFY stateChanged)
    Q_PROPERTY(QString notebookPath READ notebookPath NOTIFY stateChanged)
    Q_PROPERTY(QString notebookName READ notebookName NOTIFY stateChanged)
    Q_PROPERTY(QString errorMessage READ errorMessage NOTIFY stateChanged)
    Q_PROPERTY(QDate journalDate READ journalDate NOTIFY journalChanged)
    Q_PROPERTY(QAbstractItemModel* journalEntries READ journalEntries CONSTANT)
    Q_PROPERTY(bool canUndo READ canUndo NOTIFY stateChanged)
    Q_PROPERTY(bool canRedo READ canRedo NOTIFY stateChanged)
    Q_PROPERTY(bool isJournalPage READ isJournalPage NOTIFY journalChanged)
    Q_PROPERTY(QString currentPageId READ currentPageId NOTIFY journalChanged)
    Q_PROPERTY(QString currentPageName READ currentPageName NOTIFY journalChanged)
    Q_PROPERTY(QString currentPageTitle READ currentPageTitle NOTIFY journalChanged)
    Q_PROPERTY(QStringList pageChoices READ pageChoices NOTIFY stateChanged)

  public:
    explicit NotebookController(QObject* parent = nullptr);

    [[nodiscard]] auto hasOpenNotebook() const -> bool;
    [[nodiscard]] auto notebookPath() const -> QString;
    [[nodiscard]] auto notebookName() const -> QString;
    [[nodiscard]] auto errorMessage() const -> QString;
    [[nodiscard]] auto journalDate() const -> QDate;
    [[nodiscard]] auto journalEntries() -> QAbstractItemModel*;
    [[nodiscard]] auto canUndo() const -> bool;
    [[nodiscard]] auto canRedo() const -> bool;
    [[nodiscard]] auto isJournalPage() const -> bool;
    [[nodiscard]] auto currentPageId() const -> QString;
    [[nodiscard]] auto currentPageName() const -> QString;
    [[nodiscard]] auto currentPageTitle() const -> QString;
    [[nodiscard]] auto pageChoices() const -> QStringList;
    [[nodiscard]] Q_INVOKABLE auto pageIdAt(int index) const -> QString;

    Q_INVOKABLE void createNotebook(const QUrl& url);
    Q_INVOKABLE void openNotebook(const QUrl& url);
    Q_INVOKABLE void closeNotebook();
    Q_INVOKABLE void clearError();
    Q_INVOKABLE auto createPage(const QString& name, const QString& displayTitle) -> bool;
    Q_INVOKABLE auto renameCurrentPage(const QString& name, const QString& displayTitle) -> bool;
    Q_INVOKABLE void navigateToPage(const QString& pageId);
    Q_INVOKABLE void navigateToJournalDate(const QDate& date);
    Q_INVOKABLE void navigateToJournalDateText(const QString& isoDate);
    Q_INVOKABLE void navigateToToday();
    Q_INVOKABLE void navigateToPreviousJournalDate();
    Q_INVOKABLE void navigateToNextJournalDate();
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
    Q_INVOKABLE auto insertJournalEntry(const QString& authoredText,
                                        const QString& afterEntryId = {}) -> int;
    [[nodiscard]] Q_INVOKABLE auto journalEntryId(int row) const -> QString;
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
    Q_INVOKABLE auto updateJournalEntry(const QString& entryId, const QString& authoredText)
        -> bool;
    Q_INVOKABLE auto splitJournalEntry(const QString& entryId, const QString& authoredText,
                                       int cursorPosition) -> QVariantMap;
    Q_INVOKABLE auto joinJournalEntry(const QString& entryId, const QString& authoredText)
        -> QVariantMap;
    Q_INVOKABLE auto indentJournalEntry(const QString& entryId, const QString& authoredText,
                                        int cursorPosition) -> QVariantMap;
    Q_INVOKABLE auto outdentJournalEntry(const QString& entryId, const QString& authoredText,
                                         int cursorPosition) -> QVariantMap;
    Q_INVOKABLE auto moveJournalEntryUp(const QString& entryId, const QString& authoredText,
                                        int cursorPosition) -> QVariantMap;
    Q_INVOKABLE auto moveJournalEntryDown(const QString& entryId, const QString& authoredText,
                                          int cursorPosition) -> QVariantMap;
    Q_INVOKABLE auto deleteJournalEntry(const QString& entryId) -> QVariantMap;
    [[nodiscard]] Q_INVOKABLE auto journalSelectionText(const QStringList& entryIds) const
        -> QString;
    [[nodiscard]] Q_INVOKABLE auto journalEntrySelection(int anchorRow, int extentRow) const
        -> QVariantMap;
    Q_INVOKABLE static void copyTextToClipboard(const QString& text);
    Q_INVOKABLE auto deleteJournalSubtrees(const QStringList& entryIds) -> QVariantMap;
    Q_INVOKABLE auto undoJournalEdit(const QString& preferredEntryId = {}, int cursorPosition = 0)
        -> QVariantMap;
    Q_INVOKABLE auto redoJournalEdit(const QString& preferredEntryId = {}, int cursorPosition = 0)
        -> QVariantMap;
    void requestJournalDateRollover(const QDate& date);
    Q_INVOKABLE void completeJournalDateRollover();

  signals:
    void stateChanged();
    void journalChanged();
    void journalDateRolloverRequested();

  protected:
    auto eventFilter(QObject* watched, QEvent* event) -> bool override;

  private:
    enum class JournalHistoryDirection : std::uint8_t { undo, redo };

    void accept(const hieda::notebook::NotebookInfo& info);
    void reject(const hieda::notebook::NotebookError& error);
    void rejectSave(const hieda::notebook::NotebookError& error);
    void loadJournalDate(const QDate& date);
    void loadPage(const hieda::notebook::BlockId& pageId);
    void refreshPages();
    auto moveJournalEntry(const QString& entryId, const QString& authoredText,
                          hieda::notebook::JournalEntryMove movement, int cursorPosition)
        -> QVariantMap;
    auto applyJournalHistory(JournalHistoryDirection direction, const QString& preferredEntryId,
                             int cursorPosition) -> QVariantMap;
    void scheduleMidnightRefresh();

    hieda::notebook::NotebookSession session_;
    QString path_;
    QString name_;
    QString error_;
    QDate journalDate_;
    QDate pendingJournalDate_;
    std::optional<hieda::notebook::BlockId> currentPageId_;
    QString currentPageName_;
    QString currentPageTitle_;
    QStringList pageChoices_;
    std::vector<hieda::notebook::BlockId> pageIds_;
    JournalEntryModel journalEntries_;
    QTimer midnightTimer_;
};
