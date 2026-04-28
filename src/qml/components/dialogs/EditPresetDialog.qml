// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2024 hikaps

import QtQuick
import QtQuick.Layouts
import QtQuick.Controls as Controls
import QtQuick.Dialogs
import org.kde.kirigami as Kirigami

Kirigami.Dialog {
    id: root
    title: i18nc("@title:dialog", "Edit Preset: %1", presetName)
    standardButtons: Kirigami.Dialog.Close
    preferredWidth: Kirigami.Units.gridUnit * 30

    required property var presetManager
    property var steamConfigManager: null

    property string presetId: ""
    property string presetName: ""

    footerLeadingComponent: Controls.Button {
        text: i18nc("@action:button", "Add Directory...")
        icon.name: "folder-add"
        onClicked: folderDialog.open()
    }

    ListModel {
        id: directoriesModel
    }

    function setDirectoriesFromBackend(dirs) {
        directoriesModel.clear()
        for (let i = 0; i < dirs.length; i++) {
            directoriesModel.append({ path: dirs[i].path, mode: dirs[i].mode })
        }
    }

    function getDirectoriesArray() {
        let arr = []
        for (let i = 0; i < directoriesModel.count; i++) {
            let item = directoriesModel.get(i)
            arr.push({ path: item.path, mode: item.mode })
        }
        return arr
    }

    ColumnLayout {
        spacing: Kirigami.Units.largeSpacing
        
        Kirigami.Heading {
            level: 3
            text: i18nc("@title", "Data Directories")
            Layout.fillWidth: true
        }
        
        Kirigami.InlineMessage {
            Layout.fillWidth: true
            text: i18nc("@info", "Choose how to share data: 'Shared (ACL)' shares a single folder among all users, 'Copy' duplicates it for each user, and 'Overlay' creates a per-user copy-on-write overlay.")
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

                    Kirigami.ComboBox {
                        model: [
                            { value: "acl", text: i18nc("@item:inlistbox", "Shared (ACL)") },
                            { value: "copy", text: i18nc("@item:inlistbox", "Copy files") },
                            { value: "overlay", text: i18nc("@item:inlistbox", "Per-user overlay") }
                        ]
                        textRole: "text"
                        valueRole: "value"
                        currentIndex: {
                            if (mode === "copy") return 1;
                            if (mode === "overlay") return 2;
                            return 0;
                        }
                        onActivated: {
                            directoriesModel.setProperty(index, "mode", currentValue)
                            root.presetManager.setDataDirectories(root.presetId, root.getDirectoriesArray())
                        }
                    }

                    Controls.Button {
                        icon.name: "edit-delete"
                        display: Controls.AbstractButton.IconOnly
                        onClicked: {
                            directoriesModel.remove(index)
                            root.presetManager.setDataDirectories(root.presetId, root.getDirectoriesArray())
                        }
                    }
                }

                Kirigami.PlaceholderMessage {
                    anchors.centerIn: parent
                    visible: sharedDirsList.count === 0
                    text: i18nc("@info", "No data directories configured")
                    icon.name: "folder-open"
                }
            }
        }
    }

    FolderDialog {
        id: folderDialog
        title: i18nc("@title:dialog", "Select Directory")
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
                directoriesModel.append({ path: path, mode: "acl" })
                root.presetManager.setDataDirectories(root.presetId, root.getDirectoriesArray())
            }
        }
    }
}
