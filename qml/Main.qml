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
    property var activeJournalEditor: null
    property bool rolloverPending: false
    property bool deferJournalFocusCommit: false
    readonly property bool hasPendingJournalEdit: activeJournalEditor && activeJournalEditor.hasPendingEdit

    signal draftFocusRequested(string afterId)

    function beginDraft(afterId) {
        draftText = "";
        draftAfterId = afterId;
        Qt.callLater(function() {
            draftFocusRequested(afterId);
        });
    }

    function beginTrailingDraft() {
        if (activeJournalEditor && activeJournalEditor.isDraftEditor && !activeJournalEditor.commit(false))
            return ;

        beginDraft("");
    }

    function focusEntry(row, cursorPosition) {
        if (row < 0 || row >= journalList.count)
            return ;

        journalList.positionViewAtIndex(row, ListView.Contain);
        Qt.callLater(function() {
            const item = journalList.itemAtIndex(row);
            if (item)
                item.focusEditor(cursorPosition);

        });
    }

    function applyJournalOutcome(outcome) {
        if (!outcome.succeeded)
            return false;

        draftAfterId = "";
        draftText = "";
        if (outcome.row >= 0)
            focusEntry(outcome.row, outcome.cursorPosition);

        return true;
    }

    function registerJournalEditor(editor) {
        activeJournalEditor = editor;
    }

    function commitActiveJournalEditor() {
        const editor = activeJournalEditor;
        if (editor && editor.inputMethodComposing) {
            editor.forceActiveFocus();
            return false;
        }
        activeJournalEditor = null;
        if (editor && editor.hasPendingEdit && !editor.commit(false)) {
            registerJournalEditor(editor);
            return false;
        }
        return true;
    }

    function applyHistory(redo) {
        const editor = activeJournalEditor;
        const preferredEntryId = editor ? editor.journalEntryId : "";
        const cursorPosition = editor ? editor.cursorPosition : 0;
        if (!commitActiveJournalEditor())
            return ;

        if (redo ? !notebookController.canRedo : !notebookController.canUndo)
            return ;

        applyJournalOutcome(redo ? notebookController.redoJournalEdit(preferredEntryId, cursorPosition) : notebookController.undoJournalEdit(preferredEntryId, cursorPosition));
    }

    function handleHistoryKey(event) {
        if (!(event.modifiers & window.structureModifier))
            return false;

        if (event.key === Qt.Key_Z) {
            window.applyHistory((event.modifiers & Qt.ShiftModifier) !== 0);
            return true;
        }
        if (event.key === Qt.Key_Y) {
            window.applyHistory(true);
            return true;
        }
        return false;
    }

    function journalEditorFinished(editor) {
        if (activeJournalEditor === editor)
            activeJournalEditor = null;

        if (rolloverPending)
            Qt.callLater(function() {
            completeDeferredRollover(true);
        });

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
            if (!window.activeJournalEditor)
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
            if (window.commitActiveJournalEditor())
                notebookController.closeNotebook();
        }
    }

    Action {
        id: quitAction

        text: qsTr("&Quit")
        icon.name: "application-exit"
        shortcut: StandardKey.Quit
        onTriggered: {
            if (window.commitActiveJournalEditor())
                Qt.quit();
        }
    }

    Action {
        id: undoAction

        objectName: "undoAction"
        text: qsTr("&Undo")
        icon.name: "edit-undo"
        shortcut: StandardKey.Undo
        enabled: notebookController.hasOpenNotebook && (notebookController.canUndo || window.hasPendingJournalEdit)
        onTriggered: window.applyHistory(false)
    }

    Action {
        id: redoAction

        objectName: "redoAction"
        text: qsTr("&Redo")
        icon.name: "edit-redo"
        shortcut: StandardKey.Redo
        enabled: notebookController.hasOpenNotebook && notebookController.canRedo && !window.hasPendingJournalEdit
        onTriggered: window.applyHistory(true)
    }

    FileDialog {
        id: createDialog

        title: qsTr("Create a Notebook")
        fileMode: FileDialog.SaveFile
        defaultSuffix: "hieda"
        nameFilters: [qsTr("Hieda Notebooks (*.hieda)"), qsTr("All files (*)")]
        onAccepted: notebookController.createNotebook(selectedFile)
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

                ListView {
                    id: journalList

                    function entryItemAt(row) {
                        return itemAtIndex(row);
                    }

                    objectName: "journalList"
                    anchors.top: parent.top
                    anchors.bottom: parent.bottom
                    anchors.horizontalCenter: parent.horizontalCenter
                    width: Math.min(parent.width - (window.uiSpacing * 8), window.maximumDocumentWidth)
                    visible: notebookController.hasOpenNotebook
                    clip: true
                    model: notebookController.journalEntries
                    currentIndex: -1
                    boundsBehavior: Flickable.StopAtBounds
                    spacing: Math.round(window.uiSpacing * 0.35)

                    ScrollBar.vertical: ScrollBar {
                    }

                    header: Item {
                        width: journalList.width
                        height: dateHeading.implicitHeight + (window.uiSpacing * 6)

                        Label {
                            id: dateHeading

                            width: Math.min(parent.width - (window.uiSpacing * 8), window.maximumDocumentWidth)
                            anchors.horizontalCenter: parent.horizontalCenter
                            anchors.top: parent.top
                            anchors.topMargin: window.uiSpacing * 4
                            text: Qt.formatDate(notebookController.journalDate, Locale.LongFormat)
                            font.pixelSize: Math.round(window.font.pixelSize * 1.7)
                            font.weight: Font.DemiBold
                            Accessible.name: qsTr("Journal Page for %1").arg(text)
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
                        readonly property real outlineIndent: depth * window.uiSpacing * 3
                        readonly property var contextMenu: entryMenu
                        readonly property real editorHeight: Math.max(entryEditor.contentHeight, entryEditor.font.pixelSize * 1.45) + (window.uiSpacing * 0.7)

                        function focusEditor(cursorPosition) {
                            entryEditor.forceActiveFocus();
                            entryEditor.cursorPosition = Math.min(cursorPosition === undefined ? entryEditor.length : cursorPosition, entryEditor.length);
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
                            window.activeJournalEditor = null;
                            return true;
                        }

                        function commit() {
                            if (entryEditor.text === entryRoot.authoredText)
                                return true;

                            if (notebookController.updateJournalEntry(entryRoot.entryId, entryEditor.text))
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

                        width: journalList.width
                        height: editorHeight + inlineDraft.height

                        Rectangle {
                            x: entryRoot.outlineIndent
                            width: parent.width - x
                            height: entryRoot.editorHeight
                            color: palette.alternateBase
                            opacity: entryHover.hovered || entryEditor.activeFocus ? 0.35 : 0
                        }

                        HoverHandler {
                            id: entryHover
                        }

                        RowLayout {
                            x: entryRoot.outlineIndent
                            width: parent.width - x
                            height: entryRoot.editorHeight
                            spacing: window.uiSpacing

                            Label {
                                objectName: "journalEntryBullet-" + entryRoot.index
                                Layout.alignment: Qt.AlignTop
                                Layout.preferredWidth: Math.round(font.pixelSize * 1.25)
                                horizontalAlignment: Text.AlignHCenter
                                text: "\u2022"
                                color: palette.text

                                TapHandler {
                                    acceptedButtons: Qt.LeftButton
                                    onTapped: entryRoot.focusEditor(entryEditor.cursorPosition)
                                }

                                TapHandler {
                                    acceptedButtons: Qt.RightButton
                                    onTapped: entryRoot.openContextMenu()
                                }

                            }


                            Menu {
                                id: entryMenu

                                objectName: "journalEntryMenu-" + entryRoot.index
                                onClosed: {
                                    if (window.deferJournalFocusCommit) {
                                        window.deferJournalFocusCommit = false;
                                        if (window.activeJournalEditor === entryEditor) {
                                            if (entryEditor.commit(false)) {
                                                window.journalEditorFinished(entryEditor);
                                            } else {
                                                window.registerJournalEditor(entryEditor);
                                                entryEditor.forceActiveFocus();
                                            }
                                        }
                                    }
                                }

                                MenuItem {
                                    objectName: "journalIndentMenuItem-" + entryRoot.index
                                    text: qsTr("Indent")
                                    enabled: entryRoot.canIndent
                                    onTriggered: {
                                        if (!entryRoot.beginMenuStructureEdit())
                                            return ;

                                        if (!window.applyJournalOutcome(notebookController.indentJournalEntry(entryRoot.entryId, entryEditor.text, entryEditor.cursorPosition)))
                                            window.registerJournalEditor(entryEditor);
                                    }
                                }

                                MenuItem {
                                    text: qsTr("Outdent")
                                    enabled: entryRoot.canOutdent
                                    onTriggered: {
                                        if (!entryRoot.beginMenuStructureEdit())
                                            return ;

                                        if (!window.applyJournalOutcome(notebookController.outdentJournalEntry(entryRoot.entryId, entryEditor.text, entryEditor.cursorPosition)))
                                            window.registerJournalEditor(entryEditor);
                                    }
                                }

                                MenuItem {
                                    text: qsTr("Move Up")
                                    enabled: entryRoot.canMoveUp
                                    onTriggered: {
                                        if (!entryRoot.beginMenuStructureEdit())
                                            return ;

                                        if (!window.applyJournalOutcome(notebookController.moveJournalEntryUp(entryRoot.entryId, entryEditor.text, entryEditor.cursorPosition)))
                                            window.registerJournalEditor(entryEditor);
                                    }
                                }

                                MenuItem {
                                    text: qsTr("Move Down")
                                    enabled: entryRoot.canMoveDown
                                    onTriggered: {
                                        if (!entryRoot.beginMenuStructureEdit())
                                            return ;

                                        if (!window.applyJournalOutcome(notebookController.moveJournalEntryDown(entryRoot.entryId, entryEditor.text, entryEditor.cursorPosition)))
                                            window.registerJournalEditor(entryEditor);
                                    }
                                }

                                MenuSeparator {
                                }

                                MenuItem {
                                    text: qsTr("Delete Entry")
                                    enabled: entryRoot.canDelete
                                    onTriggered: {
                                        window.deferJournalFocusCommit = false;
                                        if (!entryEditor.commit(false)) {
                                            window.registerJournalEditor(entryEditor);
                                            entryEditor.forceActiveFocus();
                                            return ;
                                        }

                                        window.activeJournalEditor = null;
                                        window.applyJournalOutcome(notebookController.deleteJournalEntry(entryRoot.entryId));
                                    }
                                }
                            }

                            TextArea {
                                id: entryEditor

                                readonly property bool isDraftEditor: false
                                readonly property int entryRow: entryRoot.index
                                readonly property bool hasPendingEdit: text !== entryRoot.authoredText
                                readonly property string journalEntryId: entryRoot.entryId

                                function commit(openNext) {
                                    if (inputMethodComposing)
                                        return false;

                                    typingCommitTimer.stop();
                                    return entryRoot.commit();
                                }

                                objectName: "journalEntryEditor-" + entryRoot.index
                                Layout.fillWidth: true
                                Layout.alignment: Qt.AlignTop
                                text: entryRoot.authoredText
                                wrapMode: TextEdit.Wrap
                                selectByMouse: true
                                padding: 0
                                topPadding: 0
                                bottomPadding: 0
                                Accessible.name: qsTr("Journal Entry %1").arg(entryRoot.index + 1)
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
                                        window.registerJournalEditor(entryEditor);
                                    } else if (window.activeJournalEditor === entryEditor) {
                                        if (window.deferJournalFocusCommit)
                                            return ;

                                        if (entryEditor.inputMethodComposing) {
                                            Qt.callLater(function() {
                                                entryEditor.forceActiveFocus();
                                            });
                                        } else {
                                            entryEditor.commit(false);
                                            window.journalEditorFinished(entryEditor);
                                        }
                                    }
                                }
                                Keys.onPressed: function(event) {
                                    if (window.handleHistoryKey(event)) {
                                        event.accepted = true;
                                    } else if (inputMethodComposing) {
                                        return ;
                                    } else if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter) {
                                        event.accepted = true;
                                        window.activeJournalEditor = null;
                                        if (!window.applyJournalOutcome(notebookController.splitJournalEntry(entryRoot.entryId, text, cursorPosition)))
                                            window.registerJournalEditor(entryEditor);
                                    } else if (event.key === Qt.Key_Backspace && cursorPosition === 0 && selectionStart === selectionEnd) {
                                        event.accepted = true;
                                        window.activeJournalEditor = null;
                                        if (!window.applyJournalOutcome(notebookController.joinJournalEntry(entryRoot.entryId, text)))
                                            window.registerJournalEditor(entryEditor);
                                    } else if (event.key === Qt.Key_Tab && !(event.modifiers & Qt.ShiftModifier)) {
                                        event.accepted = true;
                                        window.activeJournalEditor = null;
                                        if (!window.applyJournalOutcome(notebookController.indentJournalEntry(entryRoot.entryId, text, cursorPosition)))
                                            window.registerJournalEditor(entryEditor);
                                    } else if (event.key === Qt.Key_Backtab || (event.key === Qt.Key_Tab && (event.modifiers & Qt.ShiftModifier))) {
                                        event.accepted = true;
                                        window.activeJournalEditor = null;
                                        if (!window.applyJournalOutcome(notebookController.outdentJournalEntry(entryRoot.entryId, text, cursorPosition)))
                                            window.registerJournalEditor(entryEditor);
                                    } else if ((event.modifiers & window.structureModifier) && (event.modifiers & Qt.ShiftModifier) && event.key === Qt.Key_Up) {
                                        event.accepted = true;
                                        window.activeJournalEditor = null;
                                        if (!window.applyJournalOutcome(notebookController.moveJournalEntryUp(entryRoot.entryId, text, cursorPosition)))
                                            window.registerJournalEditor(entryEditor);
                                    } else if ((event.modifiers & window.structureModifier) && (event.modifiers & Qt.ShiftModifier) && event.key === Qt.Key_Down) {
                                        event.accepted = true;
                                        window.activeJournalEditor = null;
                                        if (!window.applyJournalOutcome(notebookController.moveJournalEntryDown(entryRoot.entryId, text, cursorPosition)))
                                            window.registerJournalEditor(entryEditor);
                                    } else if (event.key === Qt.Key_Escape) {
                                        event.accepted = true;
                                        text = entryRoot.authoredText;
                                        journalList.forceActiveFocus();
                                    } else if (event.key === Qt.Key_Up) {
                                        event.accepted = true;
                                        if (entryRoot.index > 0)
                                            window.focusEntry(entryRoot.index - 1, cursorPosition);

                                    } else if (event.key === Qt.Key_Down) {
                                        event.accepted = true;
                                        if (entryRoot.index + 1 < journalList.count)
                                            window.focusEntry(entryRoot.index + 1, cursorPosition);
                                        else
                                            window.beginDraft(entryRoot.entryId);
                                    }
                                }

                                background: Item {
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
                            activeDraft: entryRoot.index + 1 < journalList.count && window.draftAfterId === entryRoot.entryId
                        }

                    }

                    footer: Item {
                        readonly property string lastEntryId: journalList.count > 0 ? notebookController.journalEntryId(journalList.count - 1) : ""
                        readonly property bool hasTrailingDraft: window.draftAfterId === "" || window.draftAfterId === lastEntryId

                        width: journalList.width
                        height: Math.max(trailingDraft.implicitHeight + (window.uiSpacing * 10), journalList.height * 0.45)

                        JournalDraft {
                            id: trailingDraft

                            width: parent.width
                            insertionAfterId: parent.lastEntryId === window.draftAfterId ? parent.lastEntryId : ""
                            anchorRow: journalList.count - 1
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
            const insertedRow = notebookController.insertJournalEntry(draftEditor.text, insertionAfterId);
            if (insertedRow < 0) {
                Qt.callLater(function() {
                    draftRoot.focusEditor(draftEditor.cursorPosition);
                });
                return false;
            }
            const insertedId = notebookController.journalEntryId(insertedRow);
            window.draftText = "";
            window.draftAfterId = openNext ? insertedId : "";
            if (openNext) {
                journalList.positionViewAtIndex(insertedRow, ListView.Contain);
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

            TextArea {
                id: draftEditor

                readonly property bool isDraftEditor: true
                readonly property int entryRow: draftRoot.anchorRow + 1
                readonly property bool hasPendingEdit: text.length > 0
                readonly property string journalEntryId: ""

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
                Accessible.name: qsTr("New Journal Entry")
                onTextChanged: {
                    if (draftRoot.activeDraft)
                        window.draftText = text;

                }
                onActiveFocusChanged: {
                    if (activeFocus) {
                        window.registerJournalEditor(draftEditor);
                    } else if (window.activeJournalEditor === draftEditor) {
                        if (draftEditor.inputMethodComposing) {
                            Qt.callLater(function() {
                                draftEditor.forceActiveFocus();
                            });
                        } else {
                            draftEditor.commit(false);
                            window.journalEditorFinished(draftEditor);
                        }
                    }
                }
                Keys.onPressed: function(event) {
                    if (window.handleHistoryKey(event)) {
                        event.accepted = true;
                    } else if (inputMethodComposing) {
                        return ;
                    } else if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter) {
                        event.accepted = true;
                        if (window.rolloverPending) {
                            if (draftRoot.commit(false)) {
                                journalList.forceActiveFocus();
                                window.journalEditorFinished(draftEditor);
                            }
                        } else {
                            draftRoot.commit(true);
                        }
                    } else if (event.key === Qt.Key_Escape) {
                        event.accepted = true;
                        window.draftText = "";
                        window.draftAfterId = "";
                        journalList.forceActiveFocus();
                    } else if (event.key === Qt.Key_Up) {
                        event.accepted = true;
                        const cursor = cursorPosition;
                        if (draftRoot.commit(false))
                            window.focusEntry(draftRoot.anchorRow, cursor);

                    } else if (event.key === Qt.Key_Down) {
                        event.accepted = true;
                        const cursor = cursorPosition;
                        const hadText = text.length > 0;
                        if (draftRoot.commit(false)) {
                            const targetRow = draftRoot.anchorRow + (hadText ? 2 : 1);
                            if (targetRow < journalList.count)
                                window.focusEntry(targetRow, cursor);

                        }
                    }
                }

                background: Item {
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
                action: undoAction
            }

            MenuItem {
                action: redoAction
            }

        }

    }

}
