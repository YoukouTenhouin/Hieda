// SPDX-License-Identifier: MPL-2.0
#include "notebook_controller.hpp"

#include <catch2/catch_session.hpp>
#include <catch2/catch_test_macros.hpp>

#include <QApplication>
#include <QElapsedTimer>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QUrl>
#include <QVariant>

#include <chrono>
#include <memory>

namespace {

template <typename Predicate>
auto waitUntil(Predicate predicate, std::chrono::milliseconds timeout = std::chrono::seconds(1))
    -> bool {
    QElapsedTimer timer;
    timer.start();
    while (!predicate() && timer.elapsed() < timeout.count()) {
        QCoreApplication::processEvents();
        QTest::qWait(5);
    }
    return predicate();
}

} // namespace

TEST_CASE("the Journal editor keeps aligned drafts focused without QML warnings") {
    QTemporaryDir temporaryDirectory;
    REQUIRE(temporaryDirectory.isValid());
    NotebookController controller;
    controller.createNotebook(
        QUrl::fromLocalFile(temporaryDirectory.filePath(QStringLiteral("ui-test.hieda"))));
    REQUIRE(controller.insertJournalEntry(QString(400, QLatin1Char('x'))) == 0);
    REQUIRE(controller.insertJournalEntry(QStringLiteral("second")) == 1);

    QQmlEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("notebookController"), &controller);
    QQmlComponent component(&engine,
                            QUrl::fromLocalFile(QStringLiteral(HIEDA_SOURCE_DIR "/qml/Main.qml")));
    std::unique_ptr<QObject> root(component.create());
    INFO(component.errorString().toStdString());
    REQUIRE(root != nullptr);
    auto* window = qobject_cast<QQuickWindow*>(root.get());
    REQUIRE(window != nullptr);
    window->setWidth(1600);
    window->setHeight(800);
    window->show();
    window->requestActivate();
    REQUIRE(waitUntil([window]() -> bool { return window->isActive(); }));

    auto* journalList = root->findChild<QQuickItem*>(QStringLiteral("journalList"));
    REQUIRE(journalList != nullptr);
    REQUIRE(
        waitUntil([journalList]() -> bool { return journalList->property("count").toInt() == 2; }));
    QVariant firstEntryValue;
    REQUIRE(QMetaObject::invokeMethod(journalList, "entryItemAt",
                                      Q_RETURN_ARG(QVariant, firstEntryValue), Q_ARG(QVariant, 0)));
    auto* firstEntry = qobject_cast<QQuickItem*>(firstEntryValue.value<QObject*>());
    REQUIRE(firstEntry != nullptr);
    auto* editor = firstEntry->findChild<QQuickItem*>(QStringLiteral("journalEntryEditor-0"));
    REQUIRE(editor != nullptr);
    REQUIRE(editor->window() == window);
    REQUIRE(editor->isVisible());
    REQUIRE(editor->isEnabled());
    REQUIRE(waitUntil([journalList]() -> bool { return journalList->width() <= 780; }));
    const auto listPosition = journalList->mapToScene({});
    CHECK(listPosition.x() >= 400);
    CHECK(firstEntry->width() <= journalList->width());
    const auto editorPosition = editor->mapToScene({});
    CHECK(editorPosition.x() >= listPosition.x());
    CHECK(editorPosition.x() + editor->width() <= listPosition.x() + journalList->width());
    CHECK(editor->height() > 18);

    QSignalSpy qmlWarnings(&engine, &QQmlEngine::warnings);
    editor->forceActiveFocus();
    REQUIRE(waitUntil([editor]() -> bool { return editor->hasActiveFocus(); }));
    QTest::keyClick(window, Qt::Key_Return);

    REQUIRE(waitUntil([firstEntry]() -> bool {
        return firstEntry->findChild<QQuickItem*>(QStringLiteral("journalDraftEditor")) != nullptr;
    }));
    auto* draftEditor = firstEntry->findChild<QQuickItem*>(QStringLiteral("journalDraftEditor"));
    REQUIRE(draftEditor != nullptr);
    REQUIRE(waitUntil([draftEditor]() -> bool { return draftEditor->hasActiveFocus(); }));
    QTest::qWait(50);
    CHECK(firstEntry->findChild<QQuickItem*>(QStringLiteral("journalDraftEditor")) == draftEditor);

    auto* entryBullet = firstEntry->findChild<QQuickItem*>(QStringLiteral("journalEntryBullet-0"));
    auto* draftBullet = firstEntry->findChild<QQuickItem*>(QStringLiteral("journalDraftBullet"));
    REQUIRE(entryBullet != nullptr);
    REQUIRE(draftBullet != nullptr);
    CHECK(draftBullet->mapToScene({}).x() == entryBullet->mapToScene({}).x());

    QTest::keyClick(window, Qt::Key_Escape);
    QVariant secondEntryValue;
    REQUIRE(QMetaObject::invokeMethod(
        journalList, "entryItemAt", Q_RETURN_ARG(QVariant, secondEntryValue), Q_ARG(QVariant, 1)));
    auto* secondEntry = qobject_cast<QQuickItem*>(secondEntryValue.value<QObject*>());
    REQUIRE(secondEntry != nullptr);
    auto* secondEditor =
        secondEntry->findChild<QQuickItem*>(QStringLiteral("journalEntryEditor-1"));
    REQUIRE(secondEditor != nullptr);
    secondEditor->forceActiveFocus();
    REQUIRE(waitUntil([secondEditor]() -> bool { return secondEditor->hasActiveFocus(); }));
    QTest::keyClick(window, Qt::Key_Return);
    REQUIRE(waitUntil([window]() -> bool {
        return window->activeFocusItem() != nullptr &&
               window->activeFocusItem()->objectName() == QStringLiteral("journalDraftEditor");
    }));
    draftEditor = window->activeFocusItem();
    QTest::qWait(50);
    CHECK(window->activeFocusItem() == draftEditor);
    draftBullet = draftEditor->parentItem()->parentItem()->findChild<QQuickItem*>(
        QStringLiteral("journalDraftBullet"));
    auto* secondBullet =
        secondEntry->findChild<QQuickItem*>(QStringLiteral("journalEntryBullet-1"));
    REQUIRE(draftBullet != nullptr);
    REQUIRE(secondBullet != nullptr);
    CHECK(draftBullet->mapToScene({}).x() == secondBullet->mapToScene({}).x());

    QTest::keyClick(window, Qt::Key_N);
    QTest::keyClick(window, Qt::Key_E);
    QTest::keyClick(window, Qt::Key_W);
    REQUIRE(QMetaObject::invokeMethod(root.get(), "beginTrailingDraft"));
    CHECK(qmlWarnings.count() == 0);
}

auto main(int argc, char* argv[]) -> int {
    QApplication application(argc, argv);
    return Catch::Session().run(argc, argv);
}
