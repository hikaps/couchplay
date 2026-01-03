// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2024 hikaps

import QtQuick
import QtQuick.Layouts
import QtQuick.Controls as Controls
import org.kde.kirigami as Kirigami

Kirigami.ScrollablePage {
    id: root

    title: i18nc("@title", "Home")

    header: Controls.Control {
        padding: Kirigami.Units.largeSpacing

        background: Rectangle {
            color: Kirigami.Theme.backgroundColor
        }

        contentItem: ColumnLayout {
            spacing: Kirigami.Units.smallSpacing

            Kirigami.Heading {
                text: i18nc("@title", "Welcome to CouchPlay")
                level: 1
            }

            Controls.Label {
                text: i18nc("@info", "Split-screen gaming made easy. Run multiple Steam instances simultaneously.")
                wrapMode: Text.WordWrap
                Layout.fillWidth: true
                opacity: 0.7
            }
        }
    }

    ColumnLayout {
        spacing: Kirigami.Units.largeSpacing

        // Quick Actions
        Kirigami.Heading {
            text: i18nc("@title", "Quick Actions")
            level: 2
        }

        RowLayout {
            spacing: Kirigami.Units.largeSpacing
            Layout.fillWidth: true

            Kirigami.Card {
                Layout.fillWidth: true
                Layout.preferredHeight: Kirigami.Units.gridUnit * 8

                contentItem: ColumnLayout {
                    spacing: Kirigami.Units.smallSpacing

                    Kirigami.Icon {
                        source: "list-add"
                        Layout.preferredWidth: Kirigami.Units.iconSizes.huge
                        Layout.preferredHeight: Kirigami.Units.iconSizes.huge
                        Layout.alignment: Qt.AlignHCenter
                    }

                    Controls.Label {
                        text: i18nc("@action", "New Session")
                        font.bold: true
                        Layout.alignment: Qt.AlignHCenter
                    }

                    Controls.Label {
                        text: i18nc("@info", "Configure and start a new split-screen session")
                        wrapMode: Text.WordWrap
                        horizontalAlignment: Text.AlignHCenter
                        Layout.fillWidth: true
                        opacity: 0.7
                    }
                }

                onClicked: {
                    applicationWindow().pageStack.clear()
                    applicationWindow().pageStack.push(sessionSetupPage)
                }
            }

            Kirigami.Card {
                Layout.fillWidth: true
                Layout.preferredHeight: Kirigami.Units.gridUnit * 8

                contentItem: ColumnLayout {
                    spacing: Kirigami.Units.smallSpacing

                    Kirigami.Icon {
                        source: "bookmark"
                        Layout.preferredWidth: Kirigami.Units.iconSizes.huge
                        Layout.preferredHeight: Kirigami.Units.iconSizes.huge
                        Layout.alignment: Qt.AlignHCenter
                    }

                    Controls.Label {
                        text: i18nc("@action", "Load Profile")
                        font.bold: true
                        Layout.alignment: Qt.AlignHCenter
                    }

                    Controls.Label {
                        text: i18nc("@info", "Launch a saved session profile")
                        wrapMode: Text.WordWrap
                        horizontalAlignment: Text.AlignHCenter
                        Layout.fillWidth: true
                        opacity: 0.7
                    }
                }

                onClicked: {
                    applicationWindow().pageStack.clear()
                    applicationWindow().pageStack.push(profilesPage)
                }
            }
        }

        // Recent Profiles
        Kirigami.Heading {
            text: i18nc("@title", "Recent Profiles")
            level: 2
            Layout.topMargin: Kirigami.Units.largeSpacing
        }

        Kirigami.PlaceholderMessage {
            text: i18nc("@info", "No profiles yet")
            explanation: i18nc("@info", "Create a new session and save it as a profile for quick access.")
            icon.name: "bookmark"
            Layout.fillWidth: true

            helpfulAction: Kirigami.Action {
                icon.name: "list-add"
                text: i18nc("@action:button", "Create New Session")
                onTriggered: {
                    applicationWindow().pageStack.clear()
                    applicationWindow().pageStack.push(sessionSetupPage)
                }
            }
        }

        // System Status
        Kirigami.Heading {
            text: i18nc("@title", "System Status")
            level: 2
            Layout.topMargin: Kirigami.Units.largeSpacing
        }

        Kirigami.FormLayout {
            Layout.fillWidth: true

            Controls.Label {
                Kirigami.FormData.label: i18nc("@label", "Gamescope:")
                text: checkGamescope() ? i18nc("@info", "Available") : i18nc("@info", "Not found")
                color: checkGamescope() ? Kirigami.Theme.positiveTextColor : Kirigami.Theme.negativeTextColor
            }

            Controls.Label {
                Kirigami.FormData.label: i18nc("@label", "Steam:")
                text: checkSteam() ? i18nc("@info", "Available") : i18nc("@info", "Not found")
                color: checkSteam() ? Kirigami.Theme.positiveTextColor : Kirigami.Theme.negativeTextColor
            }

            Controls.Label {
                Kirigami.FormData.label: i18nc("@label", "Helper Service:")
                text: i18nc("@info", "Not configured")
                color: Kirigami.Theme.neutralTextColor
            }
        }
    }

    // Helper functions
    function checkGamescope(): bool {
        // TODO: Implement actual check
        return true
    }

    function checkSteam(): bool {
        // TODO: Implement actual check
        return true
    }
}
