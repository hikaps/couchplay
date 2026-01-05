// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 CouchPlay Contributors

import QtQuick
import QtQuick.Layouts
import QtQuick.Controls as QQC2
import org.kde.kirigami as Kirigami

import "../components" as Components

Kirigami.ScrollablePage {
    id: root
    
    title: i18nc("@title", "Assign Devices")
    
    // Reference to DeviceManager (injected from parent)
    required property var deviceManager
    property int instanceCount: 2

    actions: [
        Kirigami.Action {
            text: i18nc("@action:button", "Refresh")
            icon.name: "view-refresh"
            onTriggered: deviceManager?.refresh()
        },
        Kirigami.Action {
            text: i18nc("@action:button", "Auto-Assign")
            icon.name: "distribute-horizontal"
            tooltip: i18nc("@info:tooltip", "Automatically assign one controller per player")
            onTriggered: {
                if (!deviceManager) return
                let count = deviceManager.autoAssignControllers()
                if (count > 0) {
                    applicationWindow().showPassiveNotification(
                        i18nc("@info", "Assigned %1 controller(s)", count))
                } else {
                    applicationWindow().showPassiveNotification(
                        i18nc("@info", "No controllers available to assign"))
                }
            }
        },
        Kirigami.Action {
            text: i18nc("@action:button", "Clear All")
            icon.name: "edit-clear-all"
            onTriggered: deviceManager?.unassignAll()
        }
    ]

    header: QQC2.ToolBar {
        contentItem: RowLayout {
            spacing: Kirigami.Units.smallSpacing

            QQC2.Label {
                text: i18nc("@label", "Players:")
            }

            QQC2.SpinBox {
                id: instanceCountSpin
                from: 2
                to: 4
                value: root.instanceCount
                onValueChanged: {
                    root.instanceCount = value
                    if (deviceManager) deviceManager.instanceCount = value
                }
            }

            Item { Layout.fillWidth: true }

            QQC2.CheckBox {
                text: i18nc("@option:check", "Show virtual devices")
                checked: deviceManager?.showVirtualDevices ?? false
                onToggled: { if (deviceManager) deviceManager.showVirtualDevices = checked }
            }
        }
    }

    ColumnLayout {
        spacing: Kirigami.Units.largeSpacing

        // Instructions
        Kirigami.InlineMessage {
            Layout.fillWidth: true
            type: Kirigami.MessageType.Information
            text: i18nc("@info", "Drag devices from the list below and drop them onto a player zone, or click a device to assign it to the next available player.")
            visible: true
        }

        // Player drop zones
        RowLayout {
            Layout.fillWidth: true
            spacing: Kirigami.Units.largeSpacing

            Repeater {
                model: root.instanceCount

                delegate: PlayerDropZone {
                    id: dropZoneDelegate
                    Layout.fillWidth: true
                    Layout.preferredHeight: 200
                    
                    required property int index
                    playerIndex: dropZoneDelegate.index
                    assignedDevices: (deviceManager?.visibleDevices ?? []).filter(d => d.assignedInstance === dropZoneDelegate.playerIndex)
                    
                    onDeviceDropped: (eventNumber) => {
                        deviceManager?.assignDevice(eventNumber, dropZoneDelegate.playerIndex)
                    }
                    onDeviceRemoved: (eventNumber) => {
                        deviceManager?.assignDevice(eventNumber, -1)
                    }
                }
            }
        }

        Kirigami.Separator {
            Layout.fillWidth: true
        }

        // Available devices section
        Kirigami.Heading {
            level: 3
            text: i18nc("@title:group", "Available Devices")
        }

        // Device type tabs
        QQC2.TabBar {
            id: deviceTypeBar
            Layout.fillWidth: true

            QQC2.TabButton {
                text: "Controllers (" + (deviceManager?.controllers?.length ?? 0) + ")"
                icon.name: "input-gamepad"
            }
            QQC2.TabButton {
                text: "Keyboards (" + (deviceManager?.keyboards?.length ?? 0) + ")"
                icon.name: "input-keyboard"
            }
            QQC2.TabButton {
                text: "Mice (" + (deviceManager?.mice?.length ?? 0) + ")"
                icon.name: "input-mouse"
            }
        }

        StackLayout {
            Layout.fillWidth: true
            currentIndex: deviceTypeBar.currentIndex

            // Controllers list
            DeviceList {
                deviceModel: deviceManager?.controllers ?? []
                deviceType: "controller"
                instanceCount: root.instanceCount
                onAssignDevice: (eventNumber, instanceIndex) => {
                    deviceManager?.assignDevice(eventNumber, instanceIndex)
                }
                onIdentifyDevice: (eventNumber) => {
                    deviceManager?.identifyDevice(eventNumber)
                }
            }

            // Keyboards list
            DeviceList {
                deviceModel: deviceManager?.keyboards ?? []
                deviceType: "keyboard"
                instanceCount: root.instanceCount
                onAssignDevice: (eventNumber, instanceIndex) => {
                    deviceManager?.assignDevice(eventNumber, instanceIndex)
                }
                onIdentifyDevice: (eventNumber) => {
                    // Keyboards don't support identification
                }
            }

            // Mice list
            DeviceList {
                deviceModel: deviceManager?.mice ?? []
                deviceType: "mouse"
                instanceCount: root.instanceCount
                onAssignDevice: (eventNumber, instanceIndex) => {
                    deviceManager?.assignDevice(eventNumber, instanceIndex)
                }
                onIdentifyDevice: (eventNumber) => {
                    // Mice don't support identification
                }
            }
        }

        // Empty state
        Kirigami.PlaceholderMessage {
            Layout.fillWidth: true
            visible: (deviceManager?.visibleDevices?.length ?? 0) === 0
            text: i18nc("@info", "No input devices found")
            explanation: i18nc("@info", "Connect controllers, keyboards, or mice to see them here.")
            icon.name: "input-gamepad"
            
            helpfulAction: Kirigami.Action {
                text: i18nc("@action:button", "Refresh Devices")
                icon.name: "view-refresh"
                onTriggered: deviceManager?.refresh()
            }
        }
    }

    // Player drop zone component
    component PlayerDropZone: Kirigami.Card {
        id: dropZone
        
        required property int playerIndex
        required property var assignedDevices
        
        signal deviceDropped(int eventNumber)
        signal deviceRemoved(int eventNumber)

        property bool isDragHover: false

        banner {
            title: i18nc("@title", "Player %1", playerIndex + 1)
            titleIcon: "user-identity"
        }

        background: Rectangle {
            color: dropZone.isDragHover 
                ? Qt.alpha(Kirigami.Theme.highlightColor, 0.2)
                : Kirigami.Theme.backgroundColor
            border.color: dropZone.isDragHover 
                ? Kirigami.Theme.highlightColor 
                : Kirigami.Theme.disabledTextColor
            border.width: dropZone.isDragHover ? 2 : 1
            radius: Kirigami.Units.smallSpacing

            Behavior on color {
                ColorAnimation { duration: 150 }
            }
        }

        contentItem: ColumnLayout {
            spacing: Kirigami.Units.smallSpacing

            // Show assigned devices
            Repeater {
                model: dropZone.assignedDevices

                Kirigami.Chip {
                    Layout.fillWidth: true
                    text: modelData.name
                    icon.name: {
                        switch (modelData.type) {
                            case "controller": return "input-gamepad"
                            case "keyboard": return "input-keyboard"
                            case "mouse": return "input-mouse"
                            default: return "input-gamepad"
                        }
                    }
                    closable: true
                    onRemoved: dropZone.deviceRemoved(modelData.eventNumber)
                }
            }

            // Empty state for drop zone
            Kirigami.PlaceholderMessage {
                Layout.fillWidth: true
                Layout.fillHeight: true
                visible: dropZone.assignedDevices.length === 0
                text: i18nc("@info", "Drop devices here")
                icon.name: "list-add"
            }
        }

        DropArea {
            anchors.fill: parent
            keys: ["application/x-couchplay-device"]

            onEntered: (drag) => {
                dropZone.isDragHover = true
            }
            onExited: {
                dropZone.isDragHover = false
            }
            onDropped: (drop) => {
                dropZone.isDragHover = false
                if (drop.hasText) {
                    let eventNumber = parseInt(drop.text)
                    dropZone.deviceDropped(eventNumber)
                }
            }
        }
    }

    // Device list component
    component DeviceList: ColumnLayout {
        id: deviceListRoot
        
        required property var deviceModel
        required property string deviceType
        required property int instanceCount
        
        signal assignDevice(int eventNumber, int instanceIndex)
        signal identifyDevice(int eventNumber)

        spacing: Kirigami.Units.smallSpacing

        Repeater {
            model: deviceListRoot.deviceModel

            delegate: DraggableDeviceCard {
                Layout.fillWidth: true
                
                required property var modelData
                device: modelData
                instanceCount: deviceListRoot.instanceCount
                
                onAssign: (eventNumber, instanceIndex) => {
                    deviceListRoot.assignDevice(eventNumber, instanceIndex)
                }
                onIdentify: (eventNumber) => {
                    deviceListRoot.identifyDevice(eventNumber)
                }
            }
        }

        // Empty state for this device type
        Kirigami.PlaceholderMessage {
            Layout.fillWidth: true
            visible: deviceListRoot.deviceModel.length === 0
            text: {
                switch (deviceListRoot.deviceType) {
                    case "controller": return i18nc("@info", "No controllers detected")
                    case "keyboard": return i18nc("@info", "No keyboards detected")
                    case "mouse": return i18nc("@info", "No mice detected")
                    default: return i18nc("@info", "No devices detected")
                }
            }
            icon.name: {
                switch (deviceListRoot.deviceType) {
                    case "controller": return "input-gamepad"
                    case "keyboard": return "input-keyboard"
                    case "mouse": return "input-mouse"
                    default: return "input-gamepad"
                }
            }
        }
    }

    // Draggable device card component
    component DraggableDeviceCard: Kirigami.AbstractCard {
        id: deviceCard
        
        required property var device
        required property int instanceCount
        
        signal assign(int eventNumber, int instanceIndex)
        signal identify(int eventNumber)

        property bool isDragging: false
        
        // Guard against undefined device
        visible: deviceCard.device !== undefined && deviceCard.device !== null

        contentItem: RowLayout {
            spacing: Kirigami.Units.smallSpacing

            Kirigami.Icon {
                source: {
                    if (!deviceCard.device) return "input-gamepad"
                    switch (deviceCard.device.type) {
                        case "controller": return "input-gamepad"
                        case "keyboard": return "input-keyboard"
                        case "mouse": return "input-mouse"
                        default: return "input-gamepad"
                    }
                }
                Layout.preferredWidth: Kirigami.Units.iconSizes.medium
                Layout.preferredHeight: Kirigami.Units.iconSizes.medium
            }

            ColumnLayout {
                Layout.fillWidth: true
                spacing: 0

                QQC2.Label {
                    text: deviceCard.device?.name ?? ""
                    elide: Text.ElideRight
                    Layout.fillWidth: true
                }

                QQC2.Label {
                    text: deviceCard.device?.path ?? ""
                    font: Kirigami.Theme.smallFont
                    opacity: 0.7
                    elide: Text.ElideMiddle
                    Layout.fillWidth: true
                }
            }

            // Assignment indicator
            Kirigami.Chip {
                visible: deviceCard.device?.assigned ?? false
                text: i18nc("@info", "Player %1", (deviceCard.device?.assignedInstance ?? 0) + 1)
                closable: true
                onRemoved: {
                    if (deviceCard.device) {
                        deviceCard.assign(deviceCard.device.eventNumber, -1)
                    }
                }
            }

            // Quick assign buttons
            Repeater {
                model: deviceCard.instanceCount
                
                QQC2.Button {
                    required property int index
                    text: (index + 1).toString()
                    visible: !(deviceCard.device?.assigned ?? true)
                    flat: true
                    
                    QQC2.ToolTip.visible: hovered
                    QQC2.ToolTip.text: i18nc("@info:tooltip", "Assign to Player %1", index + 1)
                    
                    onClicked: {
                        if (deviceCard.device) {
                            deviceCard.assign(deviceCard.device.eventNumber, index)
                        }
                    }
                }
            }

            // Identify button (controllers only)
            QQC2.Button {
                icon.name: "flashlight-on"
                flat: true
                visible: deviceCard.device?.type === "controller"
                
                QQC2.ToolTip.visible: hovered
                QQC2.ToolTip.text: i18nc("@info:tooltip", "Identify (vibrate controller)")
                
                onClicked: {
                    if (deviceCard.device) {
                        deviceCard.identify(deviceCard.device.eventNumber)
                    }
                }
            }
        }

        // Drag support
        Drag.active: dragArea.drag.active
        Drag.keys: ["application/x-couchplay-device"]
        Drag.mimeData: { "text/plain": (deviceCard.device?.eventNumber ?? 0).toString() }
        Drag.dragType: Drag.Automatic
        
        MouseArea {
            id: dragArea
            anchors.fill: parent
            z: -1  // Place behind contentItem so buttons receive clicks
            drag.target: deviceCard.isDragging ? deviceCard : undefined
            
            onPressed: {
                deviceCard.isDragging = true
            }
            onReleased: {
                deviceCard.isDragging = false
            }
        }

        states: State {
            when: deviceCard.isDragging
            PropertyChanges {
                target: deviceCard
                opacity: 0.5
            }
        }
    }
}
