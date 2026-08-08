// SPDX-License-Identifier: MPL-2.0
#include "notebook_controller.hpp"

#include <catch2/catch_session.hpp>
#include <catch2/catch_test_macros.hpp>

#include <QAccessible>
#include <QApplication>
#include <QClipboard>
#include <QElapsedTimer>
#include <QInputMethodEvent>
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
    childEditor->setProperty("cursorPosition", 5);
    QTest::keyClick(window, Qt::Key_X);
    REQUIRE(QMetaObject::invokeMethod(childEntry, "openContextMenu"));
    auto* indentMenuItem =
        childEntry->findChild<QQuickItem*>(QStringLiteral("journalIndentMenuItem-1"));
    REQUIRE(indentMenuItem != nullptr);
    REQUIRE(waitUntil([indentMenuItem]() -> bool { return indentMenuItem->isVisible(); }));
    const auto menuItemCenter = indentMenuItem->mapToScene(
        QPointF(indentMenuItem->width() / 2, indentMenuItem->height() / 2));
    QTest::mouseClick(indentMenuItem->window(), Qt::LeftButton, Qt::NoModifier,
                      menuItemCenter.toPoint());
    REQUIRE(waitUntil([&controller]() -> bool {
        return controller.journalEntries()
                       ->data(controller.journalEntries()->index(1, 0),
                              JournalEntryModel::DepthRole)
                       .toInt() == 1 &&
               controller.journalEntries()
                       ->data(controller.journalEntries()->index(1, 0),
                              JournalEntryModel::AuthoredTextRole)
                       .toString() == QStringLiteral("childx");
    }));
#ifdef Q_OS_MACOS
    constexpr auto undoModifier = Qt::MetaModifier;
#else
    constexpr auto undoModifier = Qt::ControlModifier;
#endif
    QTest::keyClick(window, Qt::Key_Z, undoModifier);
    REQUIRE(waitUntil([&controller]() -> bool {
        return controller.journalEntries()
                       ->data(controller.journalEntries()->index(1, 0),
                              JournalEntryModel::DepthRole)
                       .toInt() == 0 &&
               controller.journalEntries()
                       ->data(controller.journalEntries()->index(1, 0),
                              JournalEntryModel::AuthoredTextRole)
                       .toString() == QStringLiteral("child");
    }));
    childEntry = entryAt(1);
    REQUIRE(childEntry != nullptr);
    childEditor = childEntry->findChild<QQuickItem*>(QStringLiteral("journalEntryEditor-1"));
    REQUIRE(childEditor != nullptr);
    childEditor->forceActiveFocus();
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
    constexpr auto structureModifier = undoModifier;
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
    editor->setProperty("cursorPosition", 3);
    QTest::keyClick(window, Qt::Key_Z, undoModifier);
    REQUIRE(waitUntil([&controller]() -> bool {
        return controller.journalEntries()
                   ->data(controller.journalEntries()->index(0, 0),
                          JournalEntryModel::AuthoredTextRole)
                   .toString() == QStringLiteral("before");
    }));
    REQUIRE(waitUntil([window]() -> bool {
        return window->activeFocusItem() != nullptr &&
               window->activeFocusItem()->objectName() == QStringLiteral("journalEntryEditor-0");
    }));
    CHECK(window->activeFocusItem()->property("cursorPosition").toInt() == 3);
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
    const auto textBeforeClose = window->activeFocusItem()->property("text").toString();
    auto* closeAction = root->findChild<QObject*>(QStringLiteral("closeAction"));
    REQUIRE(closeAction != nullptr);
    REQUIRE(QMetaObject::invokeMethod(closeAction, "trigger"));
    REQUIRE(waitUntil([&controller]() -> bool { return !controller.hasOpenNotebook(); }));
    controller.openNotebook(
        QUrl::fromLocalFile(temporaryDirectory.filePath(QStringLiteral("undo-ui.hieda"))));
    REQUIRE(controller.journalEntries()->rowCount() == 1);
    CHECK(controller.journalEntries()
              ->data(controller.journalEntries()->index(0, 0), JournalEntryModel::AuthoredTextRole)
              .toString() == textBeforeClose);
}

