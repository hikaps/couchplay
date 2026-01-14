// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 CouchPlay Contributors

import QtQuick
import QtQuick.Layouts
import QtQuick.Controls as Controls
import org.kde.kirigami as Kirigami
import Qt.labs.platform as Platform

import "../components" as Components

Kirigami.ScrollablePage {
    id: root

    title: i18nc("@title", "Home")

    // Backend managers (from Main.qml)
    required property var sessionManager
    required property var sessionRunner
    required property var deviceManager
    required property var helperClient

    // Computed properties to avoid repeating conditions
    property bool helperAvailable: helperClient?.available ?? false
    property bool hasControllers: deviceManager && deviceManager.controllers.length > 0

    // Refresh on page load
    Component.onCompleted: {
        if (sessionManager) {
            sessionManager.refreshProfiles()
        }
    }

    header: Controls.Control {
        padding: Kirigami.Units.largeSpacing

        background: Rectangle {
            color: Kirigami.Theme.backgroundColor
        }

        contentItem: ColumnLayout {
            spacing: Kirigami.Units.smallSpacing

            RowLayout {
                Layout.fillWidth: true

                ColumnLayout {
                    Layout.fillWidth: true
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

                // Session status indicator
                Kirigami.Chip {
                    visible: sessionRunner?.running ?? false
                    text: i18nc("@info", "%1 instances running", sessionRunner ? sessionRunner.runningInstanceCount : 0)
                    icon.name: "media-playback-start"
                    closable: true
                    onRemoved: sessionRunner.stop()
                }
            }
        }
    }

    ColumnLayout {
        spacing: Kirigami.Units.largeSpacing

        // Active session banner
        Kirigami.InlineMessage {
            Layout.fillWidth: true
            visible: sessionRunner?.running ?? false
            type: Kirigami.MessageType.Positive
            text: i18nc("@info", "Session is running: %1", sessionRunner ? sessionRunner.status : "")
            
            actions: [
                Kirigami.Action {
                    text: i18nc("@action:button", "View Session")
                    icon.name: "view-visible"
                    onTriggered: applicationWindow().pushSessionSetupPage()
                },
                Kirigami.Action {
                    text: i18nc("@action:button", "Stop")
                    icon.name: "media-playback-stop"
                    onTriggered: sessionRunner.stop()
                }
            ]
        }

        // Quick Actions
        Kirigami.Heading {
            text: i18nc("@title", "Quick Actions")
            level: 2
        }

        RowLayout {
            spacing: Kirigami.Units.largeSpacing
            Layout.fillWidth: true

            Components.ActionCard {
                Layout.fillWidth: true
                Layout.preferredHeight: Kirigami.Units.gridUnit * 8
                iconName: "list-add"
                title: i18nc("@action", "New Session")
                description: i18nc("@info", "Configure and start a new split-screen session")
                onClicked: {
                    if (sessionManager) {
                        sessionManager.newSession()
                    }
                    applicationWindow().pageStack.clear()
                    applicationWindow().pushSessionSetupPage()
                }
            }

            Components.ActionCard {
                Layout.fillWidth: true
                Layout.preferredHeight: Kirigami.Units.gridUnit * 8
                iconName: "bookmark"
                title: i18nc("@action", "Load Profile")
                description: i18nc("@info", "Launch a saved session profile")
                badgeCount: sessionManager ? sessionManager.savedProfiles.length : 0
                onClicked: {
                    applicationWindow().pageStack.clear()
                    applicationWindow().pushProfilesPage()
                }
            }

            Components.ActionCard {
                Layout.fillWidth: true
                Layout.preferredHeight: Kirigami.Units.gridUnit * 8
                iconName: "input-gamepad"
                title: i18nc("@action", "Manage Devices")
                description: i18nc("@info", "View and configure input devices")
                badgeCount: deviceManager ? deviceManager.controllers.length : 0
                onClicked: {
                    applicationWindow().pageStack.clear()
                    applicationWindow().pushDeviceAssignmentPage()
                }
            }
        }

        // Recent Profiles
        Kirigami.Heading {
            text: i18nc("@title", "Recent Profiles")
            level: 2
            Layout.topMargin: Kirigami.Units.largeSpacing
        }

        // Show recent profiles or placeholder
        GridLayout {
            Layout.fillWidth: true
            columns: Math.max(1, Math.floor(root.width / (Kirigami.Units.gridUnit * 16)))
            rowSpacing: Kirigami.Units.smallSpacing
            columnSpacing: Kirigami.Units.smallSpacing
            visible: (sessionManager?.savedProfiles?.length ?? 0) > 0

            Repeater {
                // Show up to 4 recent profiles
                model: sessionManager ? sessionManager.savedProfiles.slice(0, 4) : []

                delegate: Kirigami.AbstractCard {
                    Layout.fillWidth: true

                    contentItem: RowLayout {
                        spacing: Kirigami.Units.smallSpacing

                        Kirigami.Icon {
                            source: "bookmark"
                            Layout.preferredWidth: Kirigami.Units.iconSizes.medium
                            Layout.preferredHeight: Kirigami.Units.iconSizes.medium
                        }

                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 0

                            Controls.Label {
                                text: modelData.name
                                font.bold: true
                                elide: Text.ElideRight
                                Layout.fillWidth: true
                            }

                            Controls.Label {
                                text: {
                                    switch (modelData.layout) {
                                        case "horizontal": return i18nc("@info", "Side by Side")
                                        case "vertical": return i18nc("@info", "Top and Bottom")
                                        case "grid": return i18nc("@info", "Grid")
                                        case "multi-monitor": return i18nc("@info", "Multi-Monitor")
                                        default: return modelData.layout
                                    }
                                }
                                opacity: 0.7
                                font: Kirigami.Theme.smallFont
                            }
                        }

                        Controls.Button {
                            icon.name: "media-playback-start"
                            flat: true
                            display: Controls.AbstractButton.IconOnly
                            
                            Controls.ToolTip.visible: hovered
                            Controls.ToolTip.text: i18nc("@info:tooltip", "Launch this profile")
                            
                            onClicked: {
                                if (sessionManager.loadProfile(modelData.name)) {
                                    sessionRunner.start()
                                }
                            }
                        }
                    }
                }
            }
        }

        // Empty state for profiles
        Kirigami.PlaceholderMessage {
            visible: !sessionManager || sessionManager.savedProfiles.length === 0
            text: i18nc("@info", "No profiles yet")
            explanation: i18nc("@info", "Create a new session and save it as a profile for quick access.")
            icon.name: "bookmark"
            Layout.fillWidth: true

            helpfulAction: Kirigami.Action {
                icon.name: "list-add"
                text: i18nc("@action:button", "Create New Session")
                onTriggered: {
                    applicationWindow().pageStack.clear()
                    applicationWindow().pushSessionSetupPage()
                }
            }
        }

        // View all profiles link
        Controls.Button {
            visible: (sessionManager?.savedProfiles?.length ?? 0) > 4
            text: i18nc("@action:button", "View all %1 profiles...", sessionManager ? sessionManager.savedProfiles.length : 0)
            flat: true
            Layout.alignment: Qt.AlignRight
            onClicked: {
                applicationWindow().pageStack.clear()
                applicationWindow().pushProfilesPage()
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
            wideMode: root.width > Kirigami.Units.gridUnit * 30

            RowLayout {
                Kirigami.FormData.label: i18nc("@label", "Gamescope:")
                spacing: Kirigami.Units.smallSpacing

                Kirigami.Icon {
                    source: root.gamescopeAvailable ? "emblem-ok-symbolic" : "emblem-error-symbolic"
                    color: root.gamescopeAvailable ? Kirigami.Theme.positiveTextColor : Kirigami.Theme.negativeTextColor
                    Layout.preferredWidth: Kirigami.Units.iconSizes.small
                    Layout.preferredHeight: Kirigami.Units.iconSizes.small
                }

                Controls.Label {
                    text: root.gamescopeAvailable ? i18nc("@info", "Available") : i18nc("@info", "Not found")
                    color: root.gamescopeAvailable ? Kirigami.Theme.positiveTextColor : Kirigami.Theme.negativeTextColor
                }
            }

            RowLayout {
                Kirigami.FormData.label: i18nc("@label", "Steam:")
                spacing: Kirigami.Units.smallSpacing

                Kirigami.Icon {
                    source: root.steamAvailable ? "emblem-ok-symbolic" : "emblem-error-symbolic"
                    color: root.steamAvailable ? Kirigami.Theme.positiveTextColor : Kirigami.Theme.negativeTextColor
                    Layout.preferredWidth: Kirigami.Units.iconSizes.small
                    Layout.preferredHeight: Kirigami.Units.iconSizes.small
                }

                Controls.Label {
                    text: root.steamAvailable ? i18nc("@info", "Available") : i18nc("@info", "Not found")
                    color: root.steamAvailable ? Kirigami.Theme.positiveTextColor : Kirigami.Theme.negativeTextColor
                }
            }

            RowLayout {
                Kirigami.FormData.label: i18nc("@label", "Controllers:")
                spacing: Kirigami.Units.smallSpacing

                Kirigami.Icon {
                    source: root.hasControllers ? "emblem-ok-symbolic" : "emblem-warning-symbolic"
                    color: root.hasControllers ? Kirigami.Theme.positiveTextColor : Kirigami.Theme.neutralTextColor
                    Layout.preferredWidth: Kirigami.Units.iconSizes.small
                    Layout.preferredHeight: Kirigami.Units.iconSizes.small
                }

                Controls.Label {
                    text: deviceManager 
                        ? i18ncp("@info", "%1 detected", "%1 detected", deviceManager.controllers.length)
                        : i18nc("@info", "Unknown")
                    color: root.hasControllers ? Kirigami.Theme.positiveTextColor : Kirigami.Theme.neutralTextColor
                }
            }

            RowLayout {
                Kirigami.FormData.label: i18nc("@label", "Helper Service:")
                spacing: Kirigami.Units.smallSpacing

                Kirigami.Icon {
                    source: root.helperAvailable ? "emblem-ok-symbolic" : "emblem-warning-symbolic"
                    color: root.helperAvailable ? Kirigami.Theme.positiveTextColor : Kirigami.Theme.neutralTextColor
                    Layout.preferredWidth: Kirigami.Units.iconSizes.small
                    Layout.preferredHeight: Kirigami.Units.iconSizes.small
                }

                Controls.Label {
                    text: root.helperAvailable ? i18nc("@info", "Connected") : i18nc("@info", "Not configured")
                    color: root.helperAvailable ? Kirigami.Theme.positiveTextColor : Kirigami.Theme.neutralTextColor
                }

                Controls.Button {
                    visible: !root.helperAvailable
                    text: i18nc("@action:button", "Setup")
                    flat: true
                    onClicked: applicationWindow().pushSettingsPage()
                }
            }
        }
    }

    // System check properties - check if executables exist in PATH
    property bool gamescopeAvailable: Platform.StandardPaths.findExecutable("gamescope") !== ""
    property bool steamAvailable: Platform.StandardPaths.findExecutable("steam") !== ""
}
