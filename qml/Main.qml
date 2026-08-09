// SPDX-License-Identifier: MPL-2.0
import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts

ApplicationWindow {
    id: window

    readonly property real uiSpacing: Math.max(6, Math.round(font.pixelSize * 0.6))
    readonly property real maximumDocumentWidth: 780
    readonly property int structureModifier: Qt.platform.os === "osx" ? Qt.MetaModifier : Qt.ControlModifier
    property string draftAfterId: ""
    property string draftText: ""
    property var activeOutlineEditor: null
    property bool rolloverPending: false
    property bool deferJournalFocusCommit: false
    property var outlineSelectionIds: []
    property var outlineSelectionRoots: []
    property int outlineSelectionAnchorRow: -1
    property int outlineSelectionExtentRow: -1
    property var pageRenameReturnEditor: null
    property var hierarchyExpandedNames: []
    readonly property bool hasPendingOutlineEdit: activeOutlineEditor && activeOutlineEditor.hasPendingEdit
    readonly property int outlineSelectionCount: outlineSelectionIds.length

    signal draftFocusRequested(string afterId)

    Connections {
        target: notebookController
        function onStateChanged() {
            if (!notebookController.hasOpenNotebook)
                window.hierarchyExpandedNames = [];
        }
        function onDestinationChanged() {
            Qt.callLater(window.restoreHierarchyExpansion);
        }
    }

    function restoreHierarchyExpansion() {
        if (!pageList || !notebookController.hasOpenNotebook)
            return ;

        hierarchyExpandedNames.forEach(function(name) {
            const modelIndex = notebookController.pageHierarchy.indexForPageName(name);
            if (modelIndex.valid)
                pageList.expandToIndex(modelIndex);
        });
        const currentIndex = notebookController.pageHierarchy.indexForPageName(notebookController.currentPageName);
        if (currentIndex.valid)
            pageList.expandToIndex(currentIndex);
    }

    function beginDraft(afterId) {
        if (notebookController.currentPagePreview)
            return ;

        draftText = "";
        draftAfterId = afterId;
        Qt.callLater(function() {
            draftFocusRequested(afterId);
        });
    }

    function beginTrailingDraft() {
        if (notebookController.currentPagePreview)
            return ;

        if (activeOutlineEditor && activeOutlineEditor.isDraftEditor && !activeOutlineEditor.commit(false))
            return ;

        beginDraft("");
    }

    function focusOutlineEntryWhenAvailable(row, focusAction) {
        if (row < 0 || row >= outlineList.count)
            return ;

        outlineList.positionViewAtIndex(row, ListView.Contain);
        const currentItem = outlineList.itemAtIndex(row);
        if (currentItem) {
            focusAction(currentItem);
            return ;
        }
        Qt.callLater(function() {
            const item = outlineList.itemAtIndex(row);
            if (item)
                focusAction(item);

        });
    }

    function focusEntry(row, cursorPosition) {
        focusOutlineEntryWhenAvailable(row, function(item) {
            item.focusEditor(cursorPosition);
        });
    }

    function focusEntryBoundary(row, firstLine, horizontalPosition) {
        focusOutlineEntryWhenAvailable(row, function(item) {
            item.focusEditorAtBoundary(firstLine, horizontalPosition);
        });
    }

    function cursorOnFirstVisualLine(editor) {
        return editor.cursorRectangle.y <= editor.topPadding + 0.5;
    }

    function cursorOnLastVisualLine(editor) {
        return editor.cursorRectangle.y + editor.cursorRectangle.height >= editor.contentHeight - editor.bottomPadding - 0.5;
    }

    function textModifiers(modifiers) {
        return modifiers & ~Qt.KeypadModifier;
    }

    function applyOutlineOutcome(outcome) {
        if (!outcome.succeeded)
            return false;

        clearOutlineSelection();
        draftAfterId = "";
        draftText = "";
        if (outcome.row >= 0)
            focusEntry(outcome.row, outcome.cursorPosition);

        return true;
    }

    function registerOutlineEditor(editor) {
        activeOutlineEditor = editor;
    }

    function clearOutlineSelection() {
        outlineSelectionIds = [];
        outlineSelectionRoots = [];
        outlineSelectionAnchorRow = -1;
        outlineSelectionExtentRow = -1;
    }

    function isOutlineSelected(entryId) {
        return outlineSelectionIds.indexOf(entryId) >= 0;
    }

    function focusBullet(row) {
        if (row < 0 || row >= outlineList.count)
            return ;

        outlineList.positionViewAtIndex(row, ListView.Contain);
        const currentItem = outlineList.itemAtIndex(row);
        if (currentItem)
            currentItem.focusBullet();

        Qt.callLater(function() {
            const item = outlineList.itemAtIndex(row);
            if (item)
                item.focusBullet();

        });
    }

    function selectOutline(row, extendSelection) {
        if (row < 0 || row >= outlineList.count || !commitActiveOutlineEditor())
            return false;

        if (!extendSelection || outlineSelectionAnchorRow < 0)
            outlineSelectionAnchorRow = row;

        outlineSelectionExtentRow = row;
        const selection = notebookController.outlineEntrySelection(outlineSelectionAnchorRow, row);
        outlineSelectionRoots = selection.roots;
        outlineSelectionIds = selection.entries;
        focusBullet(row);
        return true;
    }

    function copyOutlineSelection() {
        if (outlineSelectionRoots.length === 0)
            return false;

        const text = notebookController.outlineSelectionText(outlineSelectionRoots);
        if (text.length === 0)
            return false;

        notebookController.copyTextToClipboard(text);
        return true;
    }

    function cutOutlineSelection() {
        if (notebookController.currentPagePreview)
            return ;

        if (!copyOutlineSelection())
            return ;

        const roots = outlineSelectionRoots;
        const outcome = notebookController.deleteOutlineSubtrees(roots);
        if (!outcome.succeeded)
            return ;

        clearOutlineSelection();
        if (outcome.row >= 0)
            focusEntry(outcome.row, outcome.cursorPosition);
        else
            beginTrailingDraft();
    }

    function commitActiveOutlineEditor() {
        const editor = activeOutlineEditor;
        if (editor && editor.inputMethodComposing) {
            editor.forceActiveFocus();
            return false;
        }
        activeOutlineEditor = null;
        if (editor && editor.hasPendingEdit && !editor.commit(false)) {
            registerOutlineEditor(editor);
            return false;
        }
        return true;
    }

    function applyHistory(action) {
        const redo = action === redoAction;
        const editor = activeOutlineEditor;
        const preferredEntryId = editor ? editor.outlineEntryId : "";
        const cursorPosition = editor ? editor.cursorPosition : 0;
        if (!commitActiveOutlineEditor())
            return ;

        if (redo ? !notebookController.canRedo : !notebookController.canUndo)
            return ;

        applyOutlineOutcome(redo ? notebookController.redoOutlineEdit(preferredEntryId, cursorPosition) : notebookController.undoOutlineEdit(preferredEntryId, cursorPosition));
    }

    function handleHistoryKey(event) {
        if (!(event.modifiers & window.structureModifier))
            return false;

        if (event.key === Qt.Key_Z) {
            window.applyHistory((event.modifiers & Qt.ShiftModifier) !== 0 ? redoAction : undoAction);
            return true;
        }
        if (event.key === Qt.Key_Y) {
            window.applyHistory(redoAction);
            return true;
        }
        return false;
    }

    function outlineEditorFinished(editor) {
        if (activeOutlineEditor === editor)
            activeOutlineEditor = null;

        if (rolloverPending)
            Qt.callLater(function() {
            completeDeferredRollover(true);
        });

    }

    function restorePageRenameFocus() {
        const editor = pageRenameReturnEditor;
        pageRenameReturnEditor = null;
        if (editor)
            Qt.callLater(function() { editor.forceActiveFocus(); });
    }

    function completeDeferredRollover(focusDraft) {
        if (!rolloverPending)
            return ;

        rolloverPending = false;
        draftAfterId = "";
        draftText = "";
        notebookController.completeJournalDateRollover();
        if (focusDraft)
            Qt.callLater(function() {
            beginTrailingDraft();
        });

    }

    width: 760
    height: 520
    minimumWidth: 520
    minimumHeight: 360
    visible: true
    title: notebookController.hasOpenNotebook ? qsTr("%1 — Hieda").arg(notebookController.notebookName) : qsTr("Hieda")

    Connections {
        function onJournalDateRolloverRequested() {
            window.rolloverPending = true;
            if (!window.activeOutlineEditor)
                window.completeDeferredRollover(false);

        }

        target: notebookController
    }

    Action {
        id: createAction

        text: qsTr("&New Notebook…")
        icon.name: "document-new"
        shortcut: StandardKey.New
        enabled: !notebookController.hasOpenNotebook
        onTriggered: createDialog.open()
    }

    Action {
        id: openAction

        text: qsTr("&Open Notebook…")
        icon.name: "document-open"
        shortcut: StandardKey.Open
        enabled: !notebookController.hasOpenNotebook
        onTriggered: openDialog.open()
    }

    Action {
        id: closeAction

        objectName: "closeAction"
        text: qsTr("&Close Notebook")
        icon.name: "window-close"
        shortcut: StandardKey.Close
        enabled: notebookController.hasOpenNotebook
        onTriggered: {
            if (window.commitActiveOutlineEditor())
                notebookController.closeNotebook();
        }
    }

    Action {
        id: quitAction

        text: qsTr("&Quit")
        icon.name: "application-exit"
        shortcut: StandardKey.Quit
        onTriggered: {
            if (window.commitActiveOutlineEditor())
                Qt.quit();
        }
    }

    Action {
        id: undoAction

        objectName: "undoAction"
        text: qsTr("&Undo")
        icon.name: "edit-undo"
        shortcut: StandardKey.Undo
        enabled: notebookController.hasOpenNotebook && (notebookController.canUndo || window.hasPendingOutlineEdit)
        onTriggered: window.applyHistory(undoAction)
    }

    Action {
        id: redoAction

        objectName: "redoAction"
        text: qsTr("&Redo")
        icon.name: "edit-redo"
        shortcut: StandardKey.Redo
        enabled: notebookController.hasOpenNotebook && notebookController.canRedo && !window.hasPendingOutlineEdit
        onTriggered: window.applyHistory(redoAction)
    }

    Action {
        id: newPageAction
        objectName: "newPageAction"
        text: qsTr("&New Page…")
        enabled: notebookController.hasOpenNotebook
        onTriggered: newPageDialog.open()
    }

    Action {
        id: goToPageAction
        objectName: "goToPageAction"
        text: qsTr("&Go to Page…")
        enabled: notebookController.hasOpenNotebook
        onTriggered: goToPageDialog.open()
    }

    Action {
        id: renamePageAction
        objectName: "renamePageAction"
        text: qsTr("&Rename Page…")
        enabled: notebookController.hasOpenNotebook && !notebookController.isJournalPage
        onTriggered: {
            window.pageRenameReturnEditor = window.activeOutlineEditor;
            renamePageName.text = notebookController.currentPageName;
            renamePageTitle.text = notebookController.currentPageTitle;
            renamePageDialog.open();
        }
    }

    Action {
        id: cutAction

        objectName: "cutAction"
        text: qsTr("Cu&t")
        icon.name: "edit-cut"
        shortcut: StandardKey.Cut
        enabled: !notebookController.currentPagePreview && (window.outlineSelectionCount > 0 || (window.activeOutlineEditor && window.activeOutlineEditor.activeFocus && window.activeOutlineEditor.selectedText.length > 0))
        onTriggered: {
            if (window.outlineSelectionCount > 0)
                window.cutOutlineSelection();
            else if (window.activeOutlineEditor)
                window.activeOutlineEditor.cut();
        }
    }

    Action {
        id: copyAction

        objectName: "copyAction"
        text: qsTr("&Copy")
        icon.name: "edit-copy"
        shortcut: StandardKey.Copy
        enabled: window.outlineSelectionCount > 0 || (window.activeOutlineEditor && window.activeOutlineEditor.activeFocus && window.activeOutlineEditor.selectedText.length > 0)
        onTriggered: {
            if (window.outlineSelectionCount > 0)
                window.copyOutlineSelection();
            else if (window.activeOutlineEditor)
                window.activeOutlineEditor.copy();
        }
    }

    Action {
        id: pasteAction

        objectName: "pasteAction"
        text: qsTr("&Paste")
        icon.name: "edit-paste"
        shortcut: StandardKey.Paste
        enabled: window.activeOutlineEditor && window.activeOutlineEditor.activeFocus && window.activeOutlineEditor.canPaste
        onTriggered: window.activeOutlineEditor.paste()
    }

    Action {
        id: selectAllAction

        objectName: "selectAllAction"
        text: qsTr("Select &All")
        shortcut: StandardKey.SelectAll
        enabled: notebookController.hasOpenNotebook && (outlineList.count > 0 || window.activeOutlineEditor)
        onTriggered: {
            if (window.activeOutlineEditor && window.activeOutlineEditor.activeFocus) {
                window.clearOutlineSelection();
                window.activeOutlineEditor.selectAll();
            } else if (outlineList.count > 0) {
                window.outlineSelectionAnchorRow = 0;
                window.selectOutline(outlineList.count - 1, true);
            }
        }
    }

    FileDialog {
        id: createDialog

        title: qsTr("Create a Notebook")
        fileMode: FileDialog.SaveFile
        defaultSuffix: "hieda"
        nameFilters: [qsTr("Hieda Notebooks (*.hieda)"), qsTr("All files (*)")]
        onAccepted: notebookController.createNotebook(selectedFile)
    }

    Dialog {
        id: newPageDialog
        objectName: "newPageDialog"
        title: qsTr("New Page")
        modal: true
        anchors.centerIn: parent
        standardButtons: Dialog.Ok | Dialog.Cancel
        onAccepted: {
            if (!window.commitActiveOutlineEditor() || !notebookController.createPage(newPageName.text, newPageTitle.text))
                Qt.callLater(function() { newPageDialog.open(); });
            else {
                newPageName.clear();
                newPageTitle.clear();
            }
        }
        ColumnLayout {
            Label { text: qsTr("Name") }
            TextField { id: newPageName; objectName: "newPageName"; placeholderText: qsTr("project_name") }
            Label { text: qsTr("Display title") }
            TextField { id: newPageTitle; objectName: "newPageTitle" }
        }
    }

    Dialog {
        id: renamePageDialog
        objectName: "renamePageDialog"
        title: qsTr("Rename Page")
        modal: true
        anchors.centerIn: parent
        standardButtons: Dialog.Ok | Dialog.Cancel
        onAccepted: {
            if (!window.commitActiveOutlineEditor() || !notebookController.renameCurrentPage(renamePageName.text, renamePageTitle.text))
                Qt.callLater(function() { renamePageDialog.open(); });
            else
                window.restorePageRenameFocus();
        }
        onRejected: window.restorePageRenameFocus()
        ColumnLayout {
            Label { text: qsTr("Name") }
            TextField { id: renamePageName; objectName: "renamePageName" }
            Label { text: qsTr("Display title") }
            TextField { id: renamePageTitle; objectName: "renamePageTitle" }
        }
    }

    Dialog {
        id: materializePageDialog
        objectName: "materializePageDialog"
        title: qsTr("Create Page")
        modal: true
        anchors.centerIn: parent
        standardButtons: Dialog.Ok | Dialog.Cancel
        onAccepted: {
            if (!notebookController.createCurrentPage(materializePageTitle.text))
                Qt.callLater(function() { materializePageDialog.open(); });
        }
        ColumnLayout {
            Label {
                Layout.fillWidth: true
                text: qsTr("Create %1").arg(notebookController.currentPageName)
                wrapMode: Text.Wrap
            }
            Label { text: qsTr("Display title") }
            TextField {
                id: materializePageTitle
                objectName: "materializePageTitle"
                Layout.fillWidth: true
            }
        }
    }

    Dialog {
        id: goToPageDialog
        objectName: "goToPageDialog"
        title: qsTr("Go to Page")
        modal: true
        anchors.centerIn: parent
        standardButtons: Dialog.Close
        ColumnLayout {
            TextField {
                id: pageFilterField
                objectName: "pageFilterField"
                Layout.fillWidth: true
                placeholderText: qsTr("Filter Pages")
            }
            ComboBox {
                id: pagePicker
                objectName: "pagePicker"
                Layout.fillWidth: true
                model: notebookController.pageChoices.filter(function(choice) {
                    return choice.toLowerCase().includes(pageFilterField.text.toLowerCase());
                })
            }
            Button {
                objectName: "openSelectedPageButton"
                text: qsTr("Open selected Page")
                enabled: notebookController.pageIdForChoice(pagePicker.currentText).length > 0
                onClicked: {
                    if (window.commitActiveOutlineEditor()) {
                        notebookController.navigateToPage(notebookController.pageIdForChoice(pagePicker.currentText));
                        goToPageDialog.close();
                    }
                }
            }
            TextField {
                id: journalDateField
                objectName: "journalDateField"
                placeholderText: qsTr("YYYY-MM-DD")
            }
            Button {
                text: qsTr("Open Journal date")
                onClicked: {
                    if (window.commitActiveOutlineEditor()) {
                        notebookController.navigateToJournalDateText(journalDateField.text);
                        if (notebookController.errorMessage.length === 0)
                            goToPageDialog.close();
                    }
                }
            }
        }
    }

    FileDialog {
        id: openDialog

        title: qsTr("Open a Notebook")
        fileMode: FileDialog.OpenFile
        nameFilters: [qsTr("Hieda Notebooks (*.hieda)"), qsTr("All files (*)")]
        onAccepted: notebookController.openNotebook(selectedFile)
    }

    Page {
        anchors.fill: parent

        ColumnLayout {
            anchors.fill: parent
            spacing: 0

            Frame {
                Layout.fillWidth: true
                Layout.margins: window.uiSpacing
                visible: notebookController.errorMessage.length > 0

                RowLayout {
                    anchors.fill: parent
                    spacing: window.uiSpacing

                    Label {
                        text: qsTr("Error")
                        font.bold: true
                    }

                    Label {
                        Layout.fillWidth: true
                        text: notebookController.errorMessage
                        wrapMode: Text.Wrap
                    }

                    Button {
                        text: qsTr("Dismiss")
                        onClicked: notebookController.clearError()
                    }

                }

            }

            Item {
                Layout.fillWidth: true
                Layout.fillHeight: true

                ColumnLayout {
                    anchors.centerIn: parent
                    width: Math.min(parent.width - (window.uiSpacing * 8), 560)
                    spacing: window.uiSpacing * 2
                    visible: !notebookController.hasOpenNotebook

                    Label {
                        Layout.fillWidth: true
                        horizontalAlignment: Text.AlignHCenter
                        text: qsTr("Welcome to Hieda")
                        font.pixelSize: Math.round(window.font.pixelSize * 1.5)
                        font.weight: Font.DemiBold
                    }

                    Label {
                        Layout.fillWidth: true
                        horizontalAlignment: Text.AlignHCenter
                        wrapMode: Text.Wrap
                        text: qsTr("Create a portable Notebook or open one you already have.")
                    }

                    RowLayout {
                        Layout.alignment: Qt.AlignHCenter
                        spacing: window.uiSpacing

                        Button {
                            action: createAction
                            highlighted: true
                        }

                        Button {
                            action: openAction
                        }

                    }

                }

                Frame {
                    id: pageSidebar
                    objectName: "pageSidebar"
                    anchors.left: parent.left
                    anchors.top: parent.top
                    anchors.bottom: parent.bottom
                    width: Math.min(240, Math.max(180, parent.width * 0.24))
                    visible: notebookController.hasOpenNotebook

                    ColumnLayout {
                        anchors.fill: parent
                        Label { text: qsTr("Journal"); font.bold: true }
                        Label {
                            Layout.fillWidth: true
                            visible: notebookController.isJournalPage
                            text: qsTr("Current: %1").arg(Qt.formatDate(notebookController.journalDate, Locale.ShortFormat))
                            font.bold: true
                        }
                        ColumnLayout {
                            Layout.fillWidth: true
                            Button {
                                objectName: "previousJournalButton"
                                Layout.fillWidth: true
                                text: qsTr("Previous")
                                onClicked: {
                                    if (window.commitActiveOutlineEditor())
                                        notebookController.navigateToPreviousJournalDate();
                                }
                            }
                            Button {
                                objectName: "todayJournalButton"
                                Layout.fillWidth: true
                                text: qsTr("Today")
                                highlighted: notebookController.isJournalPage && Qt.formatDate(notebookController.journalDate, "yyyy-MM-dd") === Qt.formatDate(new Date(), "yyyy-MM-dd")
                                onClicked: {
                                    if (window.commitActiveOutlineEditor())
                                        notebookController.navigateToToday();
                                }
                            }
                            Button {
                                objectName: "nextJournalButton"
                                Layout.fillWidth: true
                                text: qsTr("Next")
                                onClicked: {
                                    if (window.commitActiveOutlineEditor())
                                        notebookController.navigateToNextJournalDate();
                                }
                            }
                        }
                        Label { text: qsTr("Pages"); font.bold: true }
                        TreeView {
                            id: pageList

                            function pageItemAt(row) {
                                return itemAtCell(Qt.point(0, row));
                            }


                            objectName: "pageList"
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            model: notebookController.pageHierarchy
                            clip: true
                            columnWidthProvider: function(column) { return pageList.width; }
                            Accessible.name: qsTr("Page Hierarchy")
                            onExpanded: function(row, depth) {
                                const name = notebookController.pageHierarchy.pageName(pageList.index(row, 0));
                                if (hierarchyExpandedNames.indexOf(name) < 0)
                                    hierarchyExpandedNames = hierarchyExpandedNames.concat([name]);
                            }
                            onCollapsed: function(row, recursively) {
                                const name = notebookController.pageHierarchy.pageName(pageList.index(row, 0));
                                hierarchyExpandedNames = hierarchyExpandedNames.filter(function(candidate) { return candidate !== name; });
                            }
                            onRowsChanged: Qt.callLater(window.restoreHierarchyExpansion)
                            Component.onCompleted: Qt.callLater(window.restoreHierarchyExpansion)
                            delegate: TreeViewDelegate {
                                required property string pageName
                                required property bool revealExpanded
                                required property bool currentDestination
                                required property string accessibleDescription
                                text: model.display
                                highlighted: currentDestination
                                Accessible.name: text
                                Accessible.description: accessibleDescription
                                Component.onCompleted: {
                                    if (revealExpanded)
                                        Qt.callLater(function() { pageList.expand(row); });
                                }
                                onClicked: {
                                    if (window.commitActiveOutlineEditor())
                                        notebookController.navigateToPageName(pageName);
                                }
                            }
                        }
                        Button {
                            objectName: "newPageButton"
                            action: newPageAction
                            Layout.fillWidth: true
                        }
                    }
                }

                ListView {
                    id: outlineList

                    function entryItemAt(row) {
                        return itemAtIndex(row);
                    }

                    objectName: "outlineList"
                    anchors.top: parent.top
                    anchors.bottom: parent.bottom
                    x: pageSidebar.width + Math.max(window.uiSpacing * 2, (parent.width - pageSidebar.width - width) / 2)
                    width: Math.min(parent.width - pageSidebar.width - (window.uiSpacing * 4), window.maximumDocumentWidth)
                    visible: notebookController.hasOpenNotebook
                    clip: true
                    model: notebookController.outlineEntries
                    currentIndex: -1
                    boundsBehavior: Flickable.StopAtBounds
                    spacing: Math.round(window.uiSpacing * 0.35)
                    Accessible.role: Accessible.List
                    Accessible.name: notebookController.isJournalPage ? qsTr("Journal Entries") : qsTr("Page Entries")

                    ScrollBar.vertical: ScrollBar {
                    }

                    header: Item {
                        width: outlineList.width
                        height: dateHeading.implicitHeight + destinationActions.implicitHeight +
                            (window.uiSpacing * 7) + (notebookController.currentPagePreview ?
                            previewSourcesHeading.implicitHeight + pagePreviewSourceList.height +
                            (window.uiSpacing * 3) : 0)

                        Label {
                            id: dateHeading
                            objectName: "pageHeading"

                            width: Math.min(parent.width - (window.uiSpacing * 8), window.maximumDocumentWidth)
                            anchors.horizontalCenter: parent.horizontalCenter
                            anchors.top: parent.top
                            anchors.topMargin: window.uiSpacing * 4
                            text: notebookController.isJournalPage ? Qt.formatDate(notebookController.journalDate, Locale.LongFormat) : notebookController.currentPagePreview ? qsTr("%1 — Page Preview").arg(notebookController.currentPageName) : qsTr("%1 — %2").arg(notebookController.currentPageTitle).arg(notebookController.currentPageName)
                            font.pixelSize: Math.round(window.font.pixelSize * 1.7)
                            font.weight: Font.DemiBold
                            Accessible.name: notebookController.isJournalPage ? qsTr("Journal Page for %1").arg(text) : notebookController.currentPagePreview ? qsTr("Page Preview for full Page Name %1, not materialized").arg(notebookController.currentPageName) : qsTr("Page title %1, name %2").arg(notebookController.currentPageTitle).arg(notebookController.currentPageName)
                        }

                        RowLayout {
                            id: destinationActions
                            anchors.top: dateHeading.bottom
                            anchors.topMargin: window.uiSpacing
                            anchors.horizontalCenter: parent.horizontalCenter
                            Button {
                                objectName: "materializePageButton"
                                visible: notebookController.currentPagePreview
                                text: qsTr("Create this Page")
                                onClicked: {
                                    materializePageTitle.text = notebookController.currentPageName.split("/").pop();
                                    materializePageDialog.open();
                                }
                            }
                            Button {
                                objectName: "deletePageButton"
                                visible: !notebookController.isJournalPage && !notebookController.currentPagePreview
                                text: qsTr("Delete Page")
                                onClicked: notebookController.deleteCurrentPage()
                            }
                        }

                        Label {
                            id: previewSourcesHeading

                            objectName: "pagePreviewSourcesHeading"
                            anchors.top: destinationActions.bottom
                            anchors.topMargin: window.uiSpacing * 2
                            anchors.left: parent.left
                            anchors.right: parent.right
                            visible: notebookController.currentPagePreview
                            text: pagePreviewSourceList.count === 0 ? qsTr("No unresolved Page Links") : qsTr("Linked references")
                            font.weight: Font.DemiBold
                            Accessible.role: Accessible.Heading
                        }

                        ListView {
                            id: pagePreviewSourceList

                            function sourceItemAt(row) {
                                return itemAtIndex(row);
                            }

                            objectName: "pagePreviewSourceList"
                            anchors.top: previewSourcesHeading.bottom
                            anchors.topMargin: window.uiSpacing
                            anchors.left: parent.left
                            anchors.right: parent.right
                            height: visible ? contentHeight : 0
                            visible: notebookController.currentPagePreview
                            interactive: false
                            spacing: window.uiSpacing
                            model: notebookController.pagePreviewSources
                            Accessible.role: Accessible.List
                            Accessible.name: qsTr("Unresolved Page Link sources")

                            delegate: Label {
                                required property string entryId
                                required property string authoredText

                                width: pagePreviewSourceList.width
                                padding: window.uiSpacing
                                text: notebookController.committedEntryPresentation(entryId, authoredText)
                                textFormat: Text.StyledText
                                wrapMode: Text.Wrap
                                background: Rectangle {
                                    color: palette.alternateBase
                                }
                                Accessible.role: Accessible.ListItem
                                Accessible.name: text
                            }
                        }

                    }

                    delegate: Item {
                        id: entryRoot

                        required property string entryId
                        required property string authoredText
                        required property int depth
                        required property bool hasChildren
                        required property bool canIndent
                        required property bool canOutdent
                        required property bool canMoveUp
                        required property bool canMoveDown
                        required property bool canDelete
                        required property int index
                        readonly property bool outlineSelected: window.isOutlineSelected(entryId)
                        readonly property real outlineIndent: depth * window.uiSpacing * 3
                        readonly property var contextMenu: entryMenu
                        readonly property real editorHeight: Math.max(entryEditor.contentHeight, committedPresentation.contentHeight, entryEditor.font.pixelSize * 1.45) + (window.uiSpacing * 0.7)

                        function focusEditor(cursorPosition) {
                            window.clearOutlineSelection();
                            entryEditor.forceActiveFocus();
                            entryEditor.cursorPosition = Math.min(cursorPosition === undefined ? entryEditor.length : cursorPosition, entryEditor.length);
                        }

                        function focusEditorAtBoundary(firstLine, horizontalPosition) {
                            window.clearOutlineSelection();
                            entryEditor.forceActiveFocus();
                            const verticalPosition = firstLine ? entryEditor.topPadding : Math.max(entryEditor.topPadding, entryEditor.contentHeight - entryEditor.bottomPadding);
                            entryEditor.cursorPosition = entryEditor.positionAt(horizontalPosition, verticalPosition);
                        }

                        function focusBullet() {
                            entryBullet.forceActiveFocus();
                        }

                        function openContextMenu() {
                            window.deferJournalFocusCommit = true;
                            entryMenu.popup();
                        }

                        function beginMenuStructureEdit() {
                            window.deferJournalFocusCommit = false;
                            if (entryEditor.inputMethodComposing) {
                                entryEditor.forceActiveFocus();
                                return false;
                            }
                            window.activeOutlineEditor = null;
                            return true;
                        }

                        function commit() {
                            if (entryEditor.text === entryRoot.authoredText)
                                return true;

                            if (notebookController.updateOutlineEntry(entryRoot.entryId, entryEditor.text))
                                return true;

                            entryEditor.text = entryRoot.authoredText;
                            return false;
                        }

                        Timer {
                            id: typingCommitTimer

                            interval: 1000
                            repeat: false
                            onTriggered: {
                                if (entryEditor.activeFocus && entryEditor.hasPendingEdit && !entryEditor.inputMethodComposing)
                                    entryRoot.commit();
                            }
                        }

                        width: outlineList.width
                        height: editorHeight + inlineDraft.height

                        Rectangle {
                            x: entryRoot.outlineIndent
                            width: parent.width - x
                            height: entryRoot.editorHeight
                            color: entryRoot.outlineSelected ? palette.highlight : palette.alternateBase
                            opacity: entryRoot.outlineSelected ? 0.45 : (entryHover.hovered || entryEditor.activeFocus ? 0.35 : 0)
                        }

                        HoverHandler {
                            id: entryHover
                        }

                        RowLayout {
                            x: entryRoot.outlineIndent
                            width: parent.width - x
                            height: entryRoot.editorHeight
                            spacing: window.uiSpacing

                            FocusScope {
                                id: entryBullet

                                objectName: "outlineEntryBullet-" + entryRoot.index
                                Layout.alignment: Qt.AlignTop
                                Layout.preferredWidth: Math.round(bulletLabel.font.pixelSize * 1.25)
                                Layout.preferredHeight: bulletLabel.implicitHeight
                                activeFocusOnTab: true
                                focus: entryRoot.outlineSelected && entryRoot.index === window.outlineSelectionExtentRow
                                Accessible.role: Accessible.ListItem
                                Accessible.name: notebookController.isJournalPage ? qsTr("Select Journal Entry %1").arg(entryRoot.index + 1) : qsTr("Select Page Entry %1").arg(entryRoot.index + 1)
                                Accessible.description: qsTr("Outline level %1%2").arg(entryRoot.depth + 1).arg(entryRoot.hasChildren ? qsTr(", contains child entries") : "")
                                Accessible.selectable: true
                                Accessible.selected: entryRoot.outlineSelected
                                Accessible.focusable: true
                                Accessible.focused: activeFocus
                                Accessible.onPressAction: window.selectOutline(entryRoot.index, false)

                                Label {
                                    id: bulletLabel

                                    anchors.fill: parent
                                    horizontalAlignment: Text.AlignHCenter
                                    text: "\u2022"
                                    color: palette.text
                                    Accessible.ignored: true
                                }

                                Keys.onPressed: function(event) {
                                    if ((event.modifiers & Qt.ShiftModifier) && event.key === Qt.Key_Up) {
                                        event.accepted = true;
                                        window.selectOutline(Math.max(0, entryRoot.index - 1), true);
                                    } else if ((event.modifiers & Qt.ShiftModifier) && event.key === Qt.Key_Down) {
                                        event.accepted = true;
                                        window.selectOutline(Math.min(outlineList.count - 1, entryRoot.index + 1), true);
                                    } else if (event.key === Qt.Key_Escape) {
                                        event.accepted = true;
                                        window.clearOutlineSelection();
                                        outlineList.forceActiveFocus();
                                    }
                                }

                                MouseArea {
                                    anchors.fill: parent
                                    acceptedButtons: Qt.LeftButton | Qt.RightButton
                                    onClicked: function(mouse) {
                                        if (mouse.button === Qt.RightButton) {
                                            if (!entryRoot.outlineSelected)
                                                window.selectOutline(entryRoot.index, false);
                                            entryRoot.openContextMenu();
                                        } else {
                                            window.selectOutline(entryRoot.index, (mouse.modifiers & Qt.ShiftModifier) !== 0);
                                        }
                                    }
                                }

                            }


                            Menu {
                                id: entryMenu

                                objectName: "outlineEntryMenu-" + entryRoot.index
                                onClosed: {
                                    if (window.deferJournalFocusCommit) {
                                        window.deferJournalFocusCommit = false;
                                        if (window.activeOutlineEditor === entryEditor) {
                                            if (entryEditor.commit(false)) {
                                                window.outlineEditorFinished(entryEditor);
                                            } else {
                                                window.registerOutlineEditor(entryEditor);
                                                entryEditor.forceActiveFocus();
                                            }
                                        }
                                    }
                                }

                                MenuItem {
                                    action: cutAction
                                }

                                MenuItem {
                                    action: copyAction
                                }

                                MenuItem {
                                    objectName: "followPageLinkMenuItem-" + entryRoot.index
                                    text: qsTr("Follow Page Link")
                                    enabled: !entryEditor.hasPendingEdit
                                    onTriggered: notebookController.followPageLink(
                                        entryRoot.entryId, entryEditor.cursorPosition,
                                        entryEditor.text)
                                }

                                MenuSeparator {
                                }

                                MenuItem {
                                    objectName: "journalIndentMenuItem-" + entryRoot.index
                                    text: qsTr("Indent")
                                    enabled: !notebookController.currentPagePreview && entryRoot.canIndent && window.outlineSelectionRoots.length <= 1
                                    onTriggered: {
                                        if (!entryRoot.beginMenuStructureEdit())
                                            return ;

                                        if (!window.applyOutlineOutcome(notebookController.indentOutlineEntry(entryRoot.entryId, entryEditor.text, entryEditor.cursorPosition)))
                                            window.registerOutlineEditor(entryEditor);
                                    }
                                }

                                MenuItem {
                                    text: qsTr("Outdent")
                                    enabled: !notebookController.currentPagePreview && entryRoot.canOutdent && window.outlineSelectionRoots.length <= 1
                                    onTriggered: {
                                        if (!entryRoot.beginMenuStructureEdit())
                                            return ;

                                        if (!window.applyOutlineOutcome(notebookController.outdentOutlineEntry(entryRoot.entryId, entryEditor.text, entryEditor.cursorPosition)))
                                            window.registerOutlineEditor(entryEditor);
                                    }
                                }

                                MenuItem {
                                    text: qsTr("Move Up")
                                    enabled: !notebookController.currentPagePreview && entryRoot.canMoveUp && window.outlineSelectionRoots.length <= 1
                                    onTriggered: {
                                        if (!entryRoot.beginMenuStructureEdit())
                                            return ;

                                        if (!window.applyOutlineOutcome(notebookController.moveOutlineEntryUp(entryRoot.entryId, entryEditor.text, entryEditor.cursorPosition)))
                                            window.registerOutlineEditor(entryEditor);
                                    }
                                }

                                MenuItem {
                                    text: qsTr("Move Down")
                                    enabled: !notebookController.currentPagePreview && entryRoot.canMoveDown && window.outlineSelectionRoots.length <= 1
                                    onTriggered: {
                                        if (!entryRoot.beginMenuStructureEdit())
                                            return ;

                                        if (!window.applyOutlineOutcome(notebookController.moveOutlineEntryDown(entryRoot.entryId, entryEditor.text, entryEditor.cursorPosition)))
                                            window.registerOutlineEditor(entryEditor);
                                    }
                                }

                                MenuSeparator {
                                }

                                MenuItem {
                                    text: qsTr("Delete Entry")
                                    enabled: !notebookController.currentPagePreview && entryRoot.canDelete
                                    onTriggered: {
                                        window.deferJournalFocusCommit = false;
                                        if (!entryEditor.commit(false)) {
                                            window.registerOutlineEditor(entryEditor);
                                            entryEditor.forceActiveFocus();
                                            return ;
                                        }

                                        window.activeOutlineEditor = null;
                                        window.applyOutlineOutcome(notebookController.deleteOutlineEntry(entryRoot.entryId));
                                    }
                                }
                            }

                            TextEdit {
                                id: entryEditor

                                readonly property bool isDraftEditor: false
                                readonly property int entryRow: entryRoot.index
                                readonly property bool hasPendingEdit: text !== entryRoot.authoredText
                                readonly property string outlineEntryId: entryRoot.entryId

                                function commit(openNext) {
                                    if (inputMethodComposing)
                                        return false;

                                    typingCommitTimer.stop();
                                    return entryRoot.commit();
                                }

                                objectName: "outlineEntryEditor-" + entryRoot.index
                                Layout.fillWidth: true
                                Layout.alignment: Qt.AlignTop
                                text: entryRoot.authoredText
                                readOnly: notebookController.currentPagePreview
                                color: committedPresentation.visible ? "transparent" : palette.text
                                wrapMode: TextEdit.Wrap
                                selectByMouse: true
                                padding: 0
                                topPadding: 0
                                bottomPadding: 0
                                Accessible.name: notebookController.isJournalPage ? qsTr("Journal Entry %1").arg(entryRoot.index + 1) : qsTr("Page Entry %1").arg(entryRoot.index + 1)
                                Accessible.description: qsTr("Outline level %1").arg(entryRoot.depth + 1)
                                Accessible.multiLine: true
                                onTextChanged: {
                                    if (activeFocus && text !== entryRoot.authoredText && !inputMethodComposing)
                                        typingCommitTimer.restart();
                                }
                                onInputMethodComposingChanged: {
                                    if (!inputMethodComposing && activeFocus && hasPendingEdit)
                                        typingCommitTimer.restart();
                                }
                                onActiveFocusChanged: {
                                    if (activeFocus) {
                                        window.clearOutlineSelection();
                                        window.registerOutlineEditor(entryEditor);
                                    } else if (window.activeOutlineEditor === entryEditor) {
                                        if (window.deferJournalFocusCommit)
                                            return ;

                                        if (entryEditor.inputMethodComposing) {
                                            Qt.callLater(function() {
                                                entryEditor.forceActiveFocus();
                                            });
                                        } else {
                                            entryEditor.commit(false);
                                            window.outlineEditorFinished(entryEditor);
                                        }
                                    }
                                }
                                Keys.onPressed: function(event) {
                                    if (window.handleHistoryKey(event)) {
                                        event.accepted = true;
                                    } else if (inputMethodComposing) {
                                        if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter)
                                            event.accepted = true;

                                        return ;
                                    } else if ((event.key === Qt.Key_Return || event.key === Qt.Key_Enter) && (event.modifiers & window.structureModifier)) {
                                        event.accepted = true;
                                        if (!hasPendingEdit)
                                            notebookController.followPageLink(entryRoot.entryId, cursorPosition, text);
                                    } else if (notebookController.currentPagePreview &&
                                               (event.key === Qt.Key_Return || event.key === Qt.Key_Enter ||
                                                event.key === Qt.Key_Backspace || event.key === Qt.Key_Delete ||
                                                event.key === Qt.Key_Tab || event.key === Qt.Key_Backtab)) {
                                        event.accepted = true;
                                    } else if ((event.key === Qt.Key_Return || event.key === Qt.Key_Enter) && window.textModifiers(event.modifiers) === Qt.NoModifier) {
                                        event.accepted = true;
                                        window.activeOutlineEditor = null;
                                        if (!window.applyOutlineOutcome(notebookController.splitOutlineEntry(entryRoot.entryId, text, cursorPosition)))
                                            window.registerOutlineEditor(entryEditor);
                                    } else if ((event.key === Qt.Key_Return || event.key === Qt.Key_Enter) && window.textModifiers(event.modifiers) !== Qt.ShiftModifier) {
                                        event.accepted = true;
                                    } else if (event.key === Qt.Key_Backspace && cursorPosition === 0 && selectionStart === selectionEnd) {
                                        event.accepted = true;
                                        window.activeOutlineEditor = null;
                                        if (!window.applyOutlineOutcome(notebookController.joinOutlineEntry(entryRoot.entryId, text)))
                                            window.registerOutlineEditor(entryEditor);
                                    } else if (event.key === Qt.Key_Tab && !(event.modifiers & Qt.ShiftModifier)) {
                                        event.accepted = true;
                                        window.activeOutlineEditor = null;
                                        if (!window.applyOutlineOutcome(notebookController.indentOutlineEntry(entryRoot.entryId, text, cursorPosition)))
                                            window.registerOutlineEditor(entryEditor);
                                    } else if (event.key === Qt.Key_Backtab || (event.key === Qt.Key_Tab && (event.modifiers & Qt.ShiftModifier))) {
                                        event.accepted = true;
                                        window.activeOutlineEditor = null;
                                        if (!window.applyOutlineOutcome(notebookController.outdentOutlineEntry(entryRoot.entryId, text, cursorPosition)))
                                            window.registerOutlineEditor(entryEditor);
                                    } else if ((event.modifiers & window.structureModifier) && (event.modifiers & Qt.ShiftModifier) && event.key === Qt.Key_Up) {
                                        event.accepted = true;
                                        window.activeOutlineEditor = null;
                                        if (!window.applyOutlineOutcome(notebookController.moveOutlineEntryUp(entryRoot.entryId, text, cursorPosition)))
                                            window.registerOutlineEditor(entryEditor);
                                    } else if ((event.modifiers & window.structureModifier) && (event.modifiers & Qt.ShiftModifier) && event.key === Qt.Key_Down) {
                                        event.accepted = true;
                                        window.activeOutlineEditor = null;
                                        if (!window.applyOutlineOutcome(notebookController.moveOutlineEntryDown(entryRoot.entryId, text, cursorPosition)))
                                            window.registerOutlineEditor(entryEditor);
                                    } else if (event.key === Qt.Key_Escape) {
                                        event.accepted = true;
                                        text = entryRoot.authoredText;
                                        outlineList.forceActiveFocus();
                                    } else if (event.key === Qt.Key_Up && event.modifiers === Qt.NoModifier && window.cursorOnFirstVisualLine(entryEditor)) {
                                        event.accepted = true;
                                        if (entryRoot.index > 0)
                                            window.focusEntryBoundary(entryRoot.index - 1, false, entryEditor.cursorRectangle.x);

                                    } else if (event.key === Qt.Key_Down && event.modifiers === Qt.NoModifier && window.cursorOnLastVisualLine(entryEditor)) {
                                        event.accepted = true;
                                        if (entryRoot.index + 1 < outlineList.count)
                                            window.focusEntryBoundary(entryRoot.index + 1, true, entryEditor.cursorRectangle.x);
                                        else
                                            window.beginDraft(entryRoot.entryId);
                                    }
                                }

                                Label {
                                    id: committedPresentation

                                    objectName: "outlineEntryPresentation-" + entryRoot.index
                                    anchors.fill: parent
                                    visible: !entryEditor.activeFocus || notebookController.currentPagePreview
                                    z: 1
                                    text: notebookController.committedEntryPresentation(
                                        entryRoot.entryId, entryRoot.authoredText)
                                    textFormat: Text.StyledText
                                    wrapMode: Text.Wrap
                                    color: palette.text
                                    Accessible.name: text
                                    onLinkActivated: function(link) {
                                        notebookController.followPageLink(
                                            entryRoot.entryId, Number(link),
                                            entryRoot.authoredText);
                                    }

                                    TapHandler {
                                        onTapped: function(eventPoint) {
                                            if (notebookController.currentPagePreview ||
                                                committedPresentation.linkAt(
                                                    eventPoint.position.x,
                                                    eventPoint.position.y).length > 0)
                                                return ;

                                            entryRoot.focusEditor(entryEditor.positionAt(
                                                eventPoint.position.x,
                                                eventPoint.position.y));
                                        }
                                    }
                                }

                            }

                        }

                        JournalDraft {
                            id: inlineDraft

                            anchors.top: parent.top
                            anchors.topMargin: entryRoot.editorHeight
                            x: entryRoot.outlineIndent
                            width: parent.width - x
                            insertionAfterId: entryRoot.entryId
                            anchorRow: entryRoot.index
                            activeDraft: !notebookController.currentPagePreview && entryRoot.index + 1 < outlineList.count && window.draftAfterId === entryRoot.entryId
                        }

                    }

                    footer: Item {
                        readonly property string lastEntryId: outlineList.count > 0 ? notebookController.outlineEntryId(outlineList.count - 1) : ""
                        readonly property bool hasTrailingDraft: !notebookController.currentPagePreview && (window.draftAfterId === "" || window.draftAfterId === lastEntryId)

                        width: outlineList.width
                        height: Math.max(trailingDraft.implicitHeight + (window.uiSpacing * 10), outlineList.height * 0.45)

                        JournalDraft {
                            id: trailingDraft

                            width: parent.width
                            insertionAfterId: parent.lastEntryId === window.draftAfterId ? parent.lastEntryId : ""
                            anchorRow: outlineList.count - 1
                            activeDraft: parent.hasTrailingDraft
                        }

                        Item {
                            anchors.top: trailingDraft.bottom
                            anchors.left: parent.left
                            anchors.right: parent.right
                            anchors.bottom: parent.bottom

                            TapHandler {
                                onTapped: window.beginTrailingDraft()
                            }

                        }

                        Item {
                            width: parent.width
                            height: trailingDraft.activeDraft ? 0 : trailingDraft.editorHeight

                            RowLayout {
                                anchors.fill: parent
                                spacing: window.uiSpacing

                                Label {
                                    Layout.alignment: Qt.AlignTop
                                    Layout.preferredWidth: Math.round(font.pixelSize * 1.25)
                                    horizontalAlignment: Text.AlignHCenter
                                    text: "\u2022"
                                    color: palette.placeholderText
                                }

                                Item {
                                    Layout.fillWidth: true
                                }

                            }

                            TapHandler {
                                onTapped: window.beginTrailingDraft()
                            }

                        }

                    }

                }

            }

        }

    }

    component JournalDraft: Item {
        id: draftRoot

        required property string insertionAfterId
        required property int anchorRow
        required property bool activeDraft
        readonly property real editorHeight: Math.max(draftEditor.contentHeight, draftEditor.font.pixelSize * 1.45) + (window.uiSpacing * 0.7)

        function focusEditor(cursorPosition) {
            if (!activeDraft)
                return ;

            if (draftEditor.text !== window.draftText)
                draftEditor.text = window.draftText;

            draftEditor.forceActiveFocus();
            draftEditor.cursorPosition = Math.min(cursorPosition === undefined ? draftEditor.length : cursorPosition, draftEditor.length);
        }

        function commit(openNext) {
            if (!activeDraft)
                return true;

            window.draftText = draftEditor.text;
            if (draftEditor.text.length === 0) {
                if (!openNext)
                    window.draftAfterId = "";

                return true;
            }
            const insertedRow = notebookController.insertOutlineEntry(draftEditor.text, insertionAfterId);
            if (insertedRow < 0) {
                Qt.callLater(function() {
                    draftRoot.focusEditor(draftEditor.cursorPosition);
                });
                return false;
            }
            const insertedId = notebookController.outlineEntryId(insertedRow);
            window.draftText = "";
            window.draftAfterId = openNext ? insertedId : "";
            if (openNext) {
                outlineList.positionViewAtIndex(insertedRow, ListView.Contain);
                Qt.callLater(function() {
                    window.draftFocusRequested(insertedId);
                });
            }
            return true;
        }

        visible: activeDraft
        implicitHeight: activeDraft ? editorHeight : 0
        height: implicitHeight
        onActiveDraftChanged: {
            if (activeDraft && draftEditor.text !== window.draftText)
                draftEditor.text = window.draftText;

        }
        Component.onCompleted: {
            if (activeDraft)
                draftEditor.text = window.draftText;

        }

        Connections {
            function onDraftFocusRequested(afterId) {
                if (draftRoot.activeDraft && draftRoot.insertionAfterId === afterId) {
                    if (draftEditor.text !== window.draftText)
                        draftEditor.text = window.draftText;

                    draftRoot.focusEditor(window.draftText.length);
                }
            }

            target: window
        }

        Rectangle {
            anchors.fill: parent
            color: palette.alternateBase
            opacity: draftHover.hovered || draftEditor.activeFocus ? 0.35 : 0
        }

        HoverHandler {
            id: draftHover
        }

        RowLayout {
            anchors.fill: parent
            spacing: window.uiSpacing

            Label {
                objectName: draftRoot.activeDraft ? "journalDraftBullet" : ""
                Layout.alignment: Qt.AlignTop
                Layout.preferredWidth: Math.round(font.pixelSize * 1.25)
                horizontalAlignment: Text.AlignHCenter
                text: "\u2022"
                color: palette.text

                TapHandler {
                    onTapped: draftRoot.focusEditor(draftEditor.cursorPosition)
                }

            }

            TextEdit {
                id: draftEditor

                readonly property bool isDraftEditor: true
                readonly property int entryRow: draftRoot.anchorRow + 1
                readonly property bool hasPendingEdit: text.length > 0
                readonly property string outlineEntryId: ""

                function commit(openNext) {
                    if (inputMethodComposing)
                        return false;

                    return draftRoot.commit(openNext);
                }

                objectName: draftRoot.activeDraft ? "journalDraftEditor" : ""
                Layout.fillWidth: true
                Layout.alignment: Qt.AlignTop
                wrapMode: TextEdit.Wrap
                selectByMouse: true
                padding: 0
                topPadding: 0
                bottomPadding: 0
                Accessible.name: notebookController.isJournalPage ? qsTr("New Journal Entry") : qsTr("New Page Entry")
                Accessible.multiLine: true
                onTextChanged: {
                    if (draftRoot.activeDraft)
                        window.draftText = text;

                }
                onActiveFocusChanged: {
                    if (activeFocus) {
                        window.clearOutlineSelection();
                        window.registerOutlineEditor(draftEditor);
                    } else if (window.activeOutlineEditor === draftEditor) {
                        if (draftEditor.inputMethodComposing) {
                            Qt.callLater(function() {
                                draftEditor.forceActiveFocus();
                            });
                        } else {
                            draftEditor.commit(false);
                            window.outlineEditorFinished(draftEditor);
                        }
                    }
                }
                Keys.onPressed: function(event) {
                    if (window.handleHistoryKey(event)) {
                        event.accepted = true;
                    } else if (inputMethodComposing) {
                        if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter)
                            event.accepted = true;

                        return ;
                    } else if ((event.key === Qt.Key_Return || event.key === Qt.Key_Enter) && window.textModifiers(event.modifiers) === Qt.NoModifier) {
                        event.accepted = true;
                        if (window.rolloverPending) {
                            if (draftRoot.commit(false)) {
                                outlineList.forceActiveFocus();
                                window.outlineEditorFinished(draftEditor);
                            }
                        } else {
                            draftRoot.commit(true);
                        }
                    } else if ((event.key === Qt.Key_Return || event.key === Qt.Key_Enter) && window.textModifiers(event.modifiers) !== Qt.ShiftModifier) {
                        event.accepted = true;
                    } else if (event.key === Qt.Key_Escape) {
                        event.accepted = true;
                        window.draftText = "";
                        window.draftAfterId = "";
                        outlineList.forceActiveFocus();
                    } else if (event.key === Qt.Key_Up && event.modifiers === Qt.NoModifier && window.cursorOnFirstVisualLine(draftEditor)) {
                        event.accepted = true;
                        const cursor = cursorPosition;
                        if (draftRoot.commit(false))
                            window.focusEntry(draftRoot.anchorRow, cursor);

                    } else if (event.key === Qt.Key_Down && event.modifiers === Qt.NoModifier && window.cursorOnLastVisualLine(draftEditor)) {
                        event.accepted = true;
                        const cursor = cursorPosition;
                        const hadText = text.length > 0;
                        if (draftRoot.commit(false)) {
                            const targetRow = draftRoot.anchorRow + (hadText ? 2 : 1);
                            if (targetRow < outlineList.count)
                                window.focusEntry(targetRow, cursor);

                        }
                    }
                }

            }

        }

    }

    menuBar: MenuBar {
        Menu {
            title: qsTr("&File")

            MenuItem {
                action: createAction
            }

            MenuItem {
                action: openAction
            }

            MenuItem {
                action: closeAction
            }

            MenuSeparator {
            }

            MenuItem {
                action: quitAction
            }

        }

        Menu {
            title: qsTr("&Edit")

            MenuItem {
                action: cutAction
            }

            MenuItem {
                action: copyAction
            }

            MenuItem {
                action: pasteAction
            }

            MenuItem {
                action: selectAllAction
            }

            MenuSeparator {
            }

            MenuItem {
                action: undoAction
            }

            MenuItem {
                action: redoAction
            }

        }

        Menu {
            title: qsTr("&Page")

            MenuItem { action: newPageAction }
            MenuItem { action: goToPageAction }
            MenuItem { action: renamePageAction }
            MenuSeparator { }
            MenuItem {
                text: qsTr("Today")
                onTriggered: {
                    if (window.commitActiveOutlineEditor())
                        notebookController.navigateToToday();
                }
            }
        }

    }

}
