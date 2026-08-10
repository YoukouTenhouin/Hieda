// SPDX-License-Identifier: MPL-2.0
#include "notebook_controller.hpp"

#include <QApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QTemporaryDir>
#include <QTimer>
#include <QUrl>

#include <optional>

auto
main(int argc, char* argv[]) -> int
{
    QApplication application(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("Hieda"));
    QCoreApplication::setOrganizationName(QStringLiteral("Hieda"));

    NotebookController controller;
    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(
        QStringLiteral("notebookController"), &controller);
    engine.loadFromModule(QStringLiteral("Hieda"), QStringLiteral("Main"));
    if (engine.rootObjects().isEmpty()) {
        return 1;
    }

    std::optional<QTemporaryDir> smokeDirectory;
    if (QCoreApplication::arguments().contains(
            QStringLiteral("--smoke-test"))) {
        smokeDirectory.emplace();
        const auto path =
            smokeDirectory->filePath(QStringLiteral("hierarchy-smoke.hieda"));
        controller.createNotebook(QUrl::fromLocalFile(path));
        const auto created = controller.createPage(
            QStringLiteral("work/client/alpha"), QStringLiteral("Alpha"));
        controller.navigateToPageName(QStringLiteral("work/client"));
        const auto openedPreview = controller.currentPagePreview();
        const auto materialized =
            controller.createCurrentPage(QStringLiteral("Client"));
        const auto deleted = controller.deleteCurrentPage();
        const auto undone = controller.undoOutlineEdit()
                                .value(QStringLiteral("succeeded"))
                                .toBool();
        const auto restored = !controller.currentPagePreview();
        const auto redone = controller.redoOutlineEdit()
                                .value(QStringLiteral("succeeded"))
                                .toBool();
        const auto succeeded = smokeDirectory->isValid() && created &&
                               openedPreview && materialized && deleted &&
                               undone && restored && redone &&
                               controller.currentPagePreview() &&
                               controller.pageHierarchy()->rowCount() == 1;
        QTimer::singleShot(0, &application,
                           [&application, succeeded]() -> void {
                               application.exit(succeeded ? 0 : 1);
                           });
    }
    return QCoreApplication::exec();
}
