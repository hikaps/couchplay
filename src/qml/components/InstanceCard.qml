// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 CouchPlay Contributors

import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

/**
 * InstanceCard - Displays a gamescope instance configuration
 * 
 * Shows instance status, assigned user, devices, and provides
 * controls for configuration and launching.
 */
Kirigami.Card {
    id: root

    required property int instanceIndex
    required property string userName
    property string status: "stopped" // "stopped", "starting", "running", "error"
    property var assignedDevices: [] // List of device paths
    property rect geometry: Qt.rect(0, 0, 1920, 1080)

    signal configure()
    signal launch()
    signal stop()

    banner {
        title: qsTr("Player %1").arg(instanceIndex + 1)
        titleIcon: "user-identity"
    }

    contentItem: ColumnLayout {
        spacing: Kirigami.Units.smallSpacing

        // User assignment
        RowLayout {
            spacing: Kirigami.Units.smallSpacing
            Layout.fillWidth: true

            Kirigami.Icon {
                source: "user"
                Layout.preferredWidth: Kirigami.Units.iconSizes.small
                Layout.preferredHeight: Kirigami.Units.iconSizes.small
            }

            QQC2.Label {
                text: root.userName || qsTr("No user assigned")
                opacity: root.userName ? 1.0 : 0.6
                Layout.fillWidth: true
            }
        }

        // Resolution/geometry
        RowLayout {
            spacing: Kirigami.Units.smallSpacing
            Layout.fillWidth: true

            Kirigami.Icon {
                source: "view-fullscreen"
                Layout.preferredWidth: Kirigami.Units.iconSizes.small
                Layout.preferredHeight: Kirigami.Units.iconSizes.small
            }

            QQC2.Label {
                text: qsTr("%1x%2 at (%3, %4)").arg(root.geometry.width).arg(root.geometry.height).arg(root.geometry.x).arg(root.geometry.y)
                Layout.fillWidth: true
            }
        }

        // Assigned devices summary
        RowLayout {
            spacing: Kirigami.Units.smallSpacing
            Layout.fillWidth: true

            Kirigami.Icon {
                source: "input-gamepad"
                Layout.preferredWidth: Kirigami.Units.iconSizes.small
                Layout.preferredHeight: Kirigami.Units.iconSizes.small
            }

            QQC2.Label {
                text: root.assignedDevices.length > 0 
                    ? qsTr("%1 device(s)").arg(root.assignedDevices.length)
                    : qsTr("No devices assigned")
                opacity: root.assignedDevices.length > 0 ? 1.0 : 0.6
                Layout.fillWidth: true
            }
        }

        // Status indicator
        RowLayout {
            spacing: Kirigami.Units.smallSpacing
            Layout.fillWidth: true

            Rectangle {
                width: Kirigami.Units.smallSpacing * 2
                height: width
                radius: width / 2
                color: {
                    switch (root.status) {
                        case "running": return Kirigami.Theme.positiveTextColor
                        case "starting": return Kirigami.Theme.neutralTextColor
                        case "error": return Kirigami.Theme.negativeTextColor
                        default: return Kirigami.Theme.disabledTextColor
                    }
                }
            }

            QQC2.Label {
                text: {
                    switch (root.status) {
                        case "running": return qsTr("Running")
                        case "starting": return qsTr("Starting...")
                        case "error": return qsTr("Error")
                        default: return qsTr("Stopped")
                    }
                }
                Layout.fillWidth: true
            }
        }
    }

    actions: [
        Kirigami.Action {
            text: qsTr("Configure")
            icon.name: "configure"
            onTriggered: root.configure()
        },
        Kirigami.Action {
            text: root.status === "running" ? qsTr("Stop") : qsTr("Launch")
            icon.name: root.status === "running" ? "media-playback-stop" : "media-playback-start"
            onTriggered: root.status === "running" ? root.stop() : root.launch()
        }
    ]
}
