// SPDX-License-Identifier: MPL-2.0
#include "notebook_controller.hpp"

#include <catch2/catch_session.hpp>
#include <catch2/catch_test_macros.hpp>

#include <QAccessible>
#include <QApplication>
#include <QClipboard>
#include <QElapsedTimer>
#include <QInputMethodEvent>
#include <QKeyEvent>
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

#include <array>
#include <chrono>
#include <memory>

namespace {

template <typename Predicate>
auto waitUntil(const Predicate& predicate,
               std::chrono::milliseconds timeout = std::chrono::seconds(1)) -> bool {
    QElapsedTimer timer;
    timer.start();
    while (!predicate() && timer.elapsed() < timeout.count()) {
        QCoreApplication::processEvents();
        QTest::qWait(5);
    }
    return predicate();
}

void sendKeyClickWithoutProcessingEvents(QQuickWindow* window, Qt::Key key,
                                         Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
    QKeyEvent press(QEvent::KeyPress, key, modifiers);
    QKeyEvent release(QEvent::KeyRelease, key, modifiers);
    QCoreApplication::sendEvent(window, &press);
    QCoreApplication::sendEvent(window, &release);
}

void collectQmlWarnings(QQmlEngine& engine, QStringList& messages) {
    QObject::connect(&engine, &QQmlEngine::warnings, &engine,
                     [&messages](const QList<QQmlError>& warnings) -> void {
                         for (const auto& warning : warnings) {
                             const auto message = warning.toString();
                             if (!message.isEmpty()) {
                                 messages.append(message);
                             }
                         }
                     });
}

} // namespace

