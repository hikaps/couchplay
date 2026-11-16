// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2024 hikaps

import QtQuick
import QtQuick.Layouts
import QtQuick.Controls as Controls
import org.kde.kirigami as Kirigami

import "../components" as Components

Kirigami.ScrollablePage {
    id: root
    title: i18nc("@title", "Players")

    required property var playerStore
    required property var pairingSession
    required property var sessionRunner

    // Player being deleted
    property string playerToDelete: ""

    readonly property bool nameValid: playerNameField.text.length > 0
        && playerStore.isValidName(playerNameField.text)
        && !playerStore.exists(playerNameField.text)

    function addPlayer() {
        if (nameValid && playerStore.createPlayer(playerNameField.text)) {
            playerNameField.text = ""
        }
    }

    actions: [
        Kirigami.Action {
            objectName: "actionRefresh"
            Accessible.role: Accessible.Button
            Accessible.name: text
            Accessible.onPressAction: triggered()
            icon.name: "view-refresh"
            text: i18nc("@action:button", "Refresh")
            onTriggered: playerStore?.refresh()
        }
    ]

    Connections {
        target: playerStore ?? null
        function onPlayerCreated(name) {
            applicationWindow().showPassiveNotification(
                i18nc("@info", "Player '%1' created successfully", name))
        }
        function onPlayerDeleted(name) {
            applicationWindow().showPassiveNotification(
                i18nc("@info", "Player '%1' deleted successfully", name))
        }
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
                text: i18nc("@title", "CouchPlay Players")
                level: 2
            }

            Controls.Label {
                text: i18nc("@info", "Each player has a private home folder and runs their own Steam instance inside a sandbox, as your desktop user.")
                wrapMode: Text.WordWrap
                Layout.fillWidth: true
                opacity: 0.7
            }
        }

        // Add player
        RowLayout {
            Layout.fillWidth: true
            spacing: Kirigami.Units.smallSpacing

            Controls.TextField {
                id: playerNameField
                objectName: "fieldPlayerName"
                Accessible.role: Accessible.EditableText
                Accessible.name: i18nc("@label", "Player name")
                placeholderText: i18nc("@info:placeholder", "player2")
                inputMethodHints: Qt.ImhLowercaseOnly | Qt.ImhNoAutoUppercase
                Layout.fillWidth: true
                onAccepted: root.addPlayer()
            }

            Controls.Button {
                objectName: "btnAddPlayer"
                Accessible.role: Accessible.Button
                Accessible.name: text
                icon.name: "list-add"
                text: i18nc("@action:button", "Add Player")
                enabled: root.nameValid
                onClicked: root.addPlayer()
            }
        }

        Kirigami.InlineMessage {
            objectName: "messagePlayerName"
            Layout.fillWidth: true
            visible: playerNameField.text.length > 0
            type: root.nameValid ? Kirigami.MessageType.Positive : Kirigami.MessageType.Error
            text: {
                if (!playerStore.isValidName(playerNameField.text))
                    return i18nc("@info:status", "Invalid name. Use lowercase letters, numbers, underscores and hyphens, starting with a letter or underscore (up to 32 characters).")
                if (playerStore.exists(playerNameField.text))
                    return i18nc("@info:status", "A player with this name already exists.")
                return i18nc("@info:status", "Name is available.")
            }
        }

        Kirigami.InlineMessage {
            Layout.fillWidth: true
            text: i18nc("@info", "%1 player(s) available.", playerStore?.players?.length ?? 0)
            type: (playerStore?.players?.length ?? 0) < 2 ? Kirigami.MessageType.Warning : Kirigami.MessageType.Positive
            visible: true
        }

        // Player list
        Repeater {
            model: playerStore?.players ?? []

            delegate: Kirigami.AbstractCard {
                Layout.fillWidth: true
                id: userCard
                objectName: "cardGamingUser"

                readonly property string user: modelData
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
                    text: userCard.user
                    level: 3
                    padding: Kirigami.Units.smallSpacing
                }

                contentItem: ColumnLayout {
                    spacing: Kirigami.Units.smallSpacing

                    RowLayout {
                        spacing: Kirigami.Units.largeSpacing
                        Layout.fillWidth: true

                        // Player details
                        Controls.Label {
                            text: i18nc("@info", "This player can be assigned to any player slot in a gaming session.")
                            wrapMode: Text.WordWrap
                            opacity: 0.7
                            font.italic: true
                            Layout.fillWidth: true
                        }

                        // Delete button
                        Controls.Button {
                            objectName: "btnDeletePlayer"
                            Accessible.role: Accessible.Button
                            Accessible.name: text
                            Accessible.onPressAction: clicked()
                            icon.name: "edit-delete"
                            text: i18nc("@action:button", "Delete")
                            enabled: !userCard.userBusy
                            Controls.ToolTip.visible: hovered && userCard.userBusy
                            Controls.ToolTip.text: i18nc("@info:tooltip", "Stop this player's pairing or stream before deleting the player")
                            onClicked: {
                                root.playerToDelete = userCard.user
                                deletePlayerDialog.open()
                            }
                        }
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
            objectName: "placeholderNoPlayers"
            Layout.fillWidth: true
            visible: (playerStore?.players?.length ?? 0) === 0
            icon.name: "user-group-new"
            text: i18nc("@info:placeholder", "No Players")
            explanation: i18nc("@info", "Add players to enable split-screen multiplayer. Each player gets their own Steam installation and game saves.")
        }

        // How it works
        Components.InfoCard {
            title: i18nc("@title", "How Player Sessions Work")
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
                    text: i18nc("@info", "Each player runs in a private sandbox with its own controller")
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
                    text: i18nc("@info", "Audio is routed through PipeWire for multi-player support")
                    wrapMode: Text.WordWrap
                    Layout.fillWidth: true
                }
            }
        }
    }

    // Delete Player Confirmation Dialog
    Kirigami.Dialog {
        id: deletePlayerDialog
        objectName: "dialogDeletePlayer"
        title: i18nc("@title:dialog", "Delete Player")
        standardButtons: Kirigami.Dialog.NoButton
        preferredWidth: Kirigami.Units.gridUnit * 22

        customFooterActions: [
            Kirigami.Action {
                text: i18nc("@action:button", "Cancel")
                icon.name: "dialog-cancel"
                onTriggered: deletePlayerDialog.close()
            },
            Kirigami.Action {
                text: i18nc("@action:button", "Delete Player")
                icon.name: "edit-delete"
                onTriggered: {
                    if (playerStore.deletePlayer(root.playerToDelete)) {
                        deletePlayerDialog.close()
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
                text: i18nc("@info", "Are you sure you want to delete the player '%1'?", root.playerToDelete)
                wrapMode: Text.WordWrap
                Layout.fillWidth: true
                horizontalAlignment: Text.AlignHCenter
                font.bold: true
            }

            Controls.Label {
                text: i18nc("@info", "Deletes this player's saves, logins and Moonlight pairing")
                wrapMode: Text.WordWrap
                Layout.fillWidth: true
                opacity: 0.8
            }

            Controls.Label {
                text: i18nc("@info", "Profiles using this player will need to be updated. This cannot be undone.")
                wrapMode: Text.WordWrap
                Layout.fillWidth: true
                opacity: 0.8
            }
        }
    }
}
