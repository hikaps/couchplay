// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 CouchPlay Contributors

import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

Controls.ComboBox {
    id: root

    required property var presetManager
    required property var sessionManager
    required property var steamConfigManager
    required property var heroicConfigManager

    Connections {
        target: root.steamConfigManager
        function onGamesLoaded() { root.catalogRevision++ }
    }

    Connections {
        target: root.heroicConfigManager
        function onGamesLoaded() { root.catalogRevision++ }
    }
    required property int instanceIndex
    property int catalogRevision: 0

    signal gameSelected(var selection)

    objectName: "comboGame"
    Accessible.role: Accessible.ComboBox
    Accessible.name: i18nc("@label", "Game:")
    Kirigami.FormData.label: i18nc("@label", "Game:")
    textRole: "title"
    valueRole: "gameId"
    Layout.fillWidth: true

    function selectedGame() {
        void(catalogRevision)
        if (!sessionManager) {
            return {}
        }
        const config = sessionManager.getInstanceConfig(instanceIndex)
        return config && config.gameSelection ? config.gameSelection : {}
    }

    function availableGames() {
        void(catalogRevision)
        const result = [{
            launcherId: "",
            backend: "",
            gameId: "",
            title: i18nc("@item", "Open launcher"),
            unavailable: false
        }]
        if (!presetManager || !sessionManager) {
            return result
        }

        const config = sessionManager.getInstanceConfig(instanceIndex)
        const presetId = config && config.presetId ? config.presetId : "steam"
        const games = presetManager.gamesForPreset(presetId) || []
        for (const game of games) {
            result.push(game)
        }

        const selected = selectedGame()
        if (selected.gameId && !result.some(game => game.gameId === selected.gameId
                                                   && game.launcherId === selected.launcherId)) {
            result.push({
                launcherId: selected.launcherId || "",
                backend: selected.backend || "",
                gameId: selected.gameId,
                title: selected.title
                    ? i18nc("@item", "%1 (unavailable)", selected.title)
                    : i18nc("@item", "Unavailable game"),
                unavailable: true
            })
        }
        return result
    }

    model: availableGames()

    currentIndex: {
        const selected = selectedGame()
        for (let i = 0; i < count; ++i) {
            const item = model[i]
            if ((selected.gameId || "") === (item.gameId || "")
                && (selected.launcherId || "") === (item.launcherId || "")) {
                return i
            }
        }
        return 0
    }

    displayText: currentIndex >= 0 && currentIndex < count
        ? model[currentIndex].title
        : i18nc("@item", "Open launcher")

    onActivated: {
        const selection = model[currentIndex] || { launcherId: "", backend: "", gameId: "", title: "" }
        if (sessionManager) {
            sessionManager.setInstanceGame(instanceIndex, selection)
        }
        gameSelected(selection)
    }
}