TEST_CASE("the Page sidebar presents ordinary Pages and Journal navigation") {
    QAccessible::setActive(true);
    QTemporaryDir temporaryDirectory;
    REQUIRE(temporaryDirectory.isValid());
    NotebookController controller;
    controller.createNotebook(
        QUrl::fromLocalFile(temporaryDirectory.filePath(QStringLiteral("page-ui.hieda"))));
    REQUIRE(controller.createPage(QStringLiteral("project"), QStringLiteral("Project")));
    const auto projectId = controller.currentPageId();
    REQUIRE(controller.insertOutlineEntry(QStringLiteral("durable")) == 0);
    REQUIRE(controller.createPage(QStringLiteral("second"), QStringLiteral("Second Page")));
    controller.navigateToPage(projectId);

    QQmlEngine engine;
    QSignalSpy qmlWarnings(&engine, &QQmlEngine::warnings);
    engine.rootContext()->setContextProperty(QStringLiteral("notebookController"), &controller);
    QQmlComponent component(&engine,
                            QUrl::fromLocalFile(QStringLiteral(HIEDA_SOURCE_DIR "/qml/Main.qml")));
    std::unique_ptr<QObject> root(component.create());
    INFO(component.errorString().toStdString());
    REQUIRE(root != nullptr);
    auto* sidebar = root->findChild<QQuickItem*>(QStringLiteral("pageSidebar"));
    auto* pageList = root->findChild<QQuickItem*>(QStringLiteral("pageList"));
    REQUIRE(sidebar != nullptr);
    REQUIRE(pageList != nullptr);
    CHECK(sidebar->isVisible());
    CHECK(pageList->property("rows").toInt() == 2);
    REQUIRE(root->findChild<QObject*>(QStringLiteral("newPageAction")) != nullptr);
    REQUIRE(root->findChild<QObject*>(QStringLiteral("goToPageAction")) != nullptr);
    REQUIRE(root->findChild<QObject*>(QStringLiteral("renamePageAction")) != nullptr);
    auto* pagePicker = root->findChild<QQuickItem*>(QStringLiteral("pagePicker"));
    auto* pageFilterField = root->findChild<QQuickItem*>(QStringLiteral("pageFilterField"));
    REQUIRE(pagePicker != nullptr);
    REQUIRE(pageFilterField != nullptr);
    CHECK_FALSE(pagePicker->property("editable").toBool());
    pageFilterField->setProperty("text", QStringLiteral("project"));
    REQUIRE(
        waitUntil([pagePicker]() -> bool { return pagePicker->property("count").toInt() == 1; }));

    auto* window = qobject_cast<QQuickWindow*>(root.get());
    auto* outlineList = root->findChild<QQuickItem*>(QStringLiteral("outlineList"));
    REQUIRE(window != nullptr);
    REQUIRE(outlineList != nullptr);
    window->setWidth(1254);
    window->setHeight(863);
    window->show();
    window->requestActivate();
    REQUIRE(waitUntil([window]() -> bool { return window->isActive(); }));
    QVariant entryValue;
    REQUIRE(QMetaObject::invokeMethod(outlineList, "entryItemAt",
                                      Q_RETURN_ARG(QVariant, entryValue), Q_ARG(QVariant, 0)));
    auto* entry = qobject_cast<QQuickItem*>(entryValue.value<QObject*>());
    REQUIRE(entry != nullptr);
    auto* editor = entry->findChild<QQuickItem*>(QStringLiteral("outlineEntryEditor-0"));
    REQUIRE(editor != nullptr);
    editor->forceActiveFocus();
    REQUIRE(waitUntil([editor]() -> bool { return editor->hasActiveFocus(); }));
    editor->setProperty("text", QStringLiteral("committed before rename"));

    auto* renameAction = root->findChild<QObject*>(QStringLiteral("renamePageAction"));
    auto* renameDialog = root->findChild<QObject*>(QStringLiteral("renamePageDialog"));
    auto* renameName = root->findChild<QQuickItem*>(QStringLiteral("renamePageName"));
    auto* renameTitle = root->findChild<QQuickItem*>(QStringLiteral("renamePageTitle"));
    REQUIRE(renameAction != nullptr);
    REQUIRE(renameDialog != nullptr);
    REQUIRE(renameName != nullptr);
    REQUIRE(renameTitle != nullptr);
    REQUIRE(QMetaObject::invokeMethod(renameAction, "trigger"));
    REQUIRE(
        waitUntil([renameDialog]() -> bool { return renameDialog->property("visible").toBool(); }));
    renameName->setProperty("text", QStringLiteral("renamed_project"));
    renameTitle->setProperty("text", QStringLiteral("Renamed Project"));
    REQUIRE(QMetaObject::invokeMethod(renameDialog, "accept"));
    REQUIRE(waitUntil([&controller]() -> bool {
        return controller.currentPageName() == QStringLiteral("renamed_project") &&
               controller.currentPageTitle() == QStringLiteral("Renamed Project");
    }));
    REQUIRE(waitUntil([editor]() -> bool { return editor->hasActiveFocus(); }));
    CHECK(controller.outlineEntries()
              ->data(controller.outlineEntries()->index(0, 0), OutlineEntryModel::AuthoredTextRole)
              .toString() == QStringLiteral("committed before rename"));
    auto* heading = root->findChild<QQuickItem*>(QStringLiteral("pageHeading"));
    REQUIRE(heading != nullptr);
    CHECK(heading->property("text").toString().contains(QStringLiteral("Renamed Project")));
    CHECK(heading->property("text").toString().contains(QStringLiteral("renamed_project")));
    auto* bullet = entry->findChild<QQuickItem*>(QStringLiteral("outlineEntryBullet-0"));
    REQUIRE(bullet != nullptr);
    auto* listInterface = QAccessible::queryAccessibleInterface(outlineList);
    auto* headingInterface = QAccessible::queryAccessibleInterface(heading);
    auto* bulletInterface = QAccessible::queryAccessibleInterface(bullet);
    auto* editorInterface = QAccessible::queryAccessibleInterface(editor);
    REQUIRE(listInterface != nullptr);
    REQUIRE(headingInterface != nullptr);
    REQUIRE(bulletInterface != nullptr);
    REQUIRE(editorInterface != nullptr);
    CHECK(listInterface->text(QAccessible::Name) == QStringLiteral("Page Entries"));
    CHECK(headingInterface->text(QAccessible::Name) ==
          QStringLiteral("Page title Renamed Project, name renamed_project"));
    CHECK(bulletInterface->role() == QAccessible::ListItem);
    CHECK(bulletInterface->text(QAccessible::Name) == QStringLiteral("Select Page Entry 1"));
    CHECK(editorInterface->role() == QAccessible::EditableText);

    qsizetype projectIndex = -1;
    for (qsizetype index = 0; index < controller.pageChoices().size(); ++index) {
        if (controller.pageIdAt(index) == projectId) {
            projectIndex = index;
            break;
        }
    }
    REQUIRE(projectIndex >= 0);
    QVariant pageDelegateValue;
    REQUIRE(QMetaObject::invokeMethod(pageList, "pageItemAt",
                                      Q_RETURN_ARG(QVariant, pageDelegateValue),
                                      Q_ARG(QVariant, projectIndex)));
    auto* pageDelegate = qobject_cast<QQuickItem*>(pageDelegateValue.value<QObject*>());
    REQUIRE(pageDelegate != nullptr);
    CHECK(pageDelegate->property("highlighted").toBool());
    CHECK(pageDelegate->mapToItem(sidebar, QPointF(pageDelegate->width(), 0)).x() <=
          sidebar->width());

    REQUIRE(QMetaObject::invokeMethod(renameAction, "trigger"));
    REQUIRE(
        waitUntil([renameDialog]() -> bool { return renameDialog->property("visible").toBool(); }));
    renameName->setProperty("text", QStringLiteral("second"));
    renameTitle->setProperty("text", QStringLiteral("Conflict"));
    REQUIRE(QMetaObject::invokeMethod(renameDialog, "accept"));
    REQUIRE(
        waitUntil([renameDialog]() -> bool { return renameDialog->property("visible").toBool(); }));
    CHECK_FALSE(controller.errorMessage().isEmpty());
    REQUIRE(QMetaObject::invokeMethod(renameDialog, "reject"));
    REQUIRE(waitUntil([editor]() -> bool { return editor->hasActiveFocus(); }));

    auto* goToAction = root->findChild<QObject*>(QStringLiteral("goToPageAction"));
    auto* goToDialog = root->findChild<QObject*>(QStringLiteral("goToPageDialog"));
    auto* openSelected = root->findChild<QQuickItem*>(QStringLiteral("openSelectedPageButton"));
    REQUIRE(goToAction != nullptr);
    REQUIRE(goToDialog != nullptr);
    REQUIRE(openSelected != nullptr);
    qmlWarnings.clear();
    REQUIRE(QMetaObject::invokeMethod(goToAction, "trigger"));
    pageFilterField->setProperty("text", QStringLiteral("second"));
    REQUIRE(
        waitUntil([pagePicker]() -> bool { return pagePicker->property("count").toInt() == 1; }));
    CHECK(qmlWarnings.count() == 0);
    REQUIRE(QMetaObject::invokeMethod(openSelected, "clicked"));
    REQUIRE(waitUntil([&controller, projectId]() -> bool {
        return !controller.isJournalPage() && controller.currentPageId() != projectId;
    }));
    CHECK_FALSE(goToDialog->property("visible").toBool());

    auto* newPageAction = root->findChild<QObject*>(QStringLiteral("newPageAction"));
    auto* newPageDialog = root->findChild<QObject*>(QStringLiteral("newPageDialog"));
    auto* newPageName = root->findChild<QQuickItem*>(QStringLiteral("newPageName"));
    auto* newPageTitle = root->findChild<QQuickItem*>(QStringLiteral("newPageTitle"));
    REQUIRE(newPageAction != nullptr);
    REQUIRE(newPageDialog != nullptr);
    REQUIRE(newPageName != nullptr);
    REQUIRE(newPageTitle != nullptr);
    REQUIRE(QMetaObject::invokeMethod(newPageAction, "trigger"));
    newPageName->setProperty("text", QStringLiteral("created_in_dialog"));
    newPageTitle->setProperty("text", QStringLiteral("Created in Dialog"));
    REQUIRE(QMetaObject::invokeMethod(newPageDialog, "accept"));
    REQUIRE(waitUntil([&controller]() -> bool {
        return controller.currentPageName() == QStringLiteral("created_in_dialog");
    }));
    REQUIRE(QMetaObject::invokeMethod(newPageAction, "trigger"));
    newPageName->setProperty("text", QStringLiteral("created_in_dialog"));
    newPageTitle->setProperty("text", QStringLiteral("Duplicate"));
    REQUIRE(QMetaObject::invokeMethod(newPageDialog, "accept"));
    REQUIRE(waitUntil(
        [newPageDialog]() -> bool { return newPageDialog->property("visible").toBool(); }));
    REQUIRE(QMetaObject::invokeMethod(newPageDialog, "reject"));

    auto* todayButton = root->findChild<QQuickItem*>(QStringLiteral("todayJournalButton"));
    auto* previousButton = root->findChild<QQuickItem*>(QStringLiteral("previousJournalButton"));
    auto* nextButton = root->findChild<QQuickItem*>(QStringLiteral("nextJournalButton"));
    auto* newPageButton = root->findChild<QQuickItem*>(QStringLiteral("newPageButton"));
    REQUIRE(todayButton != nullptr);
    REQUIRE(previousButton != nullptr);
    REQUIRE(nextButton != nullptr);
    REQUIRE(newPageButton != nullptr);
    CHECK(previousButton->mapToItem(sidebar, QPointF{}).x() >= 0);
    CHECK(nextButton->mapToItem(sidebar, QPointF(nextButton->width(), 0)).x() <= sidebar->width());
    CHECK(newPageButton->mapToItem(sidebar, QPointF(newPageButton->width(), 0)).x() <=
          sidebar->width());
    REQUIRE(QMetaObject::invokeMethod(todayButton, "clicked"));
    REQUIRE(waitUntil([&controller]() -> bool { return controller.isJournalPage(); }));
    const auto today = controller.journalDate();
    CHECK(todayButton->property("highlighted").toBool());
    REQUIRE(QMetaObject::invokeMethod(previousButton, "clicked"));
    CHECK(controller.journalDate() == today.addDays(-1));
    REQUIRE(QMetaObject::invokeMethod(nextButton, "clicked"));
    CHECK(controller.journalDate() == today);
    controller.navigateToPage(projectId);
    CHECK(controller.outlineEntries()
              ->data(controller.outlineEntries()->index(0, 0), OutlineEntryModel::AuthoredTextRole)
              .toString() == QStringLiteral("committed before rename"));
}

