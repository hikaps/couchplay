// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2024 hikaps

import QtQuick
import QtQuick.Layouts
import QtQuick.Controls as Controls
import org.kde.kirigami as Kirigami

import "../components" as Components

Kirigami.ScrollablePage {
    id: root
    title: i18nc("@title", "Users")

    required property var userManager
    required property var helperClient

    // Track which user is being deleted
    property string userToDelete: ""

    actions: [
        Kirigami.Action {
            icon.name: "view-refresh"
            text: i18nc("@action:button", "Refresh")
            onTriggered: userManager?.refresh()
        },
        Kirigami.Action {
            icon.name: "list-add"
            text: i18nc("@action:button", "Add User")
            onTriggered: {
                usernameField.text = ""
                validationMessage.text = ""
                addUserDialog.open()
            }
        }
    ]

    Connections {
        target: userManager ?? null
        function onUserCreated(username) {
            applicationWindow().showPassiveNotification(
                i18nc("@info", "User '%1' created successfully", username))
        }
        function onUserDeleted(username) {
            applicationWindow().showPassiveNotification(
                i18nc("@info", "User '%1' deleted successfully", username))
        }
        function onErrorOccurred(message) {
            applicationWindow().showPassiveNotification(message, "long")
        }
    }

    Connections {
        target: helperClient ?? null
        function onErrorOccurred(message) {
            applicationWindow().showPassiveNotification(message, "long")
        }
    }

    ColumnLayout {
        spacing: Kirigami.Units.largeSpacing

        // Header section
        ColumnLayout {
            Layout.fillWidth: true
            spacing: Kirigami.Units.smallSpacing

            Kirigami.Heading {
                text: i18nc("@title", "CouchPlay Users")
                level: 2
            }

            Controls.Label {
                text: i18nc("@info", "These are dedicated gaming accounts created by CouchPlay. Each player runs their own Steam instance under a separate user account.")
                wrapMode: Text.WordWrap
                Layout.fillWidth: true
                opacity: 0.7
            }
        }

        // Info about current user not being shown
        Kirigami.InlineMessage {
            Layout.fillWidth: true
            text: i18nc("@info", "Your current desktop user (%1) is not listed here. CouchPlay only manages dedicated gaming accounts in the 'couchplay' group.", userManager?.currentUser ?? "")
            type: Kirigami.MessageType.Information
            visible: true
        }

        // Helper status message
        Kirigami.InlineMessage {
            Layout.fillWidth: true
            text: i18nc("@info", "The CouchPlay Helper service is not available. User creation and deletion is disabled. Please run: sudo ./scripts/install-helper.sh install")
            type: Kirigami.MessageType.Error
            visible: !(helperClient?.available ?? false)
        }

        // User count summary
        Kirigami.InlineMessage {
            Layout.fillWidth: true
            text: {
                const count = userManager?.users?.length ?? 0
                if (count === 0) {
                    return i18nc("@info", "No CouchPlay users found. Create gaming users to enable split-screen multiplayer.")
                } else if (count === 1) {
                    return i18nc("@info", "1 gaming user available. Create more users for split-screen sessions.")
                } else {
                    return i18nc("@info", "%1 gaming users available for split-screen sessions.", count)
                }
            }
            type: (userManager?.users?.length ?? 0) < 2 ? Kirigami.MessageType.Warning : Kirigami.MessageType.Positive
            visible: true
        }

        // User list
        Repeater {
            model: userManager?.users ?? []

            delegate: Kirigami.Card {
                Layout.fillWidth: true

                banner.title: modelData.username
                banner.titleIcon: "user"

                contentItem: ColumnLayout {
                    spacing: Kirigami.Units.smallSpacing

                    RowLayout {
                        spacing: Kirigami.Units.largeSpacing
                        Layout.fillWidth: true

                        // User details
                        GridLayout {
                            columns: 2
                            columnSpacing: Kirigami.Units.largeSpacing
                            rowSpacing: Kirigami.Units.smallSpacing
                            Layout.fillWidth: true

                            Controls.Label {
                                text: i18nc("@label", "UID:")
                                opacity: 0.7
                            }
                            Controls.Label {
                                text: modelData.uid
                                font.family: "monospace"
                            }

                            Controls.Label {
                                text: i18nc("@label", "Home:")
                                opacity: 0.7
                            }
                            Controls.Label {
                                text: modelData.homeDir
                                font.family: "monospace"
                                elide: Text.ElideMiddle
                                Layout.fillWidth: true
                            }
                        }

                        // Delete button
                        Controls.Button {
                            icon.name: "edit-delete"
                            text: i18nc("@action:button", "Delete")
                            enabled: helperClient?.available ?? false
                            onClicked: {
                                root.userToDelete = modelData.username
                                deleteHomeCheckbox.checked = false
                                deleteUserDialog.open()
                            }
                        }
                    }

                    // Info for gaming users
                    Controls.Label {
                        text: i18nc("@info", "This user can be assigned to any player slot in a gaming session.")
                        wrapMode: Text.WordWrap
                        opacity: 0.7
                        font.italic: true
                        Layout.fillWidth: true
                    }
                }
            }
        }

        // Empty state
        Kirigami.PlaceholderMessage {
            Layout.fillWidth: true
            visible: (userManager?.users?.length ?? 0) === 0
            icon.name: "user-group-new"
            text: i18nc("@info:placeholder", "No Gaming Users")
            explanation: i18nc("@info", "Create dedicated gaming users to enable split-screen multiplayer. Each user will have their own Steam installation and game saves.")

            helpfulAction: Kirigami.Action {
                icon.name: "list-add-user"
                text: i18nc("@action:button", "Create Gaming User")
                enabled: helperClient?.available ?? false
                onTriggered: {
                    usernameField.text = ""
                    validationMessage.text = ""
                    addUserDialog.open()
                }
            }
        }

        // Helper info section
        Kirigami.FormLayout {
            Layout.fillWidth: true
            Layout.topMargin: Kirigami.Units.largeSpacing

            Kirigami.Separator {
                Kirigami.FormData.isSection: true
                Kirigami.FormData.label: i18nc("@title", "About User Management")
            }
        }

        Kirigami.InlineMessage {
            Layout.fillWidth: true
            text: i18nc("@info", "Creating and deleting users requires the CouchPlay Helper service, which runs with elevated privileges to manage system users safely.")
            type: Kirigami.MessageType.Information
            visible: true

            actions: [
                Kirigami.Action {
                    icon.name: "help-about"
                    text: i18nc("@action:button", "Learn More")
                    onTriggered: Qt.openUrlExternally("https://github.com/hikaps/couchplay#helper-setup")
                }
            ]
        }

        // How it works
        Components.InfoCard {
            title: i18nc("@title", "How Multi-User Sessions Work")
            Layout.fillWidth: true

            RowLayout {
                spacing: Kirigami.Units.largeSpacing
                Kirigami.Icon {
                    source: "dialog-ok"
                    Layout.preferredWidth: Kirigami.Units.iconSizes.medium
                    Layout.preferredHeight: Kirigami.Units.iconSizes.medium
                    color: Kirigami.Theme.positiveTextColor
                }
                Controls.Label {
                    text: i18nc("@info", "Each player gets their own Steam instance with separate saves and settings")
                    wrapMode: Text.WordWrap
                    Layout.fillWidth: true
                }
            }

            RowLayout {
                spacing: Kirigami.Units.largeSpacing
                Kirigami.Icon {
                    source: "dialog-ok"
                    Layout.preferredWidth: Kirigami.Units.iconSizes.medium
                    Layout.preferredHeight: Kirigami.Units.iconSizes.medium
                    color: Kirigami.Theme.positiveTextColor
                }
                Controls.Label {
                    text: i18nc("@info", "Input devices are isolated per player using gamescope")
                    wrapMode: Text.WordWrap
                    Layout.fillWidth: true
                }
            }

            RowLayout {
                spacing: Kirigami.Units.largeSpacing
                Kirigami.Icon {
                    source: "dialog-ok"
                    Layout.preferredWidth: Kirigami.Units.iconSizes.medium
                    Layout.preferredHeight: Kirigami.Units.iconSizes.medium
                    color: Kirigami.Theme.positiveTextColor
                }
                Controls.Label {
                    text: i18nc("@info", "Audio is routed through PipeWire for multi-user support")
                    wrapMode: Text.WordWrap
                    Layout.fillWidth: true
                }
            }
        }
    }

    // Add User Dialog
    Kirigami.Dialog {
        id: addUserDialog
        title: i18nc("@title:dialog", "Create Gaming User")
        standardButtons: Kirigami.Dialog.NoButton
        preferredWidth: Kirigami.Units.gridUnit * 20

        customFooterActions: [
            Kirigami.Action {
                text: i18nc("@action:button", "Cancel")
                icon.name: "dialog-cancel"
                onTriggered: addUserDialog.close()
            },
            Kirigami.Action {
                text: i18nc("@action:button", "Create User")
                icon.name: "list-add-user"
                enabled: usernameField.text.length > 0 && userManager.isValidUsername(usernameField.text) && !userManager.userExists(usernameField.text) && (helperClient?.available ?? false)
                onTriggered: {
                    if (userManager.createUser(usernameField.text)) {
                        addUserDialog.close()
                    }
                }
            }
        ]

        ColumnLayout {
            spacing: Kirigami.Units.largeSpacing

            Controls.Label {
                text: i18nc("@info", "Create a new Linux user for split-screen gaming. The user will be added to the 'couchplay' group for management.")
                wrapMode: Text.WordWrap
                Layout.fillWidth: true
            }

            Kirigami.FormLayout {
                Controls.TextField {
                    id: usernameField
                    Kirigami.FormData.label: i18nc("@label", "Username:")
                    placeholderText: i18nc("@info:placeholder", "player2")
                    validator: RegularExpressionValidator {
                        regularExpression: /^[a-z_][a-z0-9_-]*$/
                    }
                    inputMethodHints: Qt.ImhLowercaseOnly | Qt.ImhNoAutoUppercase

                    onTextChanged: {
                        if (text.length === 0) {
                            validationMessage.text = ""
                        } else if (!userManager.isValidUsername(text)) {
                            validationMessage.text = i18nc("@info:status", "Invalid username. Use lowercase letters, numbers, underscores, and hyphens only.")
                            validationMessage.type = Kirigami.MessageType.Error
                        } else if (userManager.userExists(text)) {
                            validationMessage.text = i18nc("@info:status", "A user with this name already exists.")
                            validationMessage.type = Kirigami.MessageType.Error
                        } else {
                            validationMessage.text = i18nc("@info:status", "Username is available.")
                            validationMessage.type = Kirigami.MessageType.Positive
                        }
                    }
                }
            }

            Kirigami.InlineMessage {
                id: validationMessage
                Layout.fillWidth: true
                visible: text.length > 0
            }

            Kirigami.InlineMessage {
                Layout.fillWidth: true
                text: i18nc("@info", "Suggested usernames: player2, player3, player4, couch2, gamer2")
                type: Kirigami.MessageType.Information
                visible: usernameField.text.length === 0
            }
        }
    }

    // Delete User Confirmation Dialog
    Kirigami.Dialog {
        id: deleteUserDialog
        title: i18nc("@title:dialog", "Delete User")
        standardButtons: Kirigami.Dialog.NoButton
        preferredWidth: Kirigami.Units.gridUnit * 22

        customFooterActions: [
            Kirigami.Action {
                text: i18nc("@action:button", "Cancel")
                icon.name: "dialog-cancel"
                onTriggered: deleteUserDialog.close()
            },
            Kirigami.Action {
                text: i18nc("@action:button", "Delete User")
                icon.name: "edit-delete"
                onTriggered: {
                    if (userManager.deleteUser(root.userToDelete, deleteHomeCheckbox.checked)) {
                        deleteUserDialog.close()
                    }
                }
            }
        ]

        ColumnLayout {
            spacing: Kirigami.Units.largeSpacing

            Kirigami.Icon {
                source: "dialog-warning"
                Layout.preferredWidth: Kirigami.Units.iconSizes.huge
                Layout.preferredHeight: Kirigami.Units.iconSizes.huge
                Layout.alignment: Qt.AlignHCenter
            }

            Controls.Label {
                text: i18nc("@info", "Are you sure you want to delete the user '%1'?", root.userToDelete)
                wrapMode: Text.WordWrap
                Layout.fillWidth: true
                horizontalAlignment: Text.AlignHCenter
                font.bold: true
            }

            Controls.Label {
                text: i18nc("@info", "This will remove the user account from the system. Any profiles using this user will need to be updated.")
                wrapMode: Text.WordWrap
                Layout.fillWidth: true
                opacity: 0.8
            }

            Kirigami.Separator {
                Layout.fillWidth: true
            }

            Controls.CheckBox {
                id: deleteHomeCheckbox
                text: i18nc("@option:check", "Also delete home directory and all user data")
                Layout.fillWidth: true
            }

            Kirigami.InlineMessage {
                Layout.fillWidth: true
                text: i18nc("@info:warning", "Warning: This will permanently delete all files in the user's home directory, including Steam games, saves, and configuration. This cannot be undone!")
                type: Kirigami.MessageType.Warning
                visible: deleteHomeCheckbox.checked
            }
        }
    }
}
