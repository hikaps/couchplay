// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2024 hikaps

import QtQuick
import QtQuick.Layouts
import QtQuick.Controls as Controls
import QtQuick.Dialogs
import org.kde.kirigami as Kirigami

Kirigami.Dialog {
    id: root
    objectName: "dialogEditPreset"
    Accessible.role: Accessible.Dialog
    Accessible.name: title
    title: i18nc("@title:dialog", "Edit Preset: %1", presetName)

    required property var presetManager
    property var steamConfigManager: null

    property string presetId: ""
    property string presetName: ""

    footerLeadingComponent: Controls.Button {
        objectName: "btnAddDirectory"
        text: i18nc("@action:button", "Add Directory...")
        icon.name: "folder-add"
        Accessible.role: Accessible.Button
        Accessible.name: text
        Accessible.onPressAction: clicked()
        onClicked: folderDialog.open()
    }

    ListModel {
        id: directoriesModel
    }

    function setDirectoriesFromBackend(dirs) {
        directoriesModel.clear()
        for (let i = 0; i < dirs.length; i++) {
            directoriesModel.append({ path: dirs[i] })
        }
    }

    function getDirectoriesArray() {
        let arr = []
        for (let i = 0; i < directoriesModel.count; i++) {
            arr.push(directoriesModel.get(i).path)
        }
        return arr
    }

    ColumnLayout {
        spacing: Kirigami.Units.largeSpacing
        
        Kirigami.Heading {
            level: 3
            text: i18nc("@title", "Shared Directories")
            Layout.fillWidth: true
        }
        
        Kirigami.InlineMessage {
            Layout.fillWidth: true
            text: i18nc("@info", "These directories will be accessible to this application when running.")
            type: Kirigami.MessageType.Information
            visible: true
        }

        Controls.ScrollView {
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.preferredHeight: Kirigami.Units.gridUnit * 12

            ListView {
                id: sharedDirsList
                clip: true
                model: directoriesModel

                delegate: RowLayout {
                    width: ListView.view.width
                    spacing: Kirigami.Units.smallSpacing

                    Kirigami.Icon {
                        source: "folder"
                        Layout.preferredWidth: Kirigami.Units.iconSizes.small
                        Layout.preferredHeight: Kirigami.Units.iconSizes.small
                    }

                    Controls.Label {
                        text: model.path
                        Layout.fillWidth: true
                        elide: Text.ElideMiddle
                    }

                    Controls.Button {
                        objectName: "btnRemoveDirectory"
                        icon.name: "edit-delete"
                        display: Controls.AbstractButton.IconOnly
                        Accessible.role: Accessible.Button
                        Accessible.name: i18nc("@action:button", "Remove directory")
                        Accessible.onPressAction: clicked()
                        onClicked: {
                            directoriesModel.remove(index)
                            root.presetManager.setSharedDirectories(root.presetId, root.getDirectoriesArray())
                        }
                    }
                }

                Kirigami.PlaceholderMessage {
                    anchors.centerIn: parent
                    visible: sharedDirsList.count === 0
                    text: i18nc("@info", "No shared directories configured")
                    icon.name: "folder-open"
                }
            }
        }
    }

    FolderDialog {
        id: folderDialog
        title: i18nc("@title:dialog", "Select Directory to Share")
        onAccepted: {
            let path = selectedFolder.toString()
            if (path.startsWith("file://")) path = path.substring(7)
            path = decodeURIComponent(path)
            
            let exists = false
            for (let i = 0; i < directoriesModel.count; i++) {
                if (directoriesModel.get(i).path === path) {
                    exists = true
                    break
                }
            }
            
            if (!exists) {
                directoriesModel.append({ path: path })
                root.presetManager.setSharedDirectories(root.presetId, root.getDirectoriesArray())
            }
        }
    }
}