TEST_CASE("the Page sidebar opens and materializes hierarchy Page Previews") {
    QTemporaryDir temporaryDirectory;
    REQUIRE(temporaryDirectory.isValid());
    NotebookController controller;
    controller.createNotebook(
        QUrl::fromLocalFile(temporaryDirectory.filePath(QStringLiteral("hierarchy-ui.hieda"))));
    REQUIRE(controller.createPage(QStringLiteral("work/client/alpha"), QStringLiteral("Alpha")));
    controller.navigateToPageName(QStringLiteral("work/client"));
    REQUIRE(controller.currentPagePreview());

    QStringList qmlWarningMessages;
    QQmlEngine engine;
    collectQmlWarnings(engine, qmlWarningMessages);
    engine.rootContext()->setContextProperty(QStringLiteral("notebookController"), &controller);
    QQmlComponent component(&engine,
                            QUrl::fromLocalFile(QStringLiteral(HIEDA_SOURCE_DIR "/qml/Main.qml")));
    std::unique_ptr<QObject> root(component.create());
    INFO(component.errorString().toStdString());
    REQUIRE(root != nullptr);
    auto* window = qobject_cast<QQuickWindow*>(root.get());
    REQUIRE(window != nullptr);
    window->show();
    auto* pageList = root->findChild<QQuickItem*>(QStringLiteral("pageList"));
    auto* heading = root->findChild<QQuickItem*>(QStringLiteral("pageHeading"));
    auto* materializeButton = root->findChild<QQuickItem*>(QStringLiteral("materializePageButton"));
    auto* materializeDialog = root->findChild<QObject*>(QStringLiteral("materializePageDialog"));
    auto* materializeTitle = root->findChild<QQuickItem*>(QStringLiteral("materializePageTitle"));
    REQUIRE(pageList != nullptr);
    REQUIRE(heading != nullptr);
    REQUIRE(materializeButton != nullptr);
    REQUIRE(materializeDialog != nullptr);
    REQUIRE(materializeTitle != nullptr);
    const auto hierarchyDelegateAt = [pageList](int row) -> QQuickItem* {
        QVariant item;
        if (!QMetaObject::invokeMethod(pageList, "pageItemAt", Q_RETURN_ARG(QVariant, item),
                                       Q_ARG(QVariant, row))) {
            return nullptr;
        }
        return qobject_cast<QQuickItem*>(item.value<QObject*>());
    };
    REQUIRE(
        waitUntil([&hierarchyDelegateAt]() -> bool { return hierarchyDelegateAt(1) != nullptr; }));
    auto* clientDelegate = hierarchyDelegateAt(1);
    REQUIRE(clientDelegate != nullptr);
    CHECK(clientDelegate->property("text").toString() == QStringLiteral("client (Page Preview)"));
    CHECK(clientDelegate->isVisible());
    CHECK(clientDelegate->width() > 0);
    CHECK(clientDelegate->height() > 0);
    INFO(qmlWarningMessages.join(QLatin1Char('\n')).toStdString());
    CHECK(qmlWarningMessages.isEmpty());
    CHECK(heading->property("text").toString() == QStringLiteral("work/client — Page Preview"));
    CHECK(materializeButton->isVisible());

    REQUIRE(QMetaObject::invokeMethod(materializeButton, "clicked"));
    REQUIRE(waitUntil(
        [materializeDialog]() -> bool { return materializeDialog->property("visible").toBool(); }));
    materializeTitle->setProperty("text", QStringLiteral("Client"));
    REQUIRE(QMetaObject::invokeMethod(materializeDialog, "accept"));
    REQUIRE(waitUntil([&controller]() -> bool { return !controller.currentPagePreview(); }));
    REQUIRE(waitUntil([&hierarchyDelegateAt]() -> bool {
        const auto* delegate = hierarchyDelegateAt(1);
        return delegate != nullptr &&
               delegate->property("text").toString() == QStringLiteral("Client — client");
    }));
    clientDelegate = hierarchyDelegateAt(1);
    REQUIRE(clientDelegate != nullptr);
    CHECK(clientDelegate->property("text").toString() == QStringLiteral("Client — client"));
    CHECK(clientDelegate->isVisible());
    CHECK(qmlWarningMessages.isEmpty());
    CHECK(controller.currentPageName() == QStringLiteral("work/client"));
    CHECK(heading->property("text").toString() == QStringLiteral("Client — work/client"));

    auto* deleteButton = root->findChild<QQuickItem*>(QStringLiteral("deletePageButton"));
    REQUIRE(deleteButton != nullptr);
    CHECK(deleteButton->isVisible());
    REQUIRE(QMetaObject::invokeMethod(deleteButton, "clicked"));
    REQUIRE(waitUntil([&controller]() -> bool { return controller.currentPagePreview(); }));
    CHECK(qmlWarningMessages.isEmpty());
    CHECK(heading->property("text").toString() == QStringLiteral("work/client — Page Preview"));
    controller.closeNotebook();
    REQUIRE(waitUntil([root = root.get()]() -> bool {
        return root->property("hierarchyExpandedNames").toList().isEmpty();
    }));
    INFO(qmlWarningMessages.join(QLatin1Char('\n')).toStdString());
    CHECK(qmlWarningMessages.isEmpty());
}

