// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2024 hikaps

import QtQuick
import QtQuick.Layouts
import QtQuick.Controls as Controls
import org.kde.kirigami as Kirigami

Kirigami.ScrollablePage {
    id: root
    title: i18nc("@title", "Games")

    required property var gameLibrary
    property var steamGames: []

    actions: [
        Kirigami.Action {
            icon.name: "view-refresh"
            text: i18nc("@action:button", "Refresh")
            onTriggered: gameLibrary?.refresh()
        },
        Kirigami.Action {
            icon.name: "steam"
            text: i18nc("@action:button", "Import from Steam")
            onTriggered: {
                if (!gameLibrary) return
                steamGames = gameLibrary.getSteamGames()
                if (steamGames.length === 0) {
                    applicationWindow().showPassiveNotification(
                        i18nc("@info", "No Steam games found. Make sure Steam is installed."))
                } else {
                    importSteamDialog.open()
                }
            }
        },
        Kirigami.Action {
            icon.name: "list-add"
            text: i18nc("@action:button", "Add Custom Game")
            onTriggered: {
                gameNameField.text = ""
                gameCommandField.text = ""
                gameIconField.text = ""
                addGameDialog.open()
            }
        }
    ]

    Connections {
        target: gameLibrary ?? null
        function onErrorOccurred(message) {
            applicationWindow().showPassiveNotification(message, "long")
        }
        function onGamesChanged() {
            // Force refresh of any bindings
        }
    }

    ColumnLayout {
        spacing: Kirigami.Units.largeSpacing

        // Header
        ColumnLayout {
            Layout.fillWidth: true
            spacing: Kirigami.Units.smallSpacing

            Kirigami.Heading {
                text: i18nc("@title", "Game Library")
                level: 2
            }

            Controls.Label {
                text: i18nc("@info", "Manage games for quick launching in split-screen sessions. Import from Steam or add custom games.")
                wrapMode: Text.WordWrap
                Layout.fillWidth: true
                opacity: 0.7
            }
        }

        // Game count
        Kirigami.InlineMessage {
            Layout.fillWidth: true
            text: {
                const count = gameLibrary?.games?.length ?? 0
                if (count === 0) {
                    return i18nc("@info", "No games in library. Import from Steam or add games manually.")
                } else if (count === 1) {
                    return i18nc("@info", "1 game in library")
                } else {
                    return i18nc("@info", "%1 games in library", count)
                }
            }
            type: (gameLibrary?.games?.length ?? 0) === 0 ? Kirigami.MessageType.Information : Kirigami.MessageType.Positive
            visible: true
        }

        // Games grid
        GridLayout {
            Layout.fillWidth: true
            columns: root.width > Kirigami.Units.gridUnit * 50 ? 3 : (root.width > Kirigami.Units.gridUnit * 30 ? 2 : 1)
            columnSpacing: Kirigami.Units.largeSpacing
            rowSpacing: Kirigami.Units.largeSpacing
            visible: (gameLibrary?.games?.length ?? 0) > 0

            Repeater {
                model: gameLibrary?.games ?? []

                delegate: Kirigami.Card {
                    Layout.fillWidth: true

                    banner.title: modelData.name
                    banner.titleIcon: modelData.iconPath || "applications-games"

                    contentItem: ColumnLayout {
                        spacing: Kirigami.Units.smallSpacing

                        RowLayout {
                            spacing: Kirigami.Units.smallSpacing
                            Layout.fillWidth: true

                            // Command display
                            Controls.Label {
                                text: modelData.command
                                font.family: "monospace"
                                font.pointSize: Kirigami.Theme.smallFont.pointSize
                                elide: Text.ElideMiddle
                                opacity: 0.7
                                Layout.fillWidth: true
                            }

                            // Source chip
                            Kirigami.Chip {
                                text: modelData.command.startsWith("steam://") ? "Steam" : i18nc("@info", "Custom")
                                icon.name: modelData.command.startsWith("steam://") ? "applications-games" : "application-x-executable"
                                closable: false
                                checkable: false
                            }
                        }
                    }

                    actions: [
                        Kirigami.Action {
                            icon.name: "edit-copy"
                            text: i18nc("@action:button", "Copy Command")
                            onTriggered: {
                                clipboard.text = modelData.command
                                applicationWindow().showPassiveNotification(
                                    i18nc("@info", "Command copied to clipboard"))
                            }
                        },
                        Kirigami.Action {
                            icon.name: "edit-delete"
                            text: i18nc("@action:button", "Remove")
                            onTriggered: {
                                deleteGameName = modelData.name
                                deleteConfirmDialog.open()
                            }
                        }
                    ]
                }
            }
        }

        // Empty state
        Kirigami.PlaceholderMessage {
            Layout.fillWidth: true
            Layout.fillHeight: true
            visible: (gameLibrary?.games?.length ?? 0) === 0
            icon.name: "applications-games"
            text: i18nc("@info:placeholder", "No Games in Library")
            explanation: i18nc("@info", "Add games to quickly launch them in split-screen sessions.")

            helpfulAction: Kirigami.Action {
                icon.name: "steam"
                text: i18nc("@action:button", "Import from Steam")
                onTriggered: {
                    if (!gameLibrary) return
                    steamGames = gameLibrary.getSteamGames()
                    if (steamGames.length === 0) {
                        applicationWindow().showPassiveNotification(
                            i18nc("@info", "No Steam games found."))
                    } else {
                        importSteamDialog.open()
                    }
                }
            }
        }

        // Tips section
        Kirigami.Card {
            Layout.fillWidth: true
            visible: (gameLibrary?.games?.length ?? 0) > 0

            header: Kirigami.Heading {
                text: i18nc("@title", "Tips")
                level: 3
                padding: Kirigami.Units.largeSpacing
            }

            contentItem: ColumnLayout {
                spacing: Kirigami.Units.smallSpacing

                Controls.Label {
                    text: i18nc("@info", "Games added here can be selected when creating split-screen session profiles.")
                    wrapMode: Text.WordWrap
                    Layout.fillWidth: true
                }

                Controls.Label {
                    text: i18nc("@info", "Steam games use steam:// URLs which launch through Steam's runtime.")
                    wrapMode: Text.WordWrap
                    Layout.fillWidth: true
                    opacity: 0.7
                }
            }
        }

        // Spacer
        Item {
            Layout.fillHeight: true
        }
    }

    // Clipboard helper
    Controls.TextArea {
        id: clipboard
        visible: false
        onTextChanged: {
            if (text.length > 0) {
                selectAll()
                copy()
                text = ""
            }
        }
    }

    // Delete confirmation
    property string deleteGameName: ""

    Kirigami.PromptDialog {
        id: deleteConfirmDialog
        title: i18nc("@title:dialog", "Remove Game")
        subtitle: i18nc("@info", "Remove '%1' from the library?", deleteGameName)
        standardButtons: Kirigami.Dialog.Yes | Kirigami.Dialog.Cancel
        
        onAccepted: {
            gameLibrary?.removeGame(deleteGameName)
            applicationWindow().showPassiveNotification(
                i18nc("@info", "Game removed from library"))
        }
    }

    // Add Custom Game Dialog
    Kirigami.Dialog {
        id: addGameDialog
        title: i18nc("@title:dialog", "Add Custom Game")
        standardButtons: Kirigami.Dialog.NoButton
        preferredWidth: Kirigami.Units.gridUnit * 25

        customFooterActions: [
            Kirigami.Action {
                text: i18nc("@action:button", "Cancel")
                icon.name: "dialog-cancel"
                onTriggered: addGameDialog.close()
            },
            Kirigami.Action {
                text: i18nc("@action:button", "Add Game")
                icon.name: "list-add"
                enabled: gameNameField.text.length > 0 && gameCommandField.text.length > 0
                onTriggered: {
                    if (gameLibrary?.addGame(gameNameField.text, gameCommandField.text, gameIconField.text)) {
                        addGameDialog.close()
                        applicationWindow().showPassiveNotification(
                            i18nc("@info", "Game '%1' added to library", gameNameField.text))
                    }
                }
            }
        ]

        ColumnLayout {
            spacing: Kirigami.Units.largeSpacing

            Controls.Label {
                text: i18nc("@info", "Add a custom game or application to launch in split-screen sessions.")
                wrapMode: Text.WordWrap
                Layout.fillWidth: true
            }

            Kirigami.FormLayout {
                Controls.TextField {
                    id: gameNameField
                    Kirigami.FormData.label: i18nc("@label", "Name:")
                    placeholderText: i18nc("@info:placeholder", "My Game")
                }

                Controls.TextField {
                    id: gameCommandField
                    Kirigami.FormData.label: i18nc("@label", "Command:")
                    placeholderText: i18nc("@info:placeholder", "steam://rungameid/123456 or /path/to/game")
                }

                Controls.TextField {
                    id: gameIconField
                    Kirigami.FormData.label: i18nc("@label", "Icon (optional):")
                    placeholderText: i18nc("@info:placeholder", "/path/to/icon.png")
                }
            }

            Kirigami.InlineMessage {
                Layout.fillWidth: true
                text: i18nc("@info", "For Steam games, use: steam://rungameid/APPID where APPID is the game's Steam ID.")
                type: Kirigami.MessageType.Information
                visible: true
            }
        }
    }

    // Import Steam Games Dialog
    Kirigami.Dialog {
        id: importSteamDialog
        title: i18nc("@title:dialog", "Import Steam Games")
        standardButtons: Kirigami.Dialog.NoButton
        preferredWidth: Kirigami.Units.gridUnit * 30
        preferredHeight: Kirigami.Units.gridUnit * 25

        property var selectedGames: ({})

        customFooterActions: [
            Kirigami.Action {
                text: i18nc("@action:button", "Cancel")
                icon.name: "dialog-cancel"
                onTriggered: importSteamDialog.close()
            },
            Kirigami.Action {
                text: i18nc("@action:button", "Select All")
                icon.name: "edit-select-all"
                onTriggered: {
                    let newSelected = {}
                    for (let i = 0; i < steamGames.length; i++) {
                        newSelected[steamGames[i].appId] = true
                    }
                    importSteamDialog.selectedGames = newSelected
                }
            },
            Kirigami.Action {
                text: {
                    const count = Object.keys(importSteamDialog.selectedGames).filter(k => importSteamDialog.selectedGames[k]).length
                    return i18nc("@action:button", "Import %1 Games", count)
                }
                icon.name: "document-import"
                enabled: Object.keys(importSteamDialog.selectedGames).filter(k => importSteamDialog.selectedGames[k]).length > 0
                onTriggered: {
                    let imported = 0
                    for (let i = 0; i < steamGames.length; i++) {
                        const game = steamGames[i]
                        if (importSteamDialog.selectedGames[game.appId]) {
                            if (gameLibrary?.addGame(game.name, game.command, "")) {
                                imported++
                            }
                        }
                    }
                    importSteamDialog.close()
                    importSteamDialog.selectedGames = {}
                    applicationWindow().showPassiveNotification(
                        i18nc("@info", "Imported %1 games from Steam", imported))
                }
            }
        ]

        ColumnLayout {
            spacing: Kirigami.Units.largeSpacing

            Controls.Label {
                text: i18nc("@info", "Found %1 Steam games. Select games to import:", steamGames.length)
                wrapMode: Text.WordWrap
                Layout.fillWidth: true
            }

            // Search filter
            Kirigami.SearchField {
                id: steamSearchField
                Layout.fillWidth: true
                placeholderText: i18nc("@info:placeholder", "Filter games...")
            }

            // Games list
            Controls.ScrollView {
                Layout.fillWidth: true
                Layout.fillHeight: true
                Layout.preferredHeight: Kirigami.Units.gridUnit * 15

                ListView {
                    id: steamGamesList
                    model: steamGames.filter(game => 
                        steamSearchField.text.length === 0 || 
                        game.name.toLowerCase().includes(steamSearchField.text.toLowerCase())
                    )
                    clip: true

                    delegate: Controls.ItemDelegate {
                        width: steamGamesList.width
                        
                        contentItem: RowLayout {
                            spacing: Kirigami.Units.smallSpacing

                            Controls.CheckBox {
                                id: gameCheckbox
                                checked: importSteamDialog.selectedGames[modelData.appId] || false
                                onToggled: {
                                    let newSelected = Object.assign({}, importSteamDialog.selectedGames)
                                    newSelected[modelData.appId] = checked
                                    importSteamDialog.selectedGames = newSelected
                                }
                            }

                            Kirigami.Icon {
                                source: "applications-games"
                                Layout.preferredWidth: Kirigami.Units.iconSizes.small
                                Layout.preferredHeight: Kirigami.Units.iconSizes.small
                            }

                            ColumnLayout {
                                spacing: 0
                                Layout.fillWidth: true

                                Controls.Label {
                                    text: modelData.name
                                    elide: Text.ElideRight
                                    Layout.fillWidth: true
                                }

                                Controls.Label {
                                    text: i18nc("@info", "App ID: %1", modelData.appId)
                                    font.pointSize: Kirigami.Theme.smallFont.pointSize
                                    opacity: 0.7
                                }
                            }
                        }

                        onClicked: {
                            let newSelected = Object.assign({}, importSteamDialog.selectedGames)
                            newSelected[modelData.appId] = !newSelected[modelData.appId]
                            importSteamDialog.selectedGames = newSelected
                        }
                    }
                }
            }

            Kirigami.InlineMessage {
                Layout.fillWidth: true
                visible: steamGames.length > 0 && steamGamesList.count === 0
                text: i18nc("@info", "No games match your search.")
                type: Kirigami.MessageType.Information
            }
        }

        onClosed: {
            selectedGames = {}
            steamSearchField.text = ""
        }
    }
}
