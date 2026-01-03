// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2024 hikaps

import QtQuick
import QtQuick.Layouts
import QtQuick.Controls as Controls
import org.kde.kirigami as Kirigami

Kirigami.ScrollablePage {
    id: root
    title: i18nc("@title", "Profiles")

    actions: [
        Kirigami.Action {
            icon.name: "list-add"
            text: i18nc("@action:button", "New Profile")
            onTriggered: {
                applicationWindow().pageStack.push(sessionSetupPage)
            }
        }
    ]

    Kirigami.PlaceholderMessage {
        anchors.centerIn: parent
        text: i18nc("@info", "No Saved Profiles")
        explanation: i18nc("@info", "Create a session and save it as a profile for quick access.")
        icon.name: "bookmark"

        helpfulAction: Kirigami.Action {
            icon.name: "list-add"
            text: i18nc("@action:button", "Create New Session")
            onTriggered: {
                applicationWindow().pageStack.push(sessionSetupPage)
            }
        }
    }
}
