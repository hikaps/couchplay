// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 CouchPlay Contributors

import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

/**
 * GameCard - Displays a game with launch and shortcut options
 * 
 * Shows game icon/banner, name, and provides actions for launching
 * in split-screen mode or creating desktop shortcuts.
 */
Kirigami.Card {
    id: root

    required property string gameId
    required property string gameName
    property string iconPath: ""
    property string bannerPath: ""
    property string source: "steam" // "steam", "custom"
    property bool hasShortcut: false

    signal launch()
    signal createShortcut()
    signal removeShortcut()
    signal configure()

    banner {
        // Use banner image if available
        source: root.bannerPath
        title: root.gameName
        titleIcon: root.iconPath || "applications-games"
    }

    contentItem: RowLayout {
        spacing: Kirigami.Units.smallSpacing

        // Source indicator
        Kirigami.Chip {
            text: {
                switch (root.source) {
                    case "steam": return "Steam"
                    case "custom": return qsTr("Custom")
                    default: return root.source
                }
            }
            icon.name: {
                switch (root.source) {
                    case "steam": return "steam" // May need a custom icon
                    default: return "application-x-executable"
                }
            }
            checkable: false
        }

        Item { Layout.fillWidth: true }

        // Shortcut indicator
        Kirigami.Icon {
            source: "emblem-symbolic-link"
            visible: root.hasShortcut
            Layout.preferredWidth: Kirigami.Units.iconSizes.small
            Layout.preferredHeight: Kirigami.Units.iconSizes.small

            QQC2.ToolTip.visible: shortcutHover.containsMouse
            QQC2.ToolTip.text: qsTr("Desktop shortcut exists")

            HoverHandler {
                id: shortcutHover
            }
        }
    }

    actions: [
        Kirigami.Action {
            text: qsTr("Launch")
            icon.name: "media-playback-start"
            onTriggered: root.launch()
        },
        Kirigami.Action {
            text: root.hasShortcut ? qsTr("Remove Shortcut") : qsTr("Create Shortcut")
            icon.name: root.hasShortcut ? "edit-delete" : "list-add"
            onTriggered: root.hasShortcut ? root.removeShortcut() : root.createShortcut()
        },
        Kirigami.Action {
            text: qsTr("Configure")
            icon.name: "configure"
            onTriggered: root.configure()
        }
    ]
}
