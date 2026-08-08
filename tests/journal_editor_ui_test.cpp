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
    REQUIRE(QMetaObject::invokeMethod(root.get(), "beginDraft",
                                      Q_ARG(QVariant, controller.journalEntryId(0))));

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
    REQUIRE(QMetaObject::invokeMethod(root.get(), "beginDraft",
                                      Q_ARG(QVariant, controller.journalEntryId(1))));
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

TEST_CASE("the Journal editor routes nested outline keys and exposes pointer actions") {
    QTemporaryDir temporaryDirectory;
    REQUIRE(temporaryDirectory.isValid());
    NotebookController controller;
    controller.createNotebook(
        QUrl::fromLocalFile(temporaryDirectory.filePath(QStringLiteral("outline-ui.hieda"))));
    REQUIRE(controller.insertJournalEntry(QStringLiteral("parent")) == 0);
    REQUIRE(controller.insertJournalEntry(QStringLiteral("child")) == 1);
    REQUIRE(controller.insertJournalEntry(QStringLiteral("tail")) == 2);
    const auto childId = controller.journalEntryId(1);

    QQmlEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("notebookController"), &controller);
    QQmlComponent component(&engine,
                            QUrl::fromLocalFile(QStringLiteral(HIEDA_SOURCE_DIR "/qml/Main.qml")));
    std::unique_ptr<QObject> root(component.create());
    INFO(component.errorString().toStdString());
    REQUIRE(root != nullptr);
    auto* window = qobject_cast<QQuickWindow*>(root.get());
    REQUIRE(window != nullptr);
    window->show();
    window->requestActivate();
    REQUIRE(waitUntil([window]() -> bool { return window->isActive(); }));
    auto* journalList = root->findChild<QQuickItem*>(QStringLiteral("journalList"));
    REQUIRE(journalList != nullptr);
    REQUIRE(
        waitUntil([journalList]() -> bool { return journalList->property("count").toInt() == 3; }));

    const auto entryAt = [journalList](int row) -> QQuickItem* {
        QVariant value;
        if (!QMetaObject::invokeMethod(journalList, "entryItemAt", Q_RETURN_ARG(QVariant, value),
                                       Q_ARG(QVariant, row))) {
            return nullptr;
        }
        return qobject_cast<QQuickItem*>(value.value<QObject*>());
    };
    auto* parentEntry = entryAt(0);
    auto* childEntry = entryAt(1);
    REQUIRE(parentEntry != nullptr);
    REQUIRE(childEntry != nullptr);
    auto* childEditor = childEntry->findChild<QQuickItem*>(QStringLiteral("journalEntryEditor-1"));
    REQUIRE(childEditor != nullptr);
    childEditor->forceActiveFocus();
    REQUIRE(waitUntil([childEditor]() -> bool { return childEditor->hasActiveFocus(); }));
    QTest::keyClick(window, Qt::Key_Tab);
    REQUIRE(waitUntil([&controller]() -> bool {
        return controller.journalEntries()
                   ->data(controller.journalEntries()->index(1, 0), JournalEntryModel::DepthRole)
                   .toInt() == 1;
    }));

    parentEntry = entryAt(0);
    childEntry = entryAt(1);
    REQUIRE(parentEntry != nullptr);
    REQUIRE(childEntry != nullptr);
    auto* parentEditor =
        parentEntry->findChild<QQuickItem*>(QStringLiteral("journalEntryEditor-0"));
    childEditor = childEntry->findChild<QQuickItem*>(QStringLiteral("journalEntryEditor-1"));
    REQUIRE(parentEditor != nullptr);
    REQUIRE(childEditor != nullptr);
    CHECK(childEditor->mapToScene({}).x() > parentEditor->mapToScene({}).x());
    CHECK(childEntry->property("contextMenu").value<QObject*>() != nullptr);

    childEditor->forceActiveFocus();
    childEditor->setProperty("cursorPosition", 2);
    QTest::keyClick(window, Qt::Key_Return);
    REQUIRE(
        waitUntil([journalList]() -> bool { return journalList->property("count").toInt() == 4; }));
    REQUIRE(waitUntil([window]() -> bool {
        return window->activeFocusItem() != nullptr &&
               window->activeFocusItem()->objectName() == QStringLiteral("journalEntryEditor-2");
    }));
    auto* suffixEditor = window->activeFocusItem();
    CHECK(suffixEditor->property("cursorPosition").toInt() == 0);
    QTest::keyClick(window, Qt::Key_Backspace);
    REQUIRE(
        waitUntil([journalList]() -> bool { return journalList->property("count").toInt() == 3; }));
    REQUIRE(waitUntil([window]() -> bool {
        return window->activeFocusItem() != nullptr &&
               window->activeFocusItem()->objectName() == QStringLiteral("journalEntryEditor-1");
    }));
    childEditor = window->activeFocusItem();
    CHECK(childEditor->property("cursorPosition").toInt() == 2);
    CHECK(controller.journalEntryId(1) == childId);

    QTest::keyClick(window, Qt::Key_Backtab, Qt::ShiftModifier);
    REQUIRE(waitUntil([&controller]() -> bool {
        return controller.journalEntries()
                   ->data(controller.journalEntries()->index(1, 0), JournalEntryModel::DepthRole)
                   .toInt() == 0;
    }));