TEST_CASE("the platform TreeViewDelegate expands lazy Page Hierarchy nodes") {
    QTemporaryDir temporaryDirectory;
    REQUIRE(temporaryDirectory.isValid());
    NotebookController controller;
    controller.createNotebook(QUrl::fromLocalFile(
        temporaryDirectory.filePath(QStringLiteral("hierarchy-delegate.hieda"))));
    REQUIRE(controller.createPage(QStringLiteral("work/client/alpha"), QStringLiteral("Alpha")));
    controller.navigateToPageName(QStringLiteral("work"));
    auto* hierarchyModel = controller.pageHierarchy();
    const auto rootIndex = hierarchyModel->index(0, 0);
    REQUIRE(rootIndex.isValid());
    CHECK(hierarchyModel->data(rootIndex, PageHierarchyModel::HasChildrenRole).toBool());
    CHECK(hierarchyModel->canFetchMore(rootIndex));
    CHECK(hierarchyModel->hasChildren(rootIndex));

    QStringList qmlWarningMessages;
    QQmlEngine engine;
    collectQmlWarnings(engine, qmlWarningMessages);
    engine.rootContext()->setContextProperty(QStringLiteral("notebookController"), &controller);
    QQmlComponent component(&engine);
    component.setData(R"QML(
import QtQuick
import QtQuick.Controls

ApplicationWindow {
    width: 320
    height: 240
    visible: true

    TreeView {
        id: hierarchy
        objectName: "hierarchy"
        anchors.fill: parent
        model: notebookController.pageHierarchy

        function hierarchyItemAt(row) {
            return itemAtCell(Qt.point(0, row));
        }

        function rootIsExpanded() {
            return isExpanded(0)
        }

        delegate: TreeViewDelegate {
            required property string pageName
            onClicked: notebookController.navigateToPageName(pageName)
        }
    }
}
)QML",
                      QUrl(QStringLiteral("inline:hierarchy-delegate.qml")));
    REQUIRE(
        waitUntil([&component]() -> bool { return component.status() != QQmlComponent::Loading; }));
    INFO(component.errorString().toStdString());
    REQUIRE(component.status() == QQmlComponent::Ready);
    std::unique_ptr<QObject> root(component.create());
    REQUIRE(root != nullptr);
    auto* hierarchy = root->findChild<QQuickItem*>(QStringLiteral("hierarchy"));
    REQUIRE(hierarchy != nullptr);
    REQUIRE(waitUntil([hierarchy]() -> bool {
        QVariant item;
        return QMetaObject::invokeMethod(hierarchy, "hierarchyItemAt", Q_RETURN_ARG(QVariant, item),
                                         Q_ARG(QVariant, 0)) &&
               item.value<QObject*>() != nullptr;
    }));
    QVariant item;
    REQUIRE(QMetaObject::invokeMethod(hierarchy, "hierarchyItemAt", Q_RETURN_ARG(QVariant, item),
                                      Q_ARG(QVariant, 0)));
    auto* delegate = item.value<QObject*>();
    REQUIRE(delegate != nullptr);
    auto* window = qobject_cast<QQuickWindow*>(root.get());
    auto* indicator = qobject_cast<QQuickItem*>(delegate->property("indicator").value<QObject*>());
    REQUIRE(window != nullptr);
    REQUIRE(indicator != nullptr);
    const auto indicatorCenter = indicator->mapToItem(
        window->contentItem(), QPointF(indicator->width() / 2, indicator->height() / 2));
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, indicatorCenter.toPoint());
    REQUIRE(waitUntil([hierarchy]() -> bool {
        QVariant expanded;
        return QMetaObject::invokeMethod(hierarchy, "rootIsExpanded",
                                         Q_RETURN_ARG(QVariant, expanded)) &&
               expanded.toBool();
    }));
    REQUIRE(waitUntil([hierarchy]() -> bool {
        QVariant child;
        return QMetaObject::invokeMethod(hierarchy, "hierarchyItemAt",
                                         Q_RETURN_ARG(QVariant, child), Q_ARG(QVariant, 1)) &&
               child.value<QObject*>() != nullptr;
    }));
    INFO(qmlWarningMessages.join(QLatin1Char('\n')).toStdString());
    CHECK(qmlWarningMessages.isEmpty());
}

TEST_CASE("the Page editor preserves consecutive outline key presses") {
    QTemporaryDir temporaryDirectory;
    REQUIRE(temporaryDirectory.isValid());
    NotebookController controller;
    controller.createNotebook(
        QUrl::fromLocalFile(temporaryDirectory.filePath(QStringLiteral("page-keys.hieda"))));
    REQUIRE(controller.createPage(QStringLiteral("keys"), QStringLiteral("Keys")));
    REQUIRE(controller.insertOutlineEntry(QStringLiteral("a")) == 0);
    REQUIRE(controller.insertOutlineEntry(QStringLiteral("b")) == 1);
    REQUIRE(controller.insertOutlineEntry(QStringLiteral("c")) == 2);

    QQmlEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("notebookController"), &controller);
    QQmlComponent component(&engine,
                            QUrl::fromLocalFile(QStringLiteral(HIEDA_SOURCE_DIR "/qml/Main.qml")));
    std::unique_ptr<QObject> root(component.create());
    INFO(component.errorString().toStdString());
    REQUIRE(root != nullptr);
    auto* window = qobject_cast<QQuickWindow*>(root.get());
    auto* outlineList = root->findChild<QQuickItem*>(QStringLiteral("outlineList"));
    REQUIRE(window != nullptr);
    REQUIRE(outlineList != nullptr);
    window->show();
    window->requestActivate();
    REQUIRE(waitUntil([window]() -> bool { return window->isActive(); }));

    const auto entryAt = [outlineList](int row) -> QQuickItem* {
        QVariant value;
        if (!QMetaObject::invokeMethod(outlineList, "entryItemAt", Q_RETURN_ARG(QVariant, value),
                                       Q_ARG(QVariant, row))) {
            return nullptr;
        }
        return qobject_cast<QQuickItem*>(value.value<QObject*>());
    };
    auto* firstEntry = entryAt(0);
    REQUIRE(firstEntry != nullptr);
    auto* firstEditor = firstEntry->findChild<QQuickItem*>(QStringLiteral("outlineEntryEditor-0"));
    REQUIRE(firstEditor != nullptr);
    firstEditor->forceActiveFocus();
    REQUIRE(waitUntil([firstEditor]() -> bool { return firstEditor->hasActiveFocus(); }));

    sendKeyClickWithoutProcessingEvents(window, Qt::Key_Down);
    sendKeyClickWithoutProcessingEvents(window, Qt::Key_Down);
    REQUIRE(waitUntil([window]() -> bool {
        return window->activeFocusItem() != nullptr &&
               window->activeFocusItem()->objectName() == QStringLiteral("outlineEntryEditor-2");
    }));

    auto* secondEntry = entryAt(1);
    REQUIRE(secondEntry != nullptr);
    auto* secondEditor =
        secondEntry->findChild<QQuickItem*>(QStringLiteral("outlineEntryEditor-1"));
    REQUIRE(secondEditor != nullptr);
    secondEditor->forceActiveFocus();
    REQUIRE(waitUntil([secondEditor]() -> bool { return secondEditor->hasActiveFocus(); }));
    sendKeyClickWithoutProcessingEvents(window, Qt::Key_Tab);
    REQUIRE(controller.outlineEntries()
                ->data(controller.outlineEntries()->index(1, 0), OutlineEntryModel::DepthRole)
                .toInt() == 1);
    sendKeyClickWithoutProcessingEvents(window, Qt::Key_Backtab, Qt::ShiftModifier);
    REQUIRE(waitUntil([&controller]() -> bool {
        return controller.outlineEntries()
                   ->data(controller.outlineEntries()->index(1, 0), OutlineEntryModel::DepthRole)
                   .toInt() == 0;
    }));
}

