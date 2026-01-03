// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 CouchPlay Contributors

import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

/**
 * DeviceCard - Displays an input device with assignment controls
 * 
 * Shows device name, type (keyboard/mouse/controller), and allows
 * assignment to a specific player/instance via drag-drop or selection.
 */
Kirigami.AbstractCard {
    id: root

    required property string deviceName
    required property string devicePath
    required property string deviceType // "keyboard", "mouse", "controller"
    property int assignedTo: -1 // -1 = unassigned, 0+ = player index
    property bool isDragging: false

    signal assignmentChanged(string devicePath, int playerIndex)
    signal clicked()

    implicitWidth: 280
    implicitHeight: 80

    contentItem: RowLayout {
        spacing: Kirigami.Units.smallSpacing

        Kirigami.Icon {
            source: {
                switch (root.deviceType) {
                    case "keyboard": return "input-keyboard"
                    case "mouse": return "input-mouse"
                    case "controller": return "input-gamepad"
                    default: return "input-keyboard"
                }
            }
            Layout.preferredWidth: Kirigami.Units.iconSizes.medium
            Layout.preferredHeight: Kirigami.Units.iconSizes.medium
        }

        ColumnLayout {
            Layout.fillWidth: true
            spacing: 2

            QQC2.Label {
                text: root.deviceName
                elide: Text.ElideRight
                Layout.fillWidth: true
            }

            QQC2.Label {
                text: root.devicePath
                font: Kirigami.Theme.smallFont
                opacity: 0.7
                elide: Text.ElideMiddle
                Layout.fillWidth: true
            }
        }

        Kirigami.Chip {
            text: root.assignedTo >= 0 ? qsTr("Player %1").arg(root.assignedTo + 1) : qsTr("Unassigned")
            checkable: false
            closable: root.assignedTo >= 0
            onClicked: root.clicked()
            onRemoved: root.assignmentChanged(root.devicePath, -1)
        }
    }

    Drag.active: dragArea.drag.active
    Drag.hotSpot.x: width / 2
    Drag.hotSpot.y: height / 2

    MouseArea {
        id: dragArea
        anchors.fill: parent
        drag.target: root.isDragging ? root : undefined
        onClicked: root.clicked()
        onPressAndHold: root.isDragging = true
        onReleased: root.isDragging = false
    }
}
