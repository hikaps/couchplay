// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2024 hikaps

import QtQuick
import QtQuick.Layouts
import QtQuick.Controls as Controls
import org.kde.kirigami as Kirigami

Kirigami.ScrollablePage {
    id: root
    title: i18nc("@title", "Games")

    actions: [
        Kirigami.Action {
            icon.name: "list-add"
            text: i18nc("@action:button", "Add Game")
            onTriggered: addGameDialog.open()
        }
    ]

    Kirigami.PlaceholderMessage {
        anchors.centerIn: parent
        text: i18nc("@info", "No Games Added")
        explanation: i18nc("@info", "Add games to quickly launch them with your split-screen profiles.")
        icon.name: "applications-games"

        helpfulAction: Kirigami.Action {
            icon.name: "list-add"
            text: i18nc("@action:button", "Add Game")
            onTriggered: addGameDialog.open()
        }
    }

    Kirigami.Dialog {
        id: addGameDialog
        title: i18nc("@title:dialog", "Add Game")
        standardButtons: Kirigami.Dialog.Ok | Kirigami.Dialog.Cancel

        Kirigami.FormLayout {
            Controls.TextField {
                id: gameNameField
                Kirigami.FormData.label: i18nc("@label", "Name:")
                placeholderText: i18nc("@info:placeholder", "Game name")
            }

            Controls.TextField {
                id: gameCommandField
                Kirigami.FormData.label: i18nc("@label", "Command:")
                placeholderText: i18nc("@info:placeholder", "steam://rungameid/123456")
            }
        }
    }
}