TEST_CASE("the Journal editor keeps aligned drafts focused without QML warnings") {
    QTemporaryDir temporaryDirectory;
    REQUIRE(temporaryDirectory.isValid());
    NotebookController controller;
    controller.createNotebook(
        QUrl::fromLocalFile(temporaryDirectory.filePath(QStringLiteral("ui-test.hieda"))));
    REQUIRE(controller.insertOutlineEntry(QString(400, QLatin1Char('x'))) == 0);
    REQUIRE(controller.insertOutlineEntry(QStringLiteral("second")) == 1);

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

    auto* outlineList = root->findChild<QQuickItem*>(QStringLiteral("outlineList"));
    REQUIRE(outlineList != nullptr);
    REQUIRE(
        waitUntil([outlineList]() -> bool { return outlineList->property("count").toInt() == 2; }));
    QVariant firstEntryValue;
    REQUIRE(QMetaObject::invokeMethod(outlineList, "entryItemAt",
                                      Q_RETURN_ARG(QVariant, firstEntryValue), Q_ARG(QVariant, 0)));
    auto* firstEntry = qobject_cast<QQuickItem*>(firstEntryValue.value<QObject*>());
    REQUIRE(firstEntry != nullptr);
    auto* editor = firstEntry->findChild<QQuickItem*>(QStringLiteral("outlineEntryEditor-0"));
    REQUIRE(editor != nullptr);
    REQUIRE(editor->window() == window);
    REQUIRE(editor->isVisible());
    REQUIRE(editor->isEnabled());
    REQUIRE(waitUntil([outlineList]() -> bool { return outlineList->width() <= 780; }));
    const auto listPosition = outlineList->mapToScene({});
    CHECK(listPosition.x() >= 400);
    CHECK(firstEntry->width() <= outlineList->width());
    const auto editorPosition = editor->mapToScene({});
    CHECK(editorPosition.x() >= listPosition.x());
    CHECK(editorPosition.x() + editor->width() <= listPosition.x() + outlineList->width());
    CHECK(editor->height() > 18);

    QSignalSpy qmlWarnings(&engine, &QQmlEngine::warnings);
    editor->forceActiveFocus();
    REQUIRE(waitUntil([editor]() -> bool { return editor->hasActiveFocus(); }));
    REQUIRE(QMetaObject::invokeMethod(root.get(), "beginDraft",
                                      Q_ARG(QVariant, controller.outlineEntryId(0))));

    REQUIRE(waitUntil([firstEntry]() -> bool {
        return firstEntry->findChild<QQuickItem*>(QStringLiteral("journalDraftEditor")) != nullptr;
    }));
    auto* draftEditor = firstEntry->findChild<QQuickItem*>(QStringLiteral("journalDraftEditor"));
    REQUIRE(draftEditor != nullptr);
    REQUIRE(waitUntil([draftEditor]() -> bool { return draftEditor->hasActiveFocus(); }));
    QTest::qWait(50);
    CHECK(firstEntry->findChild<QQuickItem*>(QStringLiteral("journalDraftEditor")) == draftEditor);

    auto* entryBullet = firstEntry->findChild<QQuickItem*>(QStringLiteral("outlineEntryBullet-0"));
    auto* draftBullet = firstEntry->findChild<QQuickItem*>(QStringLiteral("journalDraftBullet"));
    REQUIRE(entryBullet != nullptr);
    REQUIRE(draftBullet != nullptr);
    CHECK(draftBullet->mapToScene({}).x() == entryBullet->mapToScene({}).x());

    QTest::keyClick(window, Qt::Key_Escape);
    QVariant secondEntryValue;
    REQUIRE(QMetaObject::invokeMethod(
        outlineList, "entryItemAt", Q_RETURN_ARG(QVariant, secondEntryValue), Q_ARG(QVariant, 1)));
    auto* secondEntry = qobject_cast<QQuickItem*>(secondEntryValue.value<QObject*>());
    REQUIRE(secondEntry != nullptr);
    auto* secondEditor =
        secondEntry->findChild<QQuickItem*>(QStringLiteral("outlineEntryEditor-1"));
    REQUIRE(secondEditor != nullptr);
    secondEditor->forceActiveFocus();
    REQUIRE(waitUntil([secondEditor]() -> bool { return secondEditor->hasActiveFocus(); }));
    REQUIRE(QMetaObject::invokeMethod(root.get(), "beginDraft",
                                      Q_ARG(QVariant, controller.outlineEntryId(1))));
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
        secondEntry->findChild<QQuickItem*>(QStringLiteral("outlineEntryBullet-1"));
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
    REQUIRE(controller.insertOutlineEntry(QStringLiteral("parent")) == 0);
    REQUIRE(controller.insertOutlineEntry(QStringLiteral("child")) == 1);
    REQUIRE(controller.insertOutlineEntry(QStringLiteral("tail")) == 2);
    const auto childId = controller.outlineEntryId(1);

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
    auto* outlineList = root->findChild<QQuickItem*>(QStringLiteral("outlineList"));
    REQUIRE(outlineList != nullptr);
    REQUIRE(
        waitUntil([outlineList]() -> bool { return outlineList->property("count").toInt() == 3; }));

    const auto entryAt = [outlineList](int row) -> QQuickItem* {
        QVariant value;
        if (!QMetaObject::invokeMethod(outlineList, "entryItemAt", Q_RETURN_ARG(QVariant, value),
                                       Q_ARG(QVariant, row))) {
            return nullptr;
        }
        return qobject_cast<QQuickItem*>(value.value<QObject*>());
    };
    auto* parentEntry = entryAt(0);
    auto* childEntry = entryAt(1);
    REQUIRE(parentEntry != nullptr);
    REQUIRE(childEntry != nullptr);
    auto* childEditor = childEntry->findChild<QQuickItem*>(QStringLiteral("outlineEntryEditor-1"));
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
        return controller.outlineEntries()
                       ->data(controller.outlineEntries()->index(1, 0),
                              OutlineEntryModel::DepthRole)
                       .toInt() == 1 &&
               controller.outlineEntries()
                       ->data(controller.outlineEntries()->index(1, 0),
                              OutlineEntryModel::AuthoredTextRole)
                       .toString() == QStringLiteral("childx");
    }));
#ifdef Q_OS_MACOS
    constexpr auto undoModifier = Qt::MetaModifier;
#else
    constexpr auto undoModifier = Qt::ControlModifier;