TEST_CASE("the Journal editor inserts and pastes multiline Unicode before splitting on Enter") {
    QTemporaryDir temporaryDirectory;
    REQUIRE(temporaryDirectory.isValid());
    NotebookController controller;
    controller.createNotebook(
        QUrl::fromLocalFile(temporaryDirectory.filePath(QStringLiteral("multiline-ui.hieda"))));
    REQUIRE(controller.insertJournalEntry(QStringLiteral("alpha")) == 0);

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
    editor->setProperty("cursorPosition", 5);

    QTest::keyClick(window, Qt::Key_Return, Qt::ShiftModifier);
    REQUIRE(journalList->property("count").toInt() == 1);
    REQUIRE(waitUntil([editor]() -> bool {
        return editor->property("text").toString() == QStringLiteral("alpha\n");
    }));
    QGuiApplication::clipboard()->setText(QStringLiteral("\u03B2\n\u65E5\u672C\u8A9E"));
#ifdef Q_OS_MACOS
    constexpr auto clipboardModifier = Qt::MetaModifier;
#else
    constexpr auto clipboardModifier = Qt::ControlModifier;
#endif
    QTest::keyClick(window, Qt::Key_V, clipboardModifier);
    REQUIRE(waitUntil([editor]() -> bool {
        return editor->property("text").toString() ==
               QStringLiteral("alpha\n\u03B2\n\u65E5\u672C\u8A9E");
    }));
    REQUIRE(QMetaObject::invokeMethod(editor, "select", Q_ARG(int, 6), Q_ARG(int, 7)));
    auto* copyAction = root->findChild<QObject*>(QStringLiteral("copyAction"));
    auto* cutAction = root->findChild<QObject*>(QStringLiteral("cutAction"));
    auto* pasteAction = root->findChild<QObject*>(QStringLiteral("pasteAction"));
    REQUIRE(copyAction != nullptr);
    REQUIRE(cutAction != nullptr);
    REQUIRE(pasteAction != nullptr);
    REQUIRE(QMetaObject::invokeMethod(copyAction, "trigger"));
    CHECK(QGuiApplication::clipboard()->text() == QStringLiteral("\u03B2"));
    REQUIRE(QMetaObject::invokeMethod(cutAction, "trigger"));
    CHECK(editor->property("text").toString() == QStringLiteral("alpha\n\n\u65E5\u672C\u8A9E"));
    REQUIRE(QMetaObject::invokeMethod(pasteAction, "trigger"));
    CHECK(editor->property("text").toString() ==
          QStringLiteral("alpha\n\u03B2\n\u65E5\u672C\u8A9E"));
    REQUIRE(waitUntil(
        [&controller]() -> bool {
            return controller.journalEntries()
                       ->data(controller.journalEntries()->index(0, 0),
                              JournalEntryModel::AuthoredTextRole)
                       .toString() == QStringLiteral("alpha\n\u03B2\n\u65E5\u672C\u8A9E");
        },
        std::chrono::seconds(2)));

    editor->setProperty("cursorPosition", 6);
    QTest::keyClick(window, Qt::Key_Return);
    REQUIRE(
        waitUntil([journalList]() -> bool { return journalList->property("count").toInt() == 2; }));
    CHECK(controller.journalEntries()
              ->data(controller.journalEntries()->index(0, 0), JournalEntryModel::AuthoredTextRole)
              .toString() == QStringLiteral("alpha\n"));
    CHECK(controller.journalEntries()
              ->data(controller.journalEntries()->index(1, 0), JournalEntryModel::AuthoredTextRole)
              .toString() == QStringLiteral("\u03B2\n\u65E5\u672C\u8A9E"));
}

