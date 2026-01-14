// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 CouchPlay Contributors

import QtQuick
import QtQuick.Layouts
import QtQuick.Controls as Controls
import org.kde.kirigami as Kirigami

Kirigami.ScrollablePage {
    id: root
    title: i18nc("@title", "Profiles")

    // Backend managers (from Main.qml)
    required property var sessionManager
    required property var sessionRunner

    // Refresh profiles when page becomes visible
    Component.onCompleted: {
        if (sessionManager) {
            sessionManager.refreshProfiles()
        }
    }

    actions: [
        Kirigami.Action {
            icon.name: "list-add"
            text: i18nc("@action:button", "New Profile")
            onTriggered: {
                applicationWindow().pushSessionSetupPage()
            }
        },
        Kirigami.Action {
            icon.name: "view-refresh"
            text: i18nc("@action:button", "Refresh")
            onTriggered: sessionManager?.refreshProfiles()
        }
    ]

    // Delete confirmation dialog
    Kirigami.PromptDialog {
        id: deleteDialog
        title: i18nc("@title:dialog", "Delete Profile")
        subtitle: i18nc("@info", "Are you sure you want to delete the profile '%1'?", deleteDialog.profileName)
        standardButtons: Kirigami.Dialog.Yes | Kirigami.Dialog.No
        
        property string profileName: ""

        onAccepted: {
            if (sessionManager?.deleteProfile(profileName)) {
                applicationWindow().showPassiveNotification(
                    i18nc("@info", "Profile deleted: %1", profileName))
            }
        }
    }

    // Main content
    ColumnLayout {
        spacing: Kirigami.Units.largeSpacing
        visible: (sessionManager?.savedProfiles?.length ?? 0) > 0

        // Profile grid
        GridLayout {
            Layout.fillWidth: true
            columns: Math.max(1, Math.floor(root.width / (Kirigami.Units.gridUnit * 20)))
            rowSpacing: Kirigami.Units.largeSpacing
            columnSpacing: Kirigami.Units.largeSpacing

            Repeater {
                model: sessionManager ? sessionManager.savedProfiles : []

                delegate: ProfileCard {
                    id: profileDelegate
                    Layout.fillWidth: true

                    required property var modelData
                    required property int index

                    profile: modelData
                    isCurrentProfile: sessionManager && modelData && sessionManager.currentProfileName === modelData.name

                    onLoadProfile: {
                        if (modelData && sessionManager?.loadProfile(modelData.name)) {
                            applicationWindow().showPassiveNotification(
                                i18nc("@info", "Loaded profile: %1", modelData.name))
                            applicationWindow().pushSessionSetupPage()
                        }
                    }

                    onLaunchProfile: {
                        if (modelData && sessionManager?.loadProfile(modelData.name)) {
                            if (sessionRunner?.start()) {
                                applicationWindow().showPassiveNotification(
                                    i18nc("@info", "Starting session: %1", modelData.name))
                            }
                        }
                    }

                    onDeleteProfile: {
                        if (modelData) {
                            deleteDialog.profileName = modelData.name
                            deleteDialog.open()
                        }
                    }
                }
            }
        }
    }

    // Empty state
    Kirigami.PlaceholderMessage {
        anchors.centerIn: parent
        visible: (sessionManager?.savedProfiles?.length ?? 0) === 0
        width: parent.width - Kirigami.Units.gridUnit * 4

        text: i18nc("@info", "No Saved Profiles")
        explanation: i18nc("@info", "Create a session and save it as a profile for quick access.")
        icon.name: "bookmark"

        helpfulAction: Kirigami.Action {
            icon.name: "list-add"
            text: i18nc("@action:button", "Create New Session")
            onTriggered: {
                applicationWindow().pushSessionSetupPage()
            }
        }
    }

    // Profile card component - Minimal Modern Design
    component ProfileCard: Kirigami.Card {
        id: profileCard

        required property var profile
        property bool isCurrentProfile: false

        signal loadProfile()
        signal launchProfile()
        signal deleteProfile()

        background: Rectangle {
            color: profileCard.isCurrentProfile 
                ? Qt.alpha(Kirigami.Theme.highlightColor, 0.1)
                : Kirigami.Theme.backgroundColor
            radius: Kirigami.Units.smallSpacing
            border.width: profileCard.isCurrentProfile ? 2 : 1
            border.color: profileCard.isCurrentProfile 
                ? Kirigami.Theme.highlightColor 
                : Qt.alpha(Kirigami.Theme.textColor, 0.1)
        }

        contentItem: ColumnLayout {
            spacing: Kirigami.Units.smallSpacing

            // Top section: Icon and title
            RowLayout {
                Layout.fillWidth: true
                spacing: Kirigami.Units.mediumSpacing

                // Large profile icon
                Rectangle {
                    Layout.preferredWidth: Kirigami.Units.iconSizes.large
                    Layout.preferredHeight: Kirigami.Units.iconSizes.large
                    radius: Kirigami.Units.smallSpacing
                    color: Qt.alpha(Kirigami.Theme.highlightColor, 0.15)

                    Kirigami.Icon {
                        anchors.centerIn: parent
                        source: "bookmark"
                        width: Kirigami.Units.iconSizes.medium
                        height: Kirigami.Units.iconSizes.medium
                        color: Kirigami.Theme.highlightColor
                    }
                }

                // Title and subtitle
                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 2

                    Controls.Label {
                        text: profile?.name ?? ""
                        font.bold: true
                        font.pointSize: Kirigami.Theme.defaultFont.pointSize * 1.1
                        elide: Text.ElideRight
                        Layout.fillWidth: true
                    }

                    RowLayout {
                        spacing: Kirigami.Units.smallSpacing
                        Layout.fillWidth: true

                        Kirigami.Icon {
                            source: {
                                const layout = profile?.layout ?? "horizontal"
                                switch (layout) {
                                    case "horizontal": return "view-split-left-right"
                                    case "vertical": return "view-split-top-bottom"
                                    case "grid": return "view-grid"
                                    case "multi-monitor": return "video-display"
                                    default: return "view-split-left-right"
                                }
                            }
                            Layout.preferredWidth: Kirigami.Units.iconSizes.small
                            Layout.preferredHeight: Kirigami.Units.iconSizes.small
                            opacity: 0.7
                        }

                        Controls.Label {
                            text: {
                                const layout = profile?.layout ?? "horizontal"
                                switch (layout) {
                                    case "horizontal": return i18nc("@info", "Side by Side")
                                    case "vertical": return i18nc("@info", "Top and Bottom")
                                    case "grid": return i18nc("@info", "Grid")
                                    case "multi-monitor": return i18nc("@info", "Multi-Monitor")
                                    default: return layout
                                }
                            }
                            opacity: 0.7
                            font.pointSize: Kirigami.Theme.smallFont.pointSize
                        }
                    }
                }

                // Status indicator (top right)
                Kirigami.Chip {
                    visible: profileCard.isCurrentProfile
                    text: i18nc("@info", "Loaded")
                    icon.name: "checkmark"
                    closable: false
                    checkable: false
                }
            }

            // Action buttons row
            RowLayout {
                Layout.fillWidth: true
                spacing: Kirigami.Units.smallSpacing

                Controls.Button {
                    text: i18nc("@action:button", "Launch")
                    icon.name: "media-playback-start"
                    highlighted: true
                    onClicked: profileCard.launchProfile()
                }

                Controls.Button {
                    text: i18nc("@action:button", "Edit")
                    icon.name: "document-edit"
                    flat: true
                    onClicked: profileCard.loadProfile()
                }

                Item { Layout.fillWidth: true }

                Controls.Button {
                    icon.name: "edit-delete"
                    flat: true
                    display: Controls.AbstractButton.IconOnly
                    
                    Controls.ToolTip.visible: hovered
                    Controls.ToolTip.text: i18nc("@info:tooltip", "Delete profile")
                    
                    onClicked: profileCard.deleteProfile()
                }
            }
        }
    }
}