#endif
    QTest::keyClick(window, Qt::Key_Z, undoModifier);
    REQUIRE(waitUntil([&controller]() -> bool {
        return controller.outlineEntries()
                       ->data(controller.outlineEntries()->index(1, 0),
                              OutlineEntryModel::DepthRole)
                       .toInt() == 0 &&
               controller.outlineEntries()
                       ->data(controller.outlineEntries()->index(1, 0),
                              OutlineEntryModel::AuthoredTextRole)
                       .toString() == QStringLiteral("child");
    }));
    childEntry = entryAt(1);
    REQUIRE(childEntry != nullptr);
    childEditor = childEntry->findChild<QQuickItem*>(QStringLiteral("outlineEntryEditor-1"));
    REQUIRE(childEditor != nullptr);
    childEditor->forceActiveFocus();
    QTest::keyClick(window, Qt::Key_Tab);
    REQUIRE(waitUntil([&controller]() -> bool {
        return controller.outlineEntries()
                   ->data(controller.outlineEntries()->index(1, 0), OutlineEntryModel::DepthRole)
                   .toInt() == 1;
    }));

    parentEntry = entryAt(0);
    childEntry = entryAt(1);
    REQUIRE(parentEntry != nullptr);
    REQUIRE(childEntry != nullptr);
    auto* parentEditor =
        parentEntry->findChild<QQuickItem*>(QStringLiteral("outlineEntryEditor-0"));
    childEditor = childEntry->findChild<QQuickItem*>(QStringLiteral("outlineEntryEditor-1"));
    REQUIRE(parentEditor != nullptr);
    REQUIRE(childEditor != nullptr);
    CHECK(childEditor->mapToScene({}).x() > parentEditor->mapToScene({}).x());
    CHECK(childEntry->property("contextMenu").value<QObject*>() != nullptr);

    childEditor->forceActiveFocus();
    childEditor->setProperty("cursorPosition", 2);
    QTest::keyClick(window, Qt::Key_Return);
    REQUIRE(
        waitUntil([outlineList]() -> bool { return outlineList->property("count").toInt() == 4; }));
    REQUIRE(waitUntil([window]() -> bool {
        return window->activeFocusItem() != nullptr &&
               window->activeFocusItem()->objectName() == QStringLiteral("outlineEntryEditor-2");
    }));
    auto* suffixEditor = window->activeFocusItem();
    CHECK(suffixEditor->property("cursorPosition").toInt() == 0);
    QTest::keyClick(window, Qt::Key_Backspace);
    REQUIRE(
        waitUntil([outlineList]() -> bool { return outlineList->property("count").toInt() == 3; }));
    REQUIRE(waitUntil([window]() -> bool {
        return window->activeFocusItem() != nullptr &&
               window->activeFocusItem()->objectName() == QStringLiteral("outlineEntryEditor-1");
    }));
    childEditor = window->activeFocusItem();
    CHECK(childEditor->property("cursorPosition").toInt() == 2);
    CHECK(controller.outlineEntryId(1) == childId);

    QTest::keyClick(window, Qt::Key_Backtab, Qt::ShiftModifier);
    REQUIRE(waitUntil([&controller]() -> bool {
        return controller.outlineEntries()
                   ->data(controller.outlineEntries()->index(1, 0), OutlineEntryModel::DepthRole)
                   .toInt() == 0;
    }));
    constexpr auto structureModifier = undoModifier;
    QTest::keyClick(window, Qt::Key_Down, structureModifier | Qt::ShiftModifier);
    REQUIRE(waitUntil(
        [&controller, &childId]() -> bool { return controller.outlineEntryId(2) == childId; }));
}

TEST_CASE("the Journal editor groups typing and routes standard undo and redo") {
    QTemporaryDir temporaryDirectory;
    REQUIRE(temporaryDirectory.isValid());
    NotebookController controller;
    controller.createNotebook(
        QUrl::fromLocalFile(temporaryDirectory.filePath(QStringLiteral("undo-ui.hieda"))));
    REQUIRE(controller.insertOutlineEntry(QStringLiteral("before")) == 0);

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
    auto* outlineList = root->findChild<QQuickItem*>(QStringLiteral("outlineList"));
    REQUIRE(outlineList != nullptr);
    REQUIRE(
        waitUntil([outlineList]() -> bool { return outlineList->property("count").toInt() == 1; }));
    QVariant entryValue;
    REQUIRE(QMetaObject::invokeMethod(outlineList, "entryItemAt",
                                      Q_RETURN_ARG(QVariant, entryValue), Q_ARG(QVariant, 0)));
    auto* entry = qobject_cast<QQuickItem*>(entryValue.value<QObject*>());
    REQUIRE(entry != nullptr);
    auto* editor = entry->findChild<QQuickItem*>(QStringLiteral("outlineEntryEditor-0"));
    REQUIRE(editor != nullptr);
    editor->forceActiveFocus();
    editor->setProperty("cursorPosition", 6);
    QTest::keyClick(window, Qt::Key_X);
    REQUIRE(waitUntil(
        [&controller]() -> bool {
            return controller.outlineEntries()
                       ->data(controller.outlineEntries()->index(0, 0),
                              OutlineEntryModel::AuthoredTextRole)
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
        return controller.outlineEntries()
                   ->data(controller.outlineEntries()->index(0, 0),
                          OutlineEntryModel::AuthoredTextRole)
                   .toString() == QStringLiteral("before");
    }));
    REQUIRE(waitUntil([window]() -> bool {
        return window->activeFocusItem() != nullptr &&
               window->activeFocusItem()->objectName() == QStringLiteral("outlineEntryEditor-0");
    }));
    CHECK(window->activeFocusItem()->property("cursorPosition").toInt() == 3);
    CHECK(redoAction->property("enabled").toBool());
    QTest::keyClick(window, Qt::Key_Z, undoModifier | Qt::ShiftModifier);
    REQUIRE(waitUntil([&controller]() -> bool {
        return controller.outlineEntries()
                   ->data(controller.outlineEntries()->index(0, 0),
                          OutlineEntryModel::AuthoredTextRole)
                   .toString() == QStringLiteral("beforex");
    }));

    REQUIRE(waitUntil([window]() -> bool {
        return window->activeFocusItem() != nullptr &&
               window->activeFocusItem()->objectName() == QStringLiteral("outlineEntryEditor-0");
    }));
    QTest::keyClick(window, Qt::Key_Y);
    const auto textBeforeClose = window->activeFocusItem()->property("text").toString();
    auto* closeAction = root->findChild<QObject*>(QStringLiteral("closeAction"));
    REQUIRE(closeAction != nullptr);
    REQUIRE(QMetaObject::invokeMethod(closeAction, "trigger"));
    REQUIRE(waitUntil([&controller]() -> bool { return !controller.hasOpenNotebook(); }));
    controller.openNotebook(
        QUrl::fromLocalFile(temporaryDirectory.filePath(QStringLiteral("undo-ui.hieda"))));
    REQUIRE(controller.outlineEntries()->rowCount() == 1);
    CHECK(controller.outlineEntries()
              ->data(controller.outlineEntries()->index(0, 0), OutlineEntryModel::AuthoredTextRole)
              .toString() == textBeforeClose);
}