TEST_CASE("Journal bullets select and cut complete subtrees with accessible clipboard text") {
    QTemporaryDir temporaryDirectory;
    REQUIRE(temporaryDirectory.isValid());
    NotebookController controller;
    controller.createNotebook(
        QUrl::fromLocalFile(temporaryDirectory.filePath(QStringLiteral("selection-ui.hieda"))));
    REQUIRE(controller.insertJournalEntry(QStringLiteral("parent\ncontinuation")) == 0);
    REQUIRE(controller.insertJournalEntry(QStringLiteral("child")) == 1);
    REQUIRE(controller.insertJournalEntry(QStringLiteral("tail")) == 2);
    REQUIRE(controller.indentJournalEntry(controller.journalEntryId(1), QStringLiteral("child"), 5)
                .value(QStringLiteral("succeeded"))
                .toBool());

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

    auto bulletAt = [journalList](int row) -> QQuickItem* {
        QVariant entryValue;
        if (!QMetaObject::invokeMethod(journalList, "entryItemAt",
                                       Q_RETURN_ARG(QVariant, entryValue), Q_ARG(QVariant, row))) {
            return nullptr;
        }
        auto* entry = qobject_cast<QQuickItem*>(entryValue.value<QObject*>());
        return entry == nullptr ? nullptr
                                : entry->findChild<QQuickItem*>(
                                      QStringLiteral("journalEntryBullet-") + QString::number(row));
    };
    auto* parentBullet = bulletAt(0);
    auto* tailBullet = bulletAt(2);
    REQUIRE(parentBullet != nullptr);
    REQUIRE(tailBullet != nullptr);
    const auto parentCenter =
        parentBullet->mapToScene(QPointF(parentBullet->width() / 2, parentBullet->height() / 2));
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, parentCenter.toPoint());
    REQUIRE(waitUntil([root = root.get()]() -> bool {
        return root->property("outlineSelectionCount").toInt() == 2;
    }));
    QTest::keyClick(window, Qt::Key_Down, Qt::ShiftModifier);
    REQUIRE(waitUntil([root = root.get()]() -> bool {
        return root->property("outlineSelectionCount").toInt() == 2;
    }));
    const auto tailCenter =
        tailBullet->mapToScene(QPointF(tailBullet->width() / 2, tailBullet->height() / 2));
    QTest::mouseClick(window, Qt::LeftButton, Qt::ShiftModifier, tailCenter.toPoint());
    REQUIRE(waitUntil([root = root.get()]() -> bool {
        return root->property("outlineSelectionCount").toInt() == 3;
    }));

    auto* copyAction = root->findChild<QObject*>(QStringLiteral("copyAction"));
    auto* cutAction = root->findChild<QObject*>(QStringLiteral("cutAction"));
    REQUIRE(copyAction != nullptr);
    REQUIRE(cutAction != nullptr);
    REQUIRE(QMetaObject::invokeMethod(copyAction, "trigger"));
    CHECK(QGuiApplication::clipboard()->text() ==
          QStringLiteral("\u2022 parent\n  continuation\n  \u2022 child\n\u2022 tail"));
    REQUIRE(QMetaObject::invokeMethod(cutAction, "trigger"));
    REQUIRE(
        waitUntil([journalList]() -> bool { return journalList->property("count").toInt() == 0; }));
    REQUIRE(waitUntil([window]() -> bool {
        return window->activeFocusItem() != nullptr &&
               window->activeFocusItem()->objectName() == QStringLiteral("journalDraftEditor");
    }));
    REQUIRE(controller.undoJournalEdit().value(QStringLiteral("succeeded")).toBool());
    CHECK(controller.journalEntries()->rowCount() == 3);
}

