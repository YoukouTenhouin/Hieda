// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "hieda/notebook/notebook_session.hpp"

#include <QObject>
#include <QString>
#include <QUrl>

class NotebookController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool hasOpenNotebook READ hasOpenNotebook NOTIFY stateChanged)
    Q_PROPERTY(QString notebookPath READ notebookPath NOTIFY stateChanged)
    Q_PROPERTY(QString notebookName READ notebookName NOTIFY stateChanged)
    Q_PROPERTY(QString errorMessage READ errorMessage NOTIFY stateChanged)

  public:
    explicit NotebookController(QObject* parent = nullptr);

    [[nodiscard]] auto hasOpenNotebook() const -> bool;
    [[nodiscard]] auto notebookPath() const -> QString;
    [[nodiscard]] auto notebookName() const -> QString;
    [[nodiscard]] auto errorMessage() const -> QString;

    Q_INVOKABLE void createNotebook(const QUrl& url);
    Q_INVOKABLE void openNotebook(const QUrl& url);
    Q_INVOKABLE void closeNotebook();
    Q_INVOKABLE void clearError();

  signals:
    void stateChanged();

  private:
    void accept(const hieda::notebook::NotebookInfo& info);
    void reject(const hieda::notebook::NotebookError& error);

    hieda::notebook::NotebookSession session_;
    QString path_;
    QString name_;
    QString error_;
};