TEST_CASE("the Journal editor inserts and pastes multiline Unicode before splitting on Enter") {
    QTemporaryDir temporaryDirectory;
    REQUIRE(temporaryDirectory.isValid());
    NotebookController controller;
    controller.createNotebook(
        QUrl::fromLocalFile(temporaryDirectory.filePath(QStringLiteral("multiline-ui.hieda"))));
    REQUIRE(controller.insertOutlineEntry(QStringLiteral("alpha")) == 0);

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
    auto* outlineList = root->findChild<QQuickItem*>(QStringLiteral("outlineList"));
    REQUIRE(outlineList != nullptr);
    REQUIRE(
        waitUntil([outlineList]() -> bool { return outlineList->property("count").toInt() == 1; }));
    QVariant entryValue;
    REQUIRE(QMetaObject::invokeMethod(outlineList, "entryItemAt",
                                      Q_RETURN_ARG(QVariant, entryValue), Q_ARG(QVariant, 0)));
    auto* entry = qobject_cast<QQuickItem*>(entryValue.value<QObject*>());
    REQUIRE(entry != nullptr);
    auto* editor = entry->findChild<QQuickItem*>(QStringLiteral("outlineEntryEditor-0"));
    REQUIRE(editor != nullptr);
    editor->forceActiveFocus();
    editor->setProperty("cursorPosition", 5);

    QTest::keyClick(window, Qt::Key_Return, Qt::ShiftModifier);
    REQUIRE(outlineList->property("count").toInt() == 1);
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
    const auto dragStart = editor->mapToScene(QPointF(2, editor->height() * 0.2));
    const auto dragEnd =
        editor->mapToScene(QPointF(std::min(editor->width() - 2, 80.0), editor->height() * 0.2));
    QTest::mousePress(window, Qt::LeftButton, Qt::NoModifier, dragStart.toPoint());
    QTest::mouseMove(window, dragEnd.toPoint());
    QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier, dragEnd.toPoint());
    CHECK(editor->property("selectionStart").toInt() != editor->property("selectionEnd").toInt());
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
            return controller.outlineEntries()
                       ->data(controller.outlineEntries()->index(0, 0),
                              OutlineEntryModel::AuthoredTextRole)
                       .toString() == QStringLiteral("alpha\n\u03B2\n\u65E5\u672C\u8A9E");
        },
        std::chrono::seconds(2)));

    const auto textBeforeModifiedEnter = editor->property("text").toString();
    const std::array<Qt::KeyboardModifiers, 4> modifiedEnterKeys{
        Qt::ControlModifier, Qt::AltModifier, Qt::MetaModifier,
        Qt::KeyboardModifiers{Qt::ControlModifier | Qt::ShiftModifier}};
    for (const auto modifier : modifiedEnterKeys) {
        QTest::keyClick(window, Qt::Key_Return, modifier);
        CHECK(outlineList->property("count").toInt() == 1);
        CHECK(editor->property("text").toString() == textBeforeModifiedEnter);
    }

    editor->setProperty("cursorPosition", 6);
    QTest::keyClick(window, Qt::Key_Return);
    REQUIRE(
        waitUntil([outlineList]() -> bool { return outlineList->property("count").toInt() == 2; }));
    CHECK(controller.outlineEntries()
              ->data(controller.outlineEntries()->index(0, 0), OutlineEntryModel::AuthoredTextRole)
              .toString() == QStringLiteral("alpha\n"));
    CHECK(controller.outlineEntries()
              ->data(controller.outlineEntries()->index(1, 0), OutlineEntryModel::AuthoredTextRole)
              .toString() == QStringLiteral("\u03B2\n\u65E5\u672C\u8A9E"));
}

TEST_CASE("Journal bullets select and cut complete subtrees with accessible clipboard text") {
    QTemporaryDir temporaryDirectory;
    REQUIRE(temporaryDirectory.isValid());
    NotebookController controller;
    controller.createNotebook(
        QUrl::fromLocalFile(temporaryDirectory.filePath(QStringLiteral("selection-ui.hieda"))));
    REQUIRE(controller.insertOutlineEntry(QStringLiteral("parent\ncontinuation")) == 0);
    REQUIRE(controller.insertOutlineEntry(QStringLiteral("child")) == 1);
    REQUIRE(controller.insertOutlineEntry(QStringLiteral("tail")) == 2);
    REQUIRE(controller.indentOutlineEntry(controller.outlineEntryId(1), QStringLiteral("child"), 5)
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
    auto* outlineList = root->findChild<QQuickItem*>(QStringLiteral("outlineList"));
    REQUIRE(outlineList != nullptr);
    REQUIRE(
        waitUntil([outlineList]() -> bool { return outlineList->property("count").toInt() == 3; }));

    auto bulletAt = [outlineList](int row) -> QQuickItem* {
        QVariant entryValue;
        if (!QMetaObject::invokeMethod(outlineList, "entryItemAt",
                                       Q_RETURN_ARG(QVariant, entryValue), Q_ARG(QVariant, row))) {
            return nullptr;
        }
        auto* entry = qobject_cast<QQuickItem*>(entryValue.value<QObject*>());
        return entry == nullptr ? nullptr
                                : entry->findChild<QQuickItem*>(
                                      QStringLiteral("outlineEntryBullet-") + QString::number(row));
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
    QTest::keyClick(window, Qt::Key_Down, Qt::ShiftModifier);
    REQUIRE(waitUntil([root = root.get()]() -> bool {
        return root->property("outlineSelectionCount").toInt() == 3;
    }));
    QTest::keyClick(window, Qt::Key_Up, Qt::ShiftModifier);
    REQUIRE(waitUntil([root = root.get()]() -> bool {
        return root->property("outlineSelectionCount").toInt() == 2;
    }));
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, parentCenter.toPoint());
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
        waitUntil([outlineList]() -> bool { return outlineList->property("count").toInt() == 0; }));
    REQUIRE(waitUntil([window]() -> bool {
        return window->activeFocusItem() != nullptr &&
               window->activeFocusItem()->objectName() == QStringLiteral("journalDraftEditor");
    }));
    REQUIRE(controller.undoOutlineEdit().value(QStringLiteral("succeeded")).toBool());
    CHECK(controller.outlineEntries()->rowCount() == 3);
}

TEST_CASE("Up and Down move within multiline Entries before crossing outline boundaries") {
    QTemporaryDir temporaryDirectory;
    REQUIRE(temporaryDirectory.isValid());
    NotebookController controller;
    controller.createNotebook(
        QUrl::fromLocalFile(temporaryDirectory.filePath(QStringLiteral("arrow-ui.hieda"))));
    REQUIRE(controller.insertOutlineEntry(QStringLiteral("first\nline")) == 0);
    REQUIRE(controller.insertOutlineEntry(QStringLiteral("second\nline")) == 1);

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
    auto* outlineList = root->findChild<QQuickItem*>(QStringLiteral("outlineList"));
    REQUIRE(outlineList != nullptr);
    REQUIRE(
        waitUntil([outlineList]() -> bool { return outlineList->property("count").toInt() == 2; }));
    QVariant firstValue;
    REQUIRE(QMetaObject::invokeMethod(outlineList, "entryItemAt",
                                      Q_RETURN_ARG(QVariant, firstValue), Q_ARG(QVariant, 0)));
    auto* firstEntry = qobject_cast<QQuickItem*>(firstValue.value<QObject*>());
    REQUIRE(firstEntry != nullptr);
    auto* firstEditor = firstEntry->findChild<QQuickItem*>(QStringLiteral("outlineEntryEditor-0"));
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
               window->activeFocusItem()->objectName() == QStringLiteral("outlineEntryEditor-1");
    }));
    auto* secondEditor = window->activeFocusItem();
    CHECK(secondEditor->property("cursorPosition").toInt() < 7);
    QTest::keyClick(window, Qt::Key_Up);
    REQUIRE(waitUntil([window]() -> bool {
        return window->activeFocusItem() != nullptr &&
               window->activeFocusItem()->objectName() == QStringLiteral("outlineEntryEditor-0");
    }));
    CHECK(window->activeFocusItem()->property("cursorPosition").toInt() > 5);
}

