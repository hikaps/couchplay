// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 CouchPlay Contributors

import QtQuick
import QtQuick.Layouts
import QtQuick.Controls as Controls
import org.kde.kirigami as Kirigami

Kirigami.Dialog {
    id: root
    objectName: "dialogAddToSteam"
    Accessible.role: Accessible.Dialog
    Accessible.name: title
    title: i18nc("@title:dialog", "Add Profile to Steam")
    preferredWidth: Kirigami.Units.gridUnit * 32
    standardButtons: Kirigami.Dialog.Cancel

    required property var manager
    property string profileName: ""
    property bool restartConfirmed: false
    property bool completed: false
    property string errorText: ""

    onRejected: {
        if (manager && manager.busy) {
            manager.cancel()
        }
        close()
    }

    Connections {
        target: root.manager
        function onRegistrationFinished(name, updated, steamReopened) {
            if (name !== root.profileName) return
            root.completed = true
            root.errorText = steamReopened || !root.manager.accounts[root.comboSteamAccount.currentIndex].running
                ? i18nc("@info", "Profile added to Steam.")
                : i18nc("@info", "Profile saved, but Steam could not be reopened.")
        }
        function onErrorOccurred(message) {
            root.errorText = message
        }
    }

    contentItem: ColumnLayout {
        spacing: Kirigami.Units.largeSpacing

        Controls.Label {
            Layout.fillWidth: true
            text: i18nc("@info", "Profile: %1", root.profileName)
            wrapMode: Text.WordWrap
        }

        Kirigami.InlineMessage {
            Layout.fillWidth: true
            visible: root.manager?.gameMode ?? false
            type: Kirigami.MessageType.Warning
            text: i18nc("@info", "Switch to Desktop Mode to add this profile to Steam.")
        }

        Controls.Label {
            Layout.fillWidth: true
            visible: !(root.manager?.gameMode ?? false)
            text: i18nc("@label", "Steam account")
        }

        Controls.ComboBox {
            id: comboSteamAccount
            objectName: "comboSteamAccount"
            Layout.fillWidth: true
            visible: !(root.manager?.gameMode ?? false)
            model: root.manager ? root.manager.accounts : []
            textRole: "label"
            enabled: !root.manager?.busy && !root.completed
        }

        Kirigami.InlineMessage {
            Layout.fillWidth: true
            visible: root.errorText !== ""
            type: root.completed ? Kirigami.MessageType.Positive : Kirigami.MessageType.Error
            text: root.errorText
        }

        Controls.Label {
            Layout.fillWidth: true
            visible: root.manager?.busy ?? false
            text: root.manager ? root.manager.status : ""
            wrapMode: Text.WordWrap
        }

        Controls.Label {
            Layout.fillWidth: true
            visible: root.restartConfirmed && !root.completed
            text: i18nc("@warning", "Steam will close and reopen. Finish any running games first.")
            wrapMode: Text.WordWrap
        }

        RowLayout {
            Layout.fillWidth: true
            visible: !(root.manager?.gameMode ?? false)
            spacing: Kirigami.Units.smallSpacing

            Controls.Button {
                id: btnConfirmAddToSteam
                objectName: "btnConfirmAddToSteam"
                Layout.fillWidth: true
                enabled: !root.manager?.busy && !root.completed && comboSteamAccount.currentIndex >= 0
                text: {
                    if (root.restartConfirmed) return i18nc("@action:button", "Close Steam, Add, and Reopen")
                    if (comboSteamAccount.currentIndex >= 0
                        && root.manager.accounts[comboSteamAccount.currentIndex].running) {
                        return i18nc("@action:button", "Continue")
                    }
                    return i18nc("@action:button", "Add to Steam")
                }
                onClicked: {
                    const running = root.manager.accounts[comboSteamAccount.currentIndex].running
                    if (running && !root.restartConfirmed) {
                        root.restartConfirmed = true
                    } else {
                        root.manager.addToSteam(comboSteamAccount.currentIndex, root.restartConfirmed)
                    }
                }
            }

            Controls.Button {
                id: btnOpenSteam
                objectName: "btnOpenSteam"
                visible: root.completed
                text: i18nc("@action:button", "Open Steam")
                onClicked: root.manager.openSteam(comboSteamAccount.currentIndex)
            }
        }
    }
}
