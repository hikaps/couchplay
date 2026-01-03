// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2024 hikaps

import QtQuick
import QtQuick.Layouts
import QtQuick.Controls as Controls
import org.kde.kirigami as Kirigami

Kirigami.ScrollablePage {
    id: root
    title: i18nc("@title", "Users")

    actions: [
        Kirigami.Action {
            icon.name: "list-add"
            text: i18nc("@action:button", "Add User")
            onTriggered: addUserDialog.open()
        }
    ]

    ColumnLayout {
        spacing: Kirigami.Units.largeSpacing

        Kirigami.Heading {
            text: i18nc("@title", "Available Users")
            level: 2
        }

        Controls.Label {
            text: i18nc("@info", "These Linux users can be used for split-screen gaming sessions.")
            wrapMode: Text.WordWrap
            Layout.fillWidth: true
            opacity: 0.7
        }

        Kirigami.Card {
            Layout.fillWidth: true

            contentItem: RowLayout {
                spacing: Kirigami.Units.largeSpacing

                Kirigami.Icon {
                    source: "user"
                    Layout.preferredWidth: Kirigami.Units.iconSizes.medium
                    Layout.preferredHeight: Kirigami.Units.iconSizes.medium
                }

                ColumnLayout {
                    spacing: Kirigami.Units.smallSpacing
                    Layout.fillWidth: true

                    Controls.Label {
                        text: "Current User"
                        font.bold: true
                    }

                    Controls.Label {
                        text: i18nc("@info", "Primary user (you)")
                        opacity: 0.7
                    }
                }

                Kirigami.Chip {
                    text: i18nc("@info", "Primary")
                }
            }
        }

        Kirigami.InlineMessage {
            Layout.fillWidth: true
            text: i18nc("@info", "To add more users, you need to run the CouchPlay helper installer.")
            type: Kirigami.MessageType.Information
            visible: true
        }
    }

    Kirigami.Dialog {
        id: addUserDialog
        title: i18nc("@title:dialog", "Add User")
        standardButtons: Kirigami.Dialog.Ok | Kirigami.Dialog.Cancel

        Kirigami.FormLayout {
            Controls.TextField {
                id: usernameField
                Kirigami.FormData.label: i18nc("@label", "Username:")
                placeholderText: i18nc("@info:placeholder", "player2")
            }
        }
    }
}