TEST_CASE("IME preedit commits and cancels without premature Journal persistence") {
    QTemporaryDir temporaryDirectory;
    REQUIRE(temporaryDirectory.isValid());
    NotebookController controller;
    controller.createNotebook(
        QUrl::fromLocalFile(temporaryDirectory.filePath(QStringLiteral("ime-ui.hieda"))));
    REQUIRE(controller.insertOutlineEntry(QStringLiteral("base")) == 0);
    REQUIRE(controller.insertOutlineEntry(QStringLiteral("other")) == 1);

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
    auto* outlineList = root->findChild<QQuickItem*>(QStringLiteral("outlineList"));
    REQUIRE(outlineList != nullptr);
    REQUIRE(
        waitUntil([outlineList]() -> bool { return outlineList->property("count").toInt() == 2; }));
    QVariant firstValue;
    QVariant secondValue;
    REQUIRE(QMetaObject::invokeMethod(outlineList, "entryItemAt",
                                      Q_RETURN_ARG(QVariant, firstValue), Q_ARG(QVariant, 0)));
    REQUIRE(QMetaObject::invokeMethod(outlineList, "entryItemAt",
                                      Q_RETURN_ARG(QVariant, secondValue), Q_ARG(QVariant, 1)));
    auto* firstEntry = qobject_cast<QQuickItem*>(firstValue.value<QObject*>());
    auto* secondEntry = qobject_cast<QQuickItem*>(secondValue.value<QObject*>());
    REQUIRE(firstEntry != nullptr);
    REQUIRE(secondEntry != nullptr);
    auto* firstEditor = firstEntry->findChild<QQuickItem*>(QStringLiteral("outlineEntryEditor-0"));
    auto* secondEditor =
        secondEntry->findChild<QQuickItem*>(QStringLiteral("outlineEntryEditor-1"));
    REQUIRE(firstEditor != nullptr);
    REQUIRE(secondEditor != nullptr);
    firstEditor->forceActiveFocus();
    firstEditor->setProperty("cursorPosition", 4);

    QInputMethodEvent preedit(QStringLiteral("\u65E5"), {});
    QCoreApplication::sendEvent(firstEditor, &preedit);
    REQUIRE(firstEditor->property("inputMethodComposing").toBool());
    QTest::keyClick(window, Qt::Key_Return);
    CHECK(controller.outlineEntries()->rowCount() == 2);
    QTest::qWait(1100);
    CHECK(controller.outlineEntries()
              ->data(controller.outlineEntries()->index(0, 0), OutlineEntryModel::AuthoredTextRole)
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
            return controller.outlineEntries()
                       ->data(controller.outlineEntries()->index(0, 0),
                              OutlineEntryModel::AuthoredTextRole)
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
    CHECK(controller.outlineEntries()->rowCount() == 2);
}

TEST_CASE("outline selection refuses to discard a rejected pending edit") {
    QTemporaryDir temporaryDirectory;
    REQUIRE(temporaryDirectory.isValid());
    NotebookController controller;
    controller.createNotebook(QUrl::fromLocalFile(
        temporaryDirectory.filePath(QStringLiteral("selection-save-ui.hieda"))));
    REQUIRE(controller.insertOutlineEntry(QStringLiteral("durable")) == 0);

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
    auto* outlineList = root->findChild<QQuickItem*>(QStringLiteral("outlineList"));
    REQUIRE(outlineList != nullptr);
    REQUIRE(
        waitUntil([outlineList]() -> bool { return outlineList->property("count").toInt() == 1; }));
    QVariant entryValue;
    REQUIRE(QMetaObject::invokeMethod(outlineList, "entryItemAt",
                                      Q_RETURN_ARG(QVariant, entryValue), Q_ARG(QVariant, 0)));
    auto* entry = qobject_cast<QQuickItem*>(entryValue.value<QObject*>());
    REQUIRE(entry != nullptr);
    auto* editor = entry->findChild<QQuickItem*>(QStringLiteral("outlineEntryEditor-0"));
    REQUIRE(editor != nullptr);
    editor->forceActiveFocus();
    editor->setProperty("text", QStringLiteral("unsaved"));
    entry->setProperty("entryId", QStringLiteral("missing-entry"));
    QVariant selected;

    REQUIRE(QMetaObject::invokeMethod(root.get(), "selectOutline", Q_RETURN_ARG(QVariant, selected),
                                      Q_ARG(QVariant, 0), Q_ARG(QVariant, false)));

    CHECK_FALSE(selected.toBool());
    CHECK(root->property("outlineSelectionCount").toInt() == 0);
    CHECK(editor->hasActiveFocus());
    CHECK(editor->property("text").toString() == QStringLiteral("durable"));
    CHECK(controller.outlineEntries()
              ->data(controller.outlineEntries()->index(0, 0), OutlineEntryModel::AuthoredTextRole)
              .toString() == QStringLiteral("durable"));
    CHECK_FALSE(controller.errorMessage().isEmpty());
}

TEST_CASE("the Journal exposes list structure selection and multiline editing accessibly") {
    QAccessible::setActive(true);
    QTemporaryDir temporaryDirectory;
    REQUIRE(temporaryDirectory.isValid());
    NotebookController controller;
    controller.createNotebook(
        QUrl::fromLocalFile(temporaryDirectory.filePath(QStringLiteral("accessible-ui.hieda"))));
    REQUIRE(controller.insertOutlineEntry(QStringLiteral("accessible\ntext")) == 0);

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
    auto* outlineList = root->findChild<QQuickItem*>(QStringLiteral("outlineList"));
    REQUIRE(outlineList != nullptr);
    REQUIRE(
        waitUntil([outlineList]() -> bool { return outlineList->property("count").toInt() == 1; }));
    QVariant entryValue;
    REQUIRE(QMetaObject::invokeMethod(outlineList, "entryItemAt",
                                      Q_RETURN_ARG(QVariant, entryValue), Q_ARG(QVariant, 0)));
    auto* entry = qobject_cast<QQuickItem*>(entryValue.value<QObject*>());
    REQUIRE(entry != nullptr);
    auto* bullet = entry->findChild<QQuickItem*>(QStringLiteral("outlineEntryBullet-0"));
    auto* editor = entry->findChild<QQuickItem*>(QStringLiteral("outlineEntryEditor-0"));
    REQUIRE(bullet != nullptr);
    REQUIRE(editor != nullptr);
    QVariant selected;
    REQUIRE(QMetaObject::invokeMethod(root.get(), "selectOutline", Q_RETURN_ARG(QVariant, selected),
                                      Q_ARG(QVariant, 0), Q_ARG(QVariant, false)));
    REQUIRE(waitUntil([root = root.get()]() -> bool {
        return root->property("outlineSelectionCount").toInt() == 1;
    }));
    REQUIRE(waitUntil([bullet]() -> bool { return bullet->hasActiveFocus(); }));

    auto* listInterface = QAccessible::queryAccessibleInterface(outlineList);
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
