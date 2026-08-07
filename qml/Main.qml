// SPDX-License-Identifier: MPL-2.0
import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts

ApplicationWindow {
    id: window

    readonly property real uiSpacing: Math.max(6, Math.round(font.pixelSize * 0.6))

    width: 760
    height: 520
    minimumWidth: 520
    minimumHeight: 360
    visible: true
    title: notebookController.hasOpenNotebook ? qsTr("%1 — Hieda").arg(notebookController.notebookName) : qsTr("Hieda")

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

        text: qsTr("&Close Notebook")
        icon.name: "window-close"
        shortcut: StandardKey.Close
        enabled: notebookController.hasOpenNotebook
        onTriggered: notebookController.closeNotebook()
    }

    Action {
        id: quitAction

        text: qsTr("&Quit")
        icon.name: "application-exit"
        shortcut: StandardKey.Quit
        onTriggered: Qt.quit()
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

                    Label {
                        Layout.fillWidth: true
                        horizontalAlignment: Text.AlignHCenter
                        text: notebookController.hasOpenNotebook ? notebookController.notebookName : qsTr("Welcome to Hieda")
                        font.pixelSize: Math.round(window.font.pixelSize * 1.5)
                        font.weight: Font.DemiBold
                    }

                    Label {
                        Layout.fillWidth: true
                        horizontalAlignment: Text.AlignHCenter
                        wrapMode: Text.Wrap
                        text: notebookController.hasOpenNotebook ? qsTr("Your Notebook is ready. Journal editing arrives in the next implementation slice.") : qsTr("Create a portable Notebook or open one you already have.")
                    }

                    RowLayout {
                        Layout.alignment: Qt.AlignHCenter
                        spacing: window.uiSpacing
                        visible: !notebookController.hasOpenNotebook

                        Button {
                            action: createAction
                            highlighted: true
                        }

                        Button {
                            action: openAction
                        }

                    }

                    RowLayout {
                        Layout.fillWidth: true
                        visible: notebookController.hasOpenNotebook

                        Label {
                            text: qsTr("Location:")
                        }

                        TextField {
                            Layout.fillWidth: true
                            text: notebookController.notebookPath
                            readOnly: true
                            selectByMouse: true
                        }

                    }

                    Button {
                        Layout.alignment: Qt.AlignHCenter
                        visible: notebookController.hasOpenNotebook
                        action: closeAction
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

    }

    header: ToolBar {
        RowLayout {
            anchors.fill: parent
            spacing: window.uiSpacing

            ToolButton {
                action: createAction
                display: AbstractButton.TextBesideIcon
            }

            ToolButton {
                action: openAction
                display: AbstractButton.TextBesideIcon
            }

            Label {
                Layout.fillWidth: true
                horizontalAlignment: Text.AlignRight
                text: notebookController.hasOpenNotebook ? notebookController.notebookName : qsTr("No Notebook open")
                elide: Text.ElideMiddle
            }

            ToolButton {
                action: closeAction
                display: AbstractButton.TextBesideIcon
            }

        }

    }

}
