// SPDX-License-Identifier: MPL-2.0
#include "notebook_controller.hpp"

#include <QFileInfo>

NotebookController::NotebookController(QObject* parent) : QObject(parent) {}

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

void NotebookController::createNotebook(const QUrl& url) {
    const auto path = url.toLocalFile().toStdString();
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
    const auto path = url.toLocalFile().toStdString();
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
    emit stateChanged();
}

void NotebookController::clearError() {
    if (!error_.isEmpty()) {
        error_.clear();
        emit stateChanged();
    }
}

void NotebookController::accept(const hieda::notebook::NotebookInfo& info) {
    path_ = QString::fromStdString(info.path.string());
    name_ = QFileInfo(path_).completeBaseName();
    error_.clear();
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
    }
    emit stateChanged();
}