#ifdef Q_OS_MACOS
    constexpr auto structureModifier = Qt::MetaModifier;
#else
    constexpr auto structureModifier = Qt::ControlModifier;
#endif
    QTest::keyClick(window, Qt::Key_Down, structureModifier | Qt::ShiftModifier);
    REQUIRE(waitUntil(
        [&controller, &childId]() -> bool { return controller.journalEntryId(2) == childId; }));
}

TEST_CASE("the Journal editor groups typing and routes standard undo and redo") {
    QTemporaryDir temporaryDirectory;
    REQUIRE(temporaryDirectory.isValid());
    NotebookController controller;
    controller.createNotebook(
        QUrl::fromLocalFile(temporaryDirectory.filePath(QStringLiteral("undo-ui.hieda"))));
    REQUIRE(controller.insertJournalEntry(QStringLiteral("before")) == 0);

    QQmlEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("notebookController"), &controller);
    QQmlComponent component(&engine,
                            QUrl::fromLocalFile(QStringLiteral(HIEDA_SOURCE_DIR "/qml/Main.qml")));
    std::unique_ptr<QObject> root(component.create());
    INFO(component.errorString().toStdString());
    REQUIRE(root != nullptr);
    auto* window = qobject_cast<QQuickWindow*>(root.get());
    REQUIRE(window != nullptr);
    window->show();
    window->requestActivate();
    REQUIRE(waitUntil([window]() -> bool { return window->isActive(); }));
    auto* journalList = root->findChild<QQuickItem*>(QStringLiteral("journalList"));
    REQUIRE(journalList != nullptr);
    REQUIRE(
        waitUntil([journalList]() -> bool { return journalList->property("count").toInt() == 1; }));
    QVariant entryValue;
    REQUIRE(QMetaObject::invokeMethod(journalList, "entryItemAt",
                                      Q_RETURN_ARG(QVariant, entryValue), Q_ARG(QVariant, 0)));
    auto* entry = qobject_cast<QQuickItem*>(entryValue.value<QObject*>());
    REQUIRE(entry != nullptr);
    auto* editor = entry->findChild<QQuickItem*>(QStringLiteral("journalEntryEditor-0"));
    REQUIRE(editor != nullptr);
    editor->forceActiveFocus();
    editor->setProperty("cursorPosition", 6);
    QTest::keyClick(window, Qt::Key_X);
    REQUIRE(waitUntil(
        [&controller]() -> bool {
            return controller.journalEntries()
                       ->data(controller.journalEntries()->index(0, 0),
                              JournalEntryModel::AuthoredTextRole)
                       .toString() == QStringLiteral("beforex");
        },
        std::chrono::seconds(2)));
    auto* undoAction = root->findChild<QObject*>(QStringLiteral("undoAction"));
    auto* redoAction = root->findChild<QObject*>(QStringLiteral("redoAction"));
    REQUIRE(undoAction != nullptr);
    REQUIRE(redoAction != nullptr);
    CHECK(undoAction->property("enabled").toBool());
#ifdef Q_OS_MACOS
    constexpr auto undoModifier = Qt::MetaModifier;
#else
    constexpr auto undoModifier = Qt::ControlModifier;
#endif
    QTest::keyClick(window, Qt::Key_Z, undoModifier);
    REQUIRE(waitUntil([&controller]() -> bool {
        return controller.journalEntries()
                   ->data(controller.journalEntries()->index(0, 0),
                          JournalEntryModel::AuthoredTextRole)
                   .toString() == QStringLiteral("before");
    }));
    CHECK(redoAction->property("enabled").toBool());
    QTest::keyClick(window, Qt::Key_Z, undoModifier | Qt::ShiftModifier);
    REQUIRE(waitUntil([&controller]() -> bool {
        return controller.journalEntries()
                   ->data(controller.journalEntries()->index(0, 0),
                          JournalEntryModel::AuthoredTextRole)
                   .toString() == QStringLiteral("beforex");
    }));

    REQUIRE(waitUntil([window]() -> bool {
        return window->activeFocusItem() != nullptr &&
               window->activeFocusItem()->objectName() == QStringLiteral("journalEntryEditor-0");
    }));
    QTest::keyClick(window, Qt::Key_Y);
    auto* closeAction = root->findChild<QObject*>(QStringLiteral("closeAction"));
    REQUIRE(closeAction != nullptr);
    REQUIRE(QMetaObject::invokeMethod(closeAction, "trigger"));
    REQUIRE(waitUntil([&controller]() -> bool { return !controller.hasOpenNotebook(); }));
    controller.openNotebook(
        QUrl::fromLocalFile(temporaryDirectory.filePath(QStringLiteral("undo-ui.hieda"))));
    REQUIRE(controller.journalEntries()->rowCount() == 1);
    CHECK(controller.journalEntries()
              ->data(controller.journalEntries()->index(0, 0), JournalEntryModel::AuthoredTextRole)
              .toString() == QStringLiteral("beforexy"));
}

auto main(int argc, char* argv[]) -> int {
    QApplication application(argc, argv);
    return Catch::Session().run(argc, argv);
}