TEST_CASE("Up and Down move within multiline Entries before crossing outline boundaries") {
    QTemporaryDir temporaryDirectory;
    REQUIRE(temporaryDirectory.isValid());
    NotebookController controller;
    controller.createNotebook(
        QUrl::fromLocalFile(temporaryDirectory.filePath(QStringLiteral("arrow-ui.hieda"))));
    REQUIRE(controller.insertJournalEntry(QStringLiteral("first\nline")) == 0);
    REQUIRE(controller.insertJournalEntry(QStringLiteral("second\nline")) == 1);

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
        waitUntil([journalList]() -> bool { return journalList->property("count").toInt() == 2; }));
    QVariant firstValue;
    REQUIRE(QMetaObject::invokeMethod(journalList, "entryItemAt",
                                      Q_RETURN_ARG(QVariant, firstValue), Q_ARG(QVariant, 0)));
    auto* firstEntry = qobject_cast<QQuickItem*>(firstValue.value<QObject*>());
    REQUIRE(firstEntry != nullptr);
    auto* firstEditor = firstEntry->findChild<QQuickItem*>(QStringLiteral("journalEntryEditor-0"));
    REQUIRE(firstEditor != nullptr);
    firstEditor->forceActiveFocus();
    firstEditor->setProperty("cursorPosition", 0);

    QTest::keyClick(window, Qt::Key_Down);
    REQUIRE(window->activeFocusItem() == firstEditor);
    CHECK(firstEditor->property("cursorPosition").toInt() > 5);

    firstEditor->setProperty("cursorPosition", firstEditor->property("length").toInt());
    QTest::keyClick(window, Qt::Key_Down);
    REQUIRE(waitUntil([window]() -> bool {
        return window->activeFocusItem() != nullptr &&
               window->activeFocusItem()->objectName() == QStringLiteral("journalEntryEditor-1");
    }));
    auto* secondEditor = window->activeFocusItem();
    CHECK(secondEditor->property("cursorPosition").toInt() < 7);
    QTest::keyClick(window, Qt::Key_Up);
    REQUIRE(waitUntil([window]() -> bool {
        return window->activeFocusItem() != nullptr &&
               window->activeFocusItem()->objectName() == QStringLiteral("journalEntryEditor-0");
    }));
    CHECK(window->activeFocusItem()->property("cursorPosition").toInt() > 5);
}

TEST_CASE("IME preedit commits and cancels without premature Journal persistence") {
    QTemporaryDir temporaryDirectory;
    REQUIRE(temporaryDirectory.isValid());
    NotebookController controller;
    controller.createNotebook(
        QUrl::fromLocalFile(temporaryDirectory.filePath(QStringLiteral("ime-ui.hieda"))));
    REQUIRE(controller.insertJournalEntry(QStringLiteral("base")) == 0);
    REQUIRE(controller.insertJournalEntry(QStringLiteral("other")) == 1);

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
        waitUntil([journalList]() -> bool { return journalList->property("count").toInt() == 2; }));
    QVariant firstValue;
    QVariant secondValue;
    REQUIRE(QMetaObject::invokeMethod(journalList, "entryItemAt",
                                      Q_RETURN_ARG(QVariant, firstValue), Q_ARG(QVariant, 0)));
    REQUIRE(QMetaObject::invokeMethod(journalList, "entryItemAt",
                                      Q_RETURN_ARG(QVariant, secondValue), Q_ARG(QVariant, 1)));
    auto* firstEntry = qobject_cast<QQuickItem*>(firstValue.value<QObject*>());
    auto* secondEntry = qobject_cast<QQuickItem*>(secondValue.value<QObject*>());
    REQUIRE(firstEntry != nullptr);
    REQUIRE(secondEntry != nullptr);
    auto* firstEditor = firstEntry->findChild<QQuickItem*>(QStringLiteral("journalEntryEditor-0"));
    auto* secondEditor =
        secondEntry->findChild<QQuickItem*>(QStringLiteral("journalEntryEditor-1"));
    REQUIRE(firstEditor != nullptr);
    REQUIRE(secondEditor != nullptr);
    firstEditor->forceActiveFocus();
    firstEditor->setProperty("cursorPosition", 4);

    QInputMethodEvent preedit(QStringLiteral("\u65E5"), {});
    QCoreApplication::sendEvent(firstEditor, &preedit);
    REQUIRE(firstEditor->property("inputMethodComposing").toBool());
    QTest::qWait(1100);
    CHECK(controller.journalEntries()
              ->data(controller.journalEntries()->index(0, 0), JournalEntryModel::AuthoredTextRole)
              .toString() == QStringLiteral("base"));
    secondEditor->forceActiveFocus();
    REQUIRE(waitUntil([firstEditor]() -> bool { return firstEditor->hasActiveFocus(); }));

    QInputMethodEvent commit;
    commit.setCommitString(QStringLiteral("\u65E5\u672C\u8A9E"));
    QCoreApplication::sendEvent(firstEditor, &commit);
    REQUIRE_FALSE(firstEditor->property("inputMethodComposing").toBool());
    CHECK(firstEditor->property("text").toString() == QStringLiteral("base\u65E5\u672C\u8A9E"));
    REQUIRE(waitUntil(
        [&controller]() -> bool {
            return controller.journalEntries()
                       ->data(controller.journalEntries()->index(0, 0),
                              JournalEntryModel::AuthoredTextRole)
                       .toString() == QStringLiteral("base\u65E5\u672C\u8A9E");
        },
        std::chrono::seconds(2)));

    QInputMethodEvent cancelledPreedit(QStringLiteral("\u672A"), {});
    QCoreApplication::sendEvent(firstEditor, &cancelledPreedit);
    REQUIRE(firstEditor->property("inputMethodComposing").toBool());
    QInputMethodEvent cancel;
    QCoreApplication::sendEvent(firstEditor, &cancel);
    REQUIRE_FALSE(firstEditor->property("inputMethodComposing").toBool());
    CHECK(firstEditor->property("text").toString() == QStringLiteral("base\u65E5\u672C\u8A9E"));
    CHECK(controller.journalEntries()->rowCount() == 2);
}

