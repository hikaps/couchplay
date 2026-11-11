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
    required property var pairingSession
    required property var sessionRunner

    // Track which user is being deleted
    property string userToDelete: ""

    actions: [
        Kirigami.Action {
            objectName: "actionRefresh"
            Accessible.role: Accessible.Button
            Accessible.name: text
            Accessible.onPressAction: triggered()
            icon.name: "view-refresh"
            text: i18nc("@action:button", "Refresh")
            onTriggered: userManager?.refresh()
        },
        Kirigami.Action {
            objectName: "actionAddUser"
            Accessible.role: Accessible.Button
            Accessible.name: text
            Accessible.onPressAction: triggered()
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

        // Consolidated info: user count + desktop user
        Kirigami.InlineMessage {
            Layout.fillWidth: true
            text: i18nc("@info", "%1 gaming user(s) available. Your desktop user (%2) is managed separately.", userManager?.users?.length ?? 0, userManager?.currentUser ?? "")
            type: (userManager?.users?.length ?? 0) < 2 ? Kirigami.MessageType.Warning : Kirigami.MessageType.Positive
            visible: true
        }

        // Helper status message
        Kirigami.InlineMessage {
            Layout.fillWidth: true
            text: i18nc("@info", "The CouchPlay Helper service is not available. User creation and deletion is disabled. Please run: sudo ./scripts/install-helper.sh install")
            type: Kirigami.MessageType.Error
            visible: !(helperClient?.available ?? false)
        }

        // User list
        Repeater {
            model: userManager?.users ?? []

            delegate: Kirigami.AbstractCard {
                Layout.fillWidth: true
                id: userCard
                objectName: "cardGamingUser"

                readonly property string user: modelData.username
                readonly property string textStartStreamFirst: i18nc("@info", "Start this player's stream first, or stop the session")

                // Bumped when this user's paired clients may have changed
                property int clientsRevision: 0
                property bool unpairing: false
                // The pairing was requested from this card (a pairing session for this user is not necessarily ours)
                property bool pairRequested: false
                property bool sheetLive: false
                property int sheetInstance: 0
                property string actionError: ""

                readonly property bool sessionActive: root.sessionRunner.active
                // Dependency only: re-evaluates the bindings below when streams start, stop or change state
                readonly property var liveStreams: root.sessionRunner.streamManager.streams
                // Instance index of this user's player in the running session; -1 when there is none
                readonly property int liveInstance: {
                    userCard.liveStreams
                    return userCard.sessionActive ? root.sessionRunner.streamingInstanceForUser(userCard.user) : -1
                }
                // StreamManager.StreamState: 2 = Streaming
                readonly property bool liveReady: {
                    if (userCard.liveInstance < 0) return false
                    for (let i = 0; i < userCard.liveStreams.length; ++i) {
                        if (userCard.liveStreams[i].instanceIndex === userCard.liveInstance) {
                            return userCard.liveStreams[i].state === 2
                        }
                    }
                    return false
                }
                // PairingSession.State: 0 = Idle, 1 = Starting, 2 = Ready, 3 = Failed
                readonly property bool pairingMine: root.pairingSession.username === userCard.user
                readonly property bool canAct: !userCard.unpairing && (userCard.sessionActive ? userCard.liveReady : !root.pairingSession.busy)
                readonly property bool userBusy: userCard.unpairing
                    || (root.pairingSession.busy && userCard.pairingMine)
                    || userCard.liveInstance >= 0
                readonly property string port: root.pairingSession.hostPort(userCard.user)
                readonly property string hostAddress: root.pairingSession.streamManager.hostAddress
                readonly property var pairedClients: {
                    userCard.clientsRevision
                    userCard.liveStreams
                    return root.pairingSession.pairedClients(userCard.user)
                }

                function startPairing() {
                    actionError = ""
                    if (sessionActive) {
                        sheetLive = true
                        sheetInstance = liveInstance
                        pairingSheet.open()
                        return
                    }
                    sheetLive = false
                    sheetInstance = 0
                    pairRequested = true
                    root.pairingSession.start(user)
                }

                Connections {
                    target: root.pairingSession

                    function onPairedChanged(username) {
                        if (username === userCard.user) {
                            userCard.clientsRevision++
                        }
                    }

                    function onStateChanged() {
                        if (!userCard.pairRequested || userCard.sheetLive) return
                        const state = root.pairingSession.state
                        if (state === 2 && userCard.pairingMine) {
                            pairingSheet.open()
                        } else if (state === 0) {
                            userCard.pairRequested = false
                            if (pairingSheet.sheetOpen) {
                                pairingSheet.close()
                            }
                        }
                    }

                    function onUnpairFinished(username, ok, message) {
                        if (username !== userCard.user) return
                        userCard.unpairing = false
                        userCard.actionError = ok ? "" : message
                        userCard.clientsRevision++
                    }
                }

                header: Kirigami.Heading {
                    text: modelData.username
                    level: 3
                    padding: Kirigami.Units.smallSpacing
                }

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
                            objectName: "btnDeleteUser"
                            Accessible.role: Accessible.Button
                            Accessible.name: text
                            Accessible.onPressAction: clicked()
                            icon.name: "edit-delete"
                            text: i18nc("@action:button", "Delete")
                            enabled: (helperClient?.available ?? false) && !userCard.userBusy
                            Controls.ToolTip.visible: hovered && userCard.userBusy
                            Controls.ToolTip.text: i18nc("@info:tooltip", "Stop this player's pairing or stream before deleting the user")
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

                    // Moonlight pairing: the user's Sunshine identity lives on this page only
                    Kirigami.Separator {
                        Layout.fillWidth: true
                    }

                    Controls.Label {
                        objectName: "labelMoonlightAddress"
                        text: userCard.hostAddress !== ""
                            ? i18nc("@info", "Add in Moonlight: %1:%2", userCard.hostAddress, userCard.port)
                            : i18nc("@info", "Moonlight port: %1", userCard.port)
                        font.family: "monospace"
                        wrapMode: Text.WordWrap
                        Layout.fillWidth: true
                    }

                    Controls.Label {
                        objectName: "labelPairedClients"
                        visible: userCard.pairedClients.length === 0
                        text: i18nc("@info:status", "No Moonlight clients paired")
                        opacity: 0.7
                        Layout.fillWidth: true
                    }

                    Repeater {
                        objectName: "repeaterPairedClients"
                        model: userCard.pairedClients

                        delegate: RowLayout {
                            id: clientRow

                            required property var modelData

                            Layout.fillWidth: true
                            spacing: Kirigami.Units.smallSpacing

                            Controls.Label {
                                text: clientRow.modelData.name
                                elide: Text.ElideRight
                                Layout.fillWidth: true
                            }

                            Controls.Button {
                                objectName: "buttonUnpairClient"
                                Accessible.role: Accessible.Button
                                Accessible.name: text
                                text: i18nc("@action:button", "Unpair")
                                icon.name: "list-remove"
                                enabled: userCard.canAct
                                onClicked: {
                                    userCard.actionError = ""
                                    userCard.unpairing = true
                                    root.pairingSession.unpairClient(userCard.user, clientRow.modelData.uuid)
                                }
                            }
                        }
                    }

                    RowLayout {
                        spacing: Kirigami.Units.smallSpacing
                        Layout.fillWidth: true

                        Controls.Button {
                            objectName: "buttonPairMoonlightClient"
                            Accessible.role: Accessible.Button
                            Accessible.name: text
                            text: i18nc("@action:button", "Pair Moonlight client…")
                            icon.name: "network-connect"
                            enabled: userCard.canAct
                            onClicked: userCard.startPairing()
                        }

                        Controls.BusyIndicator {
                            objectName: "indicatorPairingStarting"
                            running: visible
                            visible: (userCard.pairRequested && userCard.pairingMine && root.pairingSession.state === 1)
                                || userCard.unpairing
                            Layout.preferredWidth: Kirigami.Units.iconSizes.small
                            Layout.preferredHeight: Kirigami.Units.iconSizes.small
                        }

                        Controls.Label {
                            objectName: "labelPairingStarting"
                            visible: userCard.pairRequested && userCard.pairingMine && root.pairingSession.state === 1
                            text: i18nc("@info:status", "Starting pairing…")
                        }

                        Controls.Label {
                            objectName: "labelPairingUnavailable"
                            visible: userCard.sessionActive && !userCard.liveReady
                            text: userCard.textStartStreamFirst
                            wrapMode: Text.WordWrap
                            opacity: 0.7
                            Layout.fillWidth: true
                        }
                    }

                    Kirigami.InlineMessage {
                        objectName: "messagePairingError"
                        Layout.fillWidth: true
                        visible: userCard.pairRequested && userCard.pairingMine && root.pairingSession.state === 3
                        type: Kirigami.MessageType.Error
                        text: root.pairingSession.error
                    }

                    Kirigami.InlineMessage {
                        objectName: "messageUnpairError"
                        Layout.fillWidth: true
                        visible: userCard.actionError !== ""
                        type: Kirigami.MessageType.Error
                        text: userCard.actionError
                    }

                    Components.MoonlightPairingSheet {
                        id: pairingSheet
                        objectName: "pairingSheet"
                        streamManager: userCard.sheetLive ? root.sessionRunner.streamManager : root.pairingSession.streamManager
                        instanceIndex: userCard.sheetInstance
                        onClosed: {
                            // The temporary Sunshine only exists for the sheet; a running session's stream stays
                            if (!userCard.sheetLive && userCard.pairRequested && root.pairingSession.state !== 0) {
                                root.pairingSession.stop()
                            }
                            userCard.pairRequested = false
                            userCard.clientsRevision++
                        }
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
                objectName: "actionCreateGamingUser"
                Accessible.role: Accessible.Button
                Accessible.name: text
                Accessible.onPressAction: triggered()
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
                    objectName: "actionLearnMore"
                    Accessible.role: Accessible.Button
                    Accessible.name: text
                    Accessible.onPressAction: triggered()
                    icon.name: "help-about"
                    text: i18nc("@action:button", "Learn More")
                    onTriggered: Qt.openUrlExternally("https://github.com/hikaps/couchplay#2-install-the-helper")
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
        objectName: "dialogAddUser"
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
                    objectName: "fieldUsername"
                    Accessible.role: Accessible.EditableText
                    Accessible.name: i18nc("@label", "Username")
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
        }
    }

    // Delete User Confirmation Dialog
    Kirigami.Dialog {
        id: deleteUserDialog
        objectName: "dialogDeleteUser"
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
                objectName: "checkDeleteHome"
                Accessible.role: Accessible.CheckBox
                Accessible.name: text
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
