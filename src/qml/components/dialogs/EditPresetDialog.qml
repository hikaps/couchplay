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
    standardButtons: Kirigami.Dialog.Close
    preferredWidth: Kirigami.Units.gridUnit * 30
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
    function integrationSelected(integrationId) {
        if (!root.presetManager) return false
        return root.presetManager.getRequiredIntegrations(root.presetId).indexOf(integrationId) >= 0
    }

    function setIntegration(integrationId, enabled) {
        if (!root.presetManager) return
        let integrations = root.presetManager.getRequiredIntegrations(root.presetId)
        let position = integrations.indexOf(integrationId)
        if (enabled && position < 0) {
            integrations.push(integrationId)
            root.presetManager.setRequiredIntegrations(root.presetId, integrations)
            let defaults = root.presetManager.getDataDirectories(integrationId) || []
            for (let i = 0; i < defaults.length; ++i) {
                let exists = false
                for (let j = 0; j < directoriesModel.count; ++j) {
                    let current = directoriesModel.get(j)
                    if (current.path === defaults[i].path && current.mode === defaults[i].mode) {
                        exists = true
                        break
                    }
                }
                if (!exists) directoriesModel.append({ path: defaults[i].path, mode: defaults[i].mode })
            }
            root.presetManager.setDataDirectories(root.presetId, root.getDirectoriesArray())
        } else if (!enabled && position >= 0) {
            integrations.splice(position, 1)
            root.presetManager.setRequiredIntegrations(root.presetId, integrations)
        }
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
            text: i18nc("@info", "Choose how to share data: 'Shared (ACL)' shares a single folder among all users, 'Copy' duplicates it for each user, 'Overlay' creates a per-user copy-on-write overlay, and 'Bind mount' exposes the folder at the same path inside each player's home.")
            type: Kirigami.MessageType.Information
            visible: true
        }
        Kirigami.Heading {
            level: 3
            text: i18nc("@title", "Required integrations")
            Layout.fillWidth: true
        }

        Repeater {
            model: root.presetManager ? root.presetManager.availableIntegrations() : []
            delegate: Controls.CheckBox {
                objectName: "integration_" + modelData.id
                text: modelData.id === "steam"
                      ? i18nc("@option:check", "Steam")
                      : i18nc("@option:check", "Heroic Games Launcher")
                checked: root.integrationSelected(modelData.id)
                enabled: modelData.available || checked
                Accessible.name: text
                onToggled: root.setIntegration(modelData.id, checked)
            }
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

                    // Capture the delegate's row before ComboBox handlers
                    // shadow `index` with their own signal parameter
                    readonly property int rowIndex: index

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

                    Controls.ComboBox {
                        model: [
                            { value: "acl", text: i18nc("@item:inlistbox", "Shared (ACL)") },
                            { value: "copy", text: i18nc("@item:inlistbox", "Copy files") },
                            { value: "overlay", text: i18nc("@item:inlistbox", "Per-user overlay") },
                            { value: "bind", text: i18nc("@item:inlistbox", "Bind mount (same home path)") }
                        ]
                        textRole: "text"
                        valueRole: "value"
                        currentIndex: {
                            if (mode === "copy") return 1;
                            if (mode === "overlay") return 2;
                            if (mode === "bind") return 3;
                            return 0;
                        }
                        onActivated: {
                            directoriesModel.setProperty(rowIndex, "mode", currentValue)
                            root.presetManager.setDataDirectories(root.presetId, root.getDirectoriesArray())
                        }
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
            const resolvedPath = root.presetManager.resolveHostPath(path)
            if (resolvedPath === "") {
                applicationWindow().showPassiveNotification(
                    i18nc("@info", "Could not resolve the selected directory for the host helper"), "long")
                return
            }
            path = resolvedPath
            
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
