// SPDX-License-Identifier: MPL-2.0
import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs

ApplicationWindow {
    id: window
    width: 760
    height: 520
    minimumWidth: 520
    minimumHeight: 360
    visible: true
    title: notebookController.hasOpenNotebook
        ? qsTr("%1 — Hieda").arg(notebookController.notebookName)
        : qsTr("Hieda")

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

    Column {
        anchors.fill: parent
        spacing: 0

        Rectangle {
            width: parent.width
            height: errorRow.implicitHeight + 24
            color: "#6f1d1b"
            visible: notebookController.errorMessage.length > 0

            Row {
                id: errorRow
                anchors.fill: parent
                anchors.margins: 12
                spacing: 12

                Label {
                    width: parent.width - dismissButton.width - parent.spacing
                    text: notebookController.errorMessage
                    color: "white"
                    wrapMode: Text.Wrap
                    anchors.verticalCenter: parent.verticalCenter
                }
                Button {
                    id: dismissButton
                    text: qsTr("Dismiss")
                    onClicked: notebookController.clearError()
                }
            }
        }

        Item {
            width: parent.width
            height: parent.height - (notebookController.errorMessage.length > 0
                ? errorRow.implicitHeight + 24 : 0)

            Column {
                anchors.centerIn: parent
                width: Math.min(parent.width - 64, 560)
                spacing: 20

                Label {
                    width: parent.width
                    horizontalAlignment: Text.AlignHCenter
                    text: notebookController.hasOpenNotebook
                        ? notebookController.notebookName : qsTr("Welcome to Hieda")
                    font.pixelSize: 30
                    font.bold: true
                }

                Label {
                    width: parent.width
                    horizontalAlignment: Text.AlignHCenter
                    wrapMode: Text.Wrap
                    text: notebookController.hasOpenNotebook
                        ? qsTr("Your Notebook is ready. Journal editing arrives in the next implementation slice.\n%1")
                            .arg(notebookController.notebookPath)
                        : qsTr("Create a portable Notebook or open one you already have.")
                    opacity: 0.75
                }

                Row {
                    anchors.horizontalCenter: parent.horizontalCenter
                    spacing: 12
                    visible: !notebookController.hasOpenNotebook

                    Button {
                        text: qsTr("Create Notebook")
                        onClicked: createDialog.open()
                    }
                    Button {
                        text: qsTr("Open Notebook")
                        onClicked: openDialog.open()
                    }
                }

                Button {
                    anchors.horizontalCenter: parent.horizontalCenter
                    visible: notebookController.hasOpenNotebook
                    text: qsTr("Close Notebook")
                    onClicked: notebookController.closeNotebook()
                }
            }
        }
    }
}
