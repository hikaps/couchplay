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

    required property var device
    property int instanceCount: 2
    property bool showAssignButtons: true
    property bool showIdentifyButton: true
    property bool isDragging: false

    signal assignmentRequested(int eventNumber, int playerIndex)
    signal identifyRequested(int eventNumber)
    signal clicked()

    // Convenience properties
    readonly property string deviceName: device?.name ?? ""
    readonly property string devicePath: device?.path ?? ""
    readonly property string deviceType: device?.type ?? "other"
    readonly property int eventNumber: device?.eventNumber ?? -1
    readonly property bool isAssigned: device?.assigned ?? false
    readonly property int assignedTo: device?.assignedInstance ?? -1
    readonly property bool isVirtual: device?.isVirtual ?? false

    implicitWidth: 320
    implicitHeight: 72

    opacity: root.isDragging ? 0.5 : 1.0

    contentItem: RowLayout {
        spacing: Kirigami.Units.smallSpacing

        // Device type icon
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
            
            // Dim virtual devices
            opacity: root.isVirtual ? 0.5 : 1.0
        }

        // Device info
        ColumnLayout {
            Layout.fillWidth: true
            spacing: 2

            RowLayout {
                spacing: Kirigami.Units.smallSpacing
                
                QQC2.Label {
                    text: root.deviceName
                    elide: Text.ElideRight
                    Layout.fillWidth: true
                    font.weight: root.isAssigned ? Font.Bold : Font.Normal
                }
                
                // Virtual device indicator
                Kirigami.Icon {
                    source: "virtual-reality"
                    visible: root.isVirtual
                    Layout.preferredWidth: Kirigami.Units.iconSizes.small
                    Layout.preferredHeight: Kirigami.Units.iconSizes.small
                    opacity: 0.5
                    
                    QQC2.ToolTip.visible: virtualHover.containsMouse
                    QQC2.ToolTip.text: qsTr("Virtual device")
                    
                    HoverHandler {
                        id: virtualHover
                    }
                }
            }

            QQC2.Label {
                text: root.devicePath
                font: Kirigami.Theme.smallFont
                opacity: 0.7
                elide: Text.ElideMiddle
                Layout.fillWidth: true
            }
        }

        // Assignment chip (when assigned)
        Kirigami.Chip {
            visible: root.isAssigned
            text: qsTr("Player %1").arg(root.assignedTo + 1)
            checkable: false
            closable: true
            
            icon.name: "user-identity"
            
            onRemoved: root.assignmentRequested(root.eventNumber, -1)
        }

        // Quick assign buttons (when not assigned)
        Row {
            visible: !root.isAssigned && root.showAssignButtons
            spacing: Kirigami.Units.smallSpacing

            Repeater {
                model: root.instanceCount

                QQC2.Button {
                    text: (index + 1).toString()
                    flat: true
                    implicitWidth: height
                    
                    QQC2.ToolTip.visible: hovered
                    QQC2.ToolTip.text: qsTr("Assign to Player %1").arg(index + 1)
                    
                    onClicked: root.assignmentRequested(root.eventNumber, index)
                }
            }
        }

        // Identify button (for controllers)
        QQC2.Button {
            visible: root.showIdentifyButton && root.deviceType === "controller"
            icon.name: "flashlight-on"
            flat: true
            
            QQC2.ToolTip.visible: hovered
            QQC2.ToolTip.text: qsTr("Identify (vibrate)")
            
            onClicked: root.identifyRequested(root.eventNumber)
        }
    }

    // Drag support
    Drag.active: dragArea.drag.active
    Drag.keys: ["application/x-couchplay-device"]
    Drag.mimeData: { "text/plain": root.eventNumber.toString() }
    Drag.hotSpot.x: width / 2
    Drag.hotSpot.y: height / 2

    MouseArea {
        id: dragArea
        anchors.fill: parent
        drag.target: root.isDragging ? root : undefined
        
        onClicked: root.clicked()
        
        onPressAndHold: {
            root.isDragging = true
            root.grabToImage(function(result) {
                root.Drag.imageSource = result.url
            })
        }
        
        onReleased: {
            root.isDragging = false
            root.Drag.drop()
        }
    }

    // Visual feedback states
    states: [
        State {
            name: "dragging"
            when: root.isDragging
            PropertyChanges {
                target: root
                opacity: 0.5
                scale: 1.05
            }
        },
        State {
            name: "assigned"
            when: root.isAssigned && !root.isDragging
            PropertyChanges {
                target: root
                // Could add visual distinction for assigned devices
            }
        }
    ]

    transitions: [
        Transition {
            from: "*"
            to: "dragging"
            NumberAnimation {
                properties: "opacity,scale"
                duration: 150
                easing.type: Easing.OutQuad
            }
        },
        Transition {
            from: "dragging"
            to: "*"
            NumberAnimation {
                properties: "opacity,scale"
                duration: 150
                easing.type: Easing.InQuad
            }
        }
    ]
}
