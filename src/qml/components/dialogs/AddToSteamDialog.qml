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
    property string errorText: ""

    onOpened: root.errorText = ""

    Connections {
        target: root.manager
        function onErrorOccurred(message) {
            root.errorText = message
        }
    }

    contentItem: ColumnLayout {
        spacing: Kirigami.Units.largeSpacing

        Controls.Label {
            objectName: "labelProfileToAdd"
            Layout.fillWidth: true
            text: i18nc("@info", "Profile: %1", root.profileName)
            wrapMode: Text.WordWrap
        }

        Kirigami.InlineMessage {
            objectName: "messageCloseSteam"
            Accessible.name: text
            Layout.fillWidth: true
            type: Kirigami.MessageType.Warning
            text: i18nc(
                "@warning",
                "CLOSE STEAM MANUALLY BEFORE ADDING. Reopen Steam manually afterward."
            )
        }

        Controls.Label {
            Layout.fillWidth: true
            text: i18nc("@label", "Steam account")
        }

        Controls.ComboBox {
            id: comboSteamAccount
            objectName: "comboSteamAccount"
            Accessible.name: i18nc("@label", "Steam account")
            Layout.fillWidth: true
            model: root.manager ? root.manager.accounts : []
            textRole: "label"
        }

        Kirigami.InlineMessage {
            objectName: "messageSteamError"
            Accessible.name: text
            Layout.fillWidth: true
            visible: root.errorText !== ""
            type: Kirigami.MessageType.Error
            text: root.errorText
        }

        Controls.Button {
            id: btnConfirmAddToSteam
            objectName: "btnConfirmAddToSteam"
            Layout.fillWidth: true
            enabled: comboSteamAccount.currentIndex >= 0
            highlighted: true
            text: i18nc("@action:button", "Add to Steam")
            onClicked: root.manager.addToSteam(comboSteamAccount.currentIndex)
        }
    }
}
