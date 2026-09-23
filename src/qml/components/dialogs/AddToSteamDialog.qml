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
    standardButtons: manager?.busy && !manager?.cancellable ? 0 : Kirigami.Dialog.Cancel
    closePolicy: manager?.busy && !manager?.cancellable ? Controls.Popup.NoAutoClose : Controls.Popup.CloseOnEscape | Controls.Popup.CloseOnPressOutside

    required property var manager
    property string profileName: ""
    property bool restartConfirmed: false
    property bool completed: false
    property string errorText: ""

    function selectedAccount() {
        const accounts = root.manager ? root.manager.accounts : []
        const index = comboSteamAccount.currentIndex
        return index >= 0 && index < accounts.length ? accounts[index] : null
    }

    onRejected: {
        if (manager?.busy && !manager.cancellable) return
        if (manager?.busy) manager.cancel()
        close()
    }

    Connections {
        target: root.manager
        function onRegistrationFinished(name, updated, steamReopened) {
            if (name !== root.profileName) return
            const account = root.selectedAccount()
            root.completed = true
            root.errorText = !account || (account.running && !steamReopened)
                ? i18nc("@info", "Profile saved, but Steam could not be reopened.")
                : i18nc("@info", "Profile added to Steam.")
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
            Accessible.name: i18nc("@label", "Steam account")
            Layout.fillWidth: true
            visible: !(root.manager?.gameMode ?? false)
            model: root.manager ? root.manager.accounts : []
            textRole: "label"
            onCurrentIndexChanged: root.restartConfirmed = false
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
                enabled: !root.manager?.busy && !root.completed && root.selectedAccount() !== null
                text: {
                    if (root.restartConfirmed) return i18nc("@action:button", "Close Steam, Add, and Reopen")
                    const account = root.selectedAccount()
                    if (account && account.running) {
                        return i18nc("@action:button", "Continue")
                    }
                    return i18nc("@action:button", "Add to Steam")
                }
                onClicked: {
                    const account = root.selectedAccount()
                    if (!account) return
                    if (account.running && !root.restartConfirmed) {
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
                enabled: root.selectedAccount() !== null && !root.manager?.busy
                text: i18nc("@action:button", "Open Steam")
                onClicked: {
                    if (root.selectedAccount()) root.manager.openSteam(comboSteamAccount.currentIndex)
                }
            }
        }
    }
}
