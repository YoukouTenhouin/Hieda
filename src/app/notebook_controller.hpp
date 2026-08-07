// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "hieda/notebook/notebook_session.hpp"

#include <QAbstractListModel>
#include <QDate>
#include <QObject>
#include <QString>
#include <QTimer>
#include <QUrl>

class JournalEntryModel final : public QAbstractListModel {
    Q_OBJECT

  public:
    enum Role : std::uint16_t { EntryIdRole = Qt::UserRole + 1, AuthoredTextRole };

    explicit JournalEntryModel(QObject* parent = nullptr);
    [[nodiscard]] auto rowCount(const QModelIndex& parent = {}) const -> int override;
    [[nodiscard]] auto data(const QModelIndex& index, int role) const -> QVariant override;
    [[nodiscard]] auto roleNames() const -> QHash<int, QByteArray> override;
    void setEntries(std::vector<hieda::notebook::JournalEntry> entries);
    void updateEntry(const hieda::notebook::JournalEntry& entry);
    [[nodiscard]] auto entryId(int row) const -> QString;

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

  public:
    explicit NotebookController(QObject* parent = nullptr);

    [[nodiscard]] auto hasOpenNotebook() const -> bool;
    [[nodiscard]] auto notebookPath() const -> QString;
    [[nodiscard]] auto notebookName() const -> QString;
    [[nodiscard]] auto errorMessage() const -> QString;
    [[nodiscard]] auto journalDate() const -> QDate;
    [[nodiscard]] auto journalEntries() -> QAbstractItemModel*;

    Q_INVOKABLE void createNotebook(const QUrl& url);
    Q_INVOKABLE void openNotebook(const QUrl& url);
    Q_INVOKABLE void closeNotebook();
    Q_INVOKABLE void clearError();
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
    Q_INVOKABLE auto insertJournalEntry(const QString& authoredText,
                                        const QString& afterEntryId = {}) -> int;
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
    Q_INVOKABLE auto updateJournalEntry(const QString& entryId, const QString& authoredText)
        -> bool;
    void requestJournalDateRollover(const QDate& date);
    Q_INVOKABLE void completeJournalDateRollover();

  signals:
    void stateChanged();
    void journalChanged();
    void journalDateRolloverRequested();

  protected:
    auto eventFilter(QObject* watched, QEvent* event) -> bool override;

  private:
    void accept(const hieda::notebook::NotebookInfo& info);
    void reject(const hieda::notebook::NotebookError& error);
    void rejectSave(const hieda::notebook::NotebookError& error);
    void loadJournalDate(const QDate& date);
    void scheduleMidnightRefresh();

    hieda::notebook::NotebookSession session_;
    QString path_;
    QString name_;
    QString error_;
    QDate journalDate_;
    QDate pendingJournalDate_;
    JournalEntryModel journalEntries_;
    QTimer midnightTimer_;
};
