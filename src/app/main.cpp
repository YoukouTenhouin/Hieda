// SPDX-License-Identifier: MPL-2.0
#include "notebook_controller.hpp"

#include <QApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QTimer>

auto main(int argc, char* argv[]) -> int {
    QApplication application(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("Hieda"));
    QCoreApplication::setOrganizationName(QStringLiteral("Hieda"));

    NotebookController controller;
    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("notebookController"), &controller);
    engine.loadFromModule(QStringLiteral("Hieda"), QStringLiteral("Main"));
    if (engine.rootObjects().isEmpty()) {
        return 1;
    }

    if (QCoreApplication::arguments().contains(QStringLiteral("--smoke-test"))) {
        QTimer::singleShot(0, &application, &QCoreApplication::quit);
    }
    return QCoreApplication::exec();
}