TEST_CASE("the Journal exposes list structure selection and multiline editing accessibly") {
    QAccessible::setActive(true);
    QTemporaryDir temporaryDirectory;
    REQUIRE(temporaryDirectory.isValid());
    NotebookController controller;
    controller.createNotebook(
        QUrl::fromLocalFile(temporaryDirectory.filePath(QStringLiteral("accessible-ui.hieda"))));
    REQUIRE(controller.insertJournalEntry(QStringLiteral("accessible\ntext")) == 0);

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
    auto* bullet = entry->findChild<QQuickItem*>(QStringLiteral("journalEntryBullet-0"));
    auto* editor = entry->findChild<QQuickItem*>(QStringLiteral("journalEntryEditor-0"));
    REQUIRE(bullet != nullptr);
    REQUIRE(editor != nullptr);
    QVariant selected;
    REQUIRE(QMetaObject::invokeMethod(root.get(), "selectOutline", Q_RETURN_ARG(QVariant, selected),
                                      Q_ARG(QVariant, 0), Q_ARG(QVariant, false)));
    REQUIRE(waitUntil([root = root.get()]() -> bool {
        return root->property("outlineSelectionCount").toInt() == 1;
    }));
    REQUIRE(waitUntil([bullet]() -> bool { return bullet->hasActiveFocus(); }));

    auto* listInterface = QAccessible::queryAccessibleInterface(journalList);
    auto* bulletInterface = QAccessible::queryAccessibleInterface(bullet);
    auto* editorInterface = QAccessible::queryAccessibleInterface(editor);
    REQUIRE(listInterface != nullptr);
    REQUIRE(bulletInterface != nullptr);
    REQUIRE(editorInterface != nullptr);
    CHECK(listInterface->role() == QAccessible::List);
    CHECK(bulletInterface->role() == QAccessible::ListItem);
    CHECK(bulletInterface->text(QAccessible::Name) == QStringLiteral("Select Journal Entry 1"));
    CHECK(bulletInterface->state().selectable);
    CHECK(bulletInterface->state().selected);
    CHECK(bulletInterface->state().focused);
    CHECK(editorInterface->role() == QAccessible::EditableText);
    CHECK(editorInterface->state().multiLine);
    CHECK(editorInterface->textInterface() != nullptr);
}

auto main(int argc, char* argv[]) -> int {
    QApplication application(argc, argv);
    return Catch::Session().run(argc, argv);
}
