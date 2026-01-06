// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 CouchPlay Contributors

import QtQuick
import QtQuick.Layouts
import QtQuick.Controls as Controls
import org.kde.kirigami as Kirigami

Kirigami.ScrollablePage {
    id: root

    title: i18nc("@title", "New Session")

    // References to backend managers (from Main.qml)
    required property var sessionManager
    required property var sessionRunner
    required property var deviceManager
    required property var monitorManager
    required property var userManager

    // Sync with session manager
    property int instanceCount: sessionManager ? sessionManager.instanceCount : 2
    property string layoutMode: sessionManager ? sessionManager.currentLayout : "horizontal"

    // Helper function to get primary monitor size
    function getPrimaryMonitorSize() {
        if (!monitorManager) return { width: 1920, height: 1080 }
        let monitors = monitorManager.monitors
        for (let i = 0; i < monitors.length; i++) {
            if (monitors[i].primary) {
                return { width: monitors[i].width, height: monitors[i].height }
            }
        }
        // Fallback to first monitor or default
        if (monitors.length > 0) {
            return { width: monitors[0].width, height: monitors[0].height }
        }
        return { width: 1920, height: 1080 }
    }

    onLayoutModeChanged: {
        if (sessionManager) {
            sessionManager.currentLayout = layoutMode
            // Recalculate output resolutions based on screen size
            let screenSize = getPrimaryMonitorSize()
            sessionManager.recalculateOutputResolutions(screenSize.width, screenSize.height)
            // Show feedback to user
            let layoutName = ""
            switch (layoutMode) {
                case "horizontal": layoutName = i18nc("@info", "Side by Side"); break
                case "vertical": layoutName = i18nc("@info", "Top and Bottom"); break
                case "grid": layoutName = i18nc("@info", "Grid"); break
                case "multi-monitor": layoutName = i18nc("@info", "Multi-Monitor"); break
            }
            applicationWindow().showPassiveNotification(
                i18nc("@info", "Layout: %1", layoutName))
        }
    }

    onInstanceCountChanged: {
        if (sessionManager && sessionManager.instanceCount !== instanceCount) {
            sessionManager.instanceCount = instanceCount
            // Recalculate output resolutions when instance count changes
            let screenSize = getPrimaryMonitorSize()
            sessionManager.recalculateOutputResolutions(screenSize.width, screenSize.height)
        }
    }

    actions: [
        Kirigami.Action {
            icon.name: "media-playback-start"
            text: sessionRunner && sessionRunner.running 
                ? i18nc("@action:button", "Stop Session")
                : i18nc("@action:button", "Start Session")
            onTriggered: {
                if (sessionRunner.running) {
                    sessionRunner.stop()
                } else {
                    sessionRunner.start()
                }
            }
        },
        Kirigami.Action {
            icon.name: "go-next"
            text: i18nc("@action:button", "Assign Devices")
            onTriggered: {
                applicationWindow().pushDeviceAssignmentPage()
            }
        },
        Kirigami.Action {
            icon.name: "document-save"
            text: i18nc("@action:button", "Save Profile")
            onTriggered: saveProfileDialog.open()
        }
    ]

    // Save profile dialog
    Kirigami.PromptDialog {
        id: saveProfileDialog
        title: i18nc("@title:dialog", "Save Profile")
        subtitle: sessionManager?.currentProfileName 
            ? i18nc("@info", "Save changes to '%1' or enter a new name", sessionManager.currentProfileName)
            : i18nc("@info", "Enter a name for this session profile")
        standardButtons: Kirigami.Dialog.Save | Kirigami.Dialog.Cancel

        Controls.TextField {
            id: profileNameField
            placeholderText: i18nc("@info:placeholder", "Profile name")
            text: sessionManager?.currentProfileName ?? ""
            Layout.fillWidth: true
        }

        onAccepted: {
            if (profileNameField.text.trim() !== "") {
                sessionManager.saveProfile(profileNameField.text.trim())
                applicationWindow().showPassiveNotification(
                    i18nc("@info", "Profile saved: %1", profileNameField.text))
            }
        }
        
        onOpened: {
            // Pre-fill with current profile name when editing
            profileNameField.text = sessionManager?.currentProfileName ?? ""
        }
    }

    ColumnLayout {
        spacing: Kirigami.Units.largeSpacing

        // Running session status
        Kirigami.InlineMessage {
            Layout.fillWidth: true
            visible: sessionRunner?.running ?? false
            type: Kirigami.MessageType.Positive
            text: i18nc("@info", "Session running: %1 instance(s) active", 
                       sessionRunner ? sessionRunner.runningInstanceCount : 0)
            
            actions: [
                Kirigami.Action {
                    text: i18nc("@action:button", "Stop")
                    icon.name: "media-playback-stop"
                    onTriggered: sessionRunner.stop()
                }
            ]
        }

        // Layout Selection
        Kirigami.Heading {
            text: i18nc("@title", "Screen Layout")
            level: 2
        }

        Controls.Label {
            text: i18nc("@info", "Choose how to arrange the game instances on your screen.")
            wrapMode: Text.WordWrap
            Layout.fillWidth: true
            opacity: 0.7
        }

        // Player count selector
        RowLayout {
            Layout.fillWidth: true
            spacing: Kirigami.Units.largeSpacing

            Controls.Label {
                text: i18nc("@label", "Number of players:")
            }

            Controls.SpinBox {
                id: playerCountSpin
                from: 2
                to: 4
                value: root.instanceCount
                onValueModified: root.instanceCount = value
            }
        }

        RowLayout {
            spacing: Kirigami.Units.largeSpacing
            Layout.fillWidth: true
            Layout.topMargin: Kirigami.Units.smallSpacing

            // Horizontal split
            LayoutCard {
                Layout.fillWidth: true
                layoutType: "horizontal"
                selected: layoutMode === "horizontal"
                title: i18nc("@option", "Side by Side")
                description: i18nc("@info", "Split screen horizontally")
                instanceCount: root.instanceCount
                onClicked: layoutMode = "horizontal"
            }

            // Vertical split
            LayoutCard {
                Layout.fillWidth: true
                layoutType: "vertical"
                selected: layoutMode === "vertical"
                title: i18nc("@option", "Top and Bottom")
                description: i18nc("@info", "Split screen vertically")
                instanceCount: root.instanceCount
                onClicked: layoutMode = "vertical"
            }

            // Grid (for 3-4 players)
            LayoutCard {
                Layout.fillWidth: true
                layoutType: "grid"
                selected: layoutMode === "grid"
                title: i18nc("@option", "Grid")
                description: i18nc("@info", "2x2 grid layout")
                instanceCount: root.instanceCount
                visible: root.instanceCount > 2
                onClicked: layoutMode = "grid"
            }

            // Multi-monitor
            LayoutCard {
                Layout.fillWidth: true
                layoutType: "multi-monitor"
                selected: layoutMode === "multi-monitor"
                title: i18nc("@option", "Multi-Monitor")
                description: i18nc("@info", "One instance per monitor")
                instanceCount: root.instanceCount
                onClicked: layoutMode = "multi-monitor"
            }
        }

        // Instance Configuration
        Kirigami.Heading {
            text: i18nc("@title", "Instance Configuration")
            level: 2
            Layout.topMargin: Kirigami.Units.largeSpacing
        }

        Repeater {
            model: root.instanceCount

            delegate: Kirigami.Card {
                id: instanceCard
                Layout.fillWidth: true

                required property int index
                
                // Pre-translated strings to avoid i18nc scoping issues inside FormLayout
                readonly property string labelUser: i18nc("@label", "User:")
                readonly property string labelResolution: i18nc("@label", "Game Resolution:")
                readonly property string labelRefreshRate: i18nc("@label", "Refresh Rate:")
                readonly property string labelScaling: i18nc("@label", "Scaling:")
                readonly property string labelDevices: i18nc("@label", "Devices:")

                readonly property string textNoneAssigned: i18nc("@info", "None assigned")
                readonly property string textAssign: i18nc("@action:button", "Assign...")

                header: Kirigami.Heading {
                    text: i18nc("@title", "Player %1", instanceCard.index + 1)
                    level: 3
                    padding: Kirigami.Units.smallSpacing
                }

                contentItem: Kirigami.FormLayout {
                    // Prevent binding loop on wideMode
                    wideMode: false
                    
                    // Get current config for this instance
                    property var config: sessionManager ? sessionManager.getInstanceConfig(instanceCard.index) : ({})

                    // Update controls when config changes (e.g., after profile load)
                    Connections {
                        target: sessionManager
                        function onInstancesChanged() {
                            let cfg = sessionManager ? sessionManager.getInstanceConfig(instanceCard.index) : ({})
                            resWidthSpin.value = cfg.internalWidth || 1920
                            resHeightSpin.value = cfg.internalHeight || 1080
                            refreshSpin.value = cfg.refreshRate || 60
                            // Update user combo selection from config
                            let savedUser = cfg.username || ""
                            let idx = userCombo.userList.indexOf(savedUser)
                            if (idx >= 0) {
                                userCombo.currentIndex = idx
                            }
                        }
                    }

                    Controls.ComboBox {
                        id: userCombo
                        Kirigami.FormData.label: instanceCard.labelUser
                        Layout.fillWidth: true
                        
                        // Current user's username (for filtering)
                        readonly property string currentUsername: userManager ? userManager.currentUser : ""
                        
                        // Build simple user list - no dynamic filtering to avoid binding loops
                        // Instance 0: current user only
                        // Instance 1+: all secondary users (validation happens at session start)
                        readonly property var userList: {
                            if (instanceCard.index === 0) {
                                return currentUsername ? [currentUsername] : []
                            }
                            
                            // Secondary instances: show all non-current users
                            let users = []
                            let allUsers = userManager ? userManager.users : []
                            for (let i = 0; i < allUsers.length; i++) {
                                let user = allUsers[i]
                                if (user.username !== currentUsername) {
                                    users.push(user.username)
                                }
                            }
                            return users
                        }
                        
                        model: userList
                        
                        // Show placeholder when no users available
                        displayText: userList.length === 0 
                            ? i18nc("@info", "No users available") 
                            : (currentIndex >= 0 && currentIndex < userList.length ? userList[currentIndex] : "")
                        
                        // Set initial selection from config
                        Component.onCompleted: {
                            let cfg = sessionManager ? sessionManager.getInstanceConfig(instanceCard.index) : ({})
                            let savedUser = cfg.username || ""
                            let idx = userList.indexOf(savedUser)
                            currentIndex = idx >= 0 ? idx : 0
                        }
                        
                        // Only update on actual user interaction (not programmatic changes)
                        onActivated: function(index) {
                            if (sessionManager && index >= 0 && index < userList.length) {
                                sessionManager.setInstanceUser(instanceCard.index, userList[index])
                            }
                        }
                    }
                    
                    // Warning when no secondary users available
                    Controls.Label {
                        visible: instanceCard.index > 0 && userCombo.userList.length === 0
                        text: i18nc("@info", "Create a user in Settings to enable this instance")
                        color: Kirigami.Theme.neutralTextColor
                        font.italic: true
                        wrapMode: Text.WordWrap
                        Layout.fillWidth: true
                    }

                    RowLayout {
                        Kirigami.FormData.label: instanceCard.labelResolution
                        spacing: Kirigami.Units.smallSpacing

                        Controls.SpinBox {
                            id: resWidthSpin
                            from: 640
                            to: 3840
                            value: parent.parent.config.internalWidth || 1920
                            stepSize: 10
                            editable: true
                            
                            onValueModified: updateResolution()
                        }

                        Controls.Label {
                            text: "x"
                        }

                        Controls.SpinBox {
                            id: resHeightSpin
                            from: 480
                            to: 2160
                            value: parent.parent.config.internalHeight || 1080
                            stepSize: 10
                            editable: true
                            
                            onValueModified: updateResolution()
                        }

                        function updateResolution() {
                            if (sessionManager) {
                                // For split-screen, output size is calculated by SessionRunner
                                sessionManager.setInstanceResolution(
                                    instanceCard.index,
                                    resWidthSpin.value, resHeightSpin.value,
                                    resWidthSpin.value, resHeightSpin.value
                                )
                            }
                        }
                    }

                    Controls.SpinBox {
                        id: refreshSpin
                        Kirigami.FormData.label: instanceCard.labelRefreshRate
                        from: 30
                        to: 240
                        value: parent.config.refreshRate || 60
                        textFromValue: function(value) { return value + " Hz" }
                        valueFromText: function(text) { return parseInt(text) }
                        
                        onValueModified: {
                            if (sessionManager) {
                                let config = sessionManager.getInstanceConfig(instanceCard.index)
                                config.refreshRate = value
                                sessionManager.setInstanceConfig(instanceCard.index, config)
                            }
                        }
                    }

                    Controls.ComboBox {
                        Kirigami.FormData.label: instanceCard.labelScaling
                        model: ["fit", "stretch", "integer", "auto"]
                        currentIndex: 0
                        Layout.fillWidth: true
                    }

                    // Show assigned devices
                    RowLayout {
                        Kirigami.FormData.label: instanceCard.labelDevices
                        spacing: Kirigami.Units.smallSpacing

                        Controls.Label {
                            text: {
                                if (!deviceManager) return instanceCard.textNoneAssigned
                                let paths = deviceManager.getDevicePathsForInstance(instanceCard.index)
                                if (paths.length === 0) return instanceCard.textNoneAssigned
                                return paths.length === 1 
                                    ? (paths.length + " device")
                                    : (paths.length + " devices")
                            }
                            opacity: 0.7
                        }

                        Controls.Button {
                            text: instanceCard.textAssign
                            flat: true
                            onClicked: applicationWindow().pushDeviceAssignmentPage()
                        }
                    }
                }
            }
        }

        // Quick actions
        Kirigami.Separator {
            Layout.fillWidth: true
            Layout.topMargin: Kirigami.Units.largeSpacing
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: Kirigami.Units.largeSpacing

            Controls.Button {
                text: i18nc("@action:button", "Auto-Assign Controllers")
                icon.name: "input-gamepad"
                onClicked: {
                    if (deviceManager) {
                        deviceManager.instanceCount = root.instanceCount
                        let count = deviceManager.autoAssignControllers()
                        applicationWindow().showPassiveNotification(
                            i18ncp("@info", "Assigned %1 controller", "Assigned %1 controllers", count))
                    }
                }
            }

            Item { Layout.fillWidth: true }

            Controls.Button {
                text: sessionRunner && sessionRunner.running 
                    ? i18nc("@action:button", "Stop Session")
                    : i18nc("@action:button", "Start Session")
                icon.name: sessionRunner && sessionRunner.running 
                    ? "media-playback-stop" 
                    : "media-playback-start"
                highlighted: true
                onClicked: {
                    if (sessionRunner.running) {
                        sessionRunner.stop()
                    } else {
                        sessionRunner.start()
                    }
                }
            }
        }
    }

    // Layout card component for visual selection
    component LayoutCard: Kirigami.Card {
        id: layoutCard

        required property string layoutType
        required property bool selected
        required property string title
        required property string description
        required property int instanceCount

        Layout.preferredHeight: Kirigami.Units.gridUnit * 10
        highlighted: selected

        contentItem: ColumnLayout {
            spacing: Kirigami.Units.smallSpacing

            // Visual representation
            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: Kirigami.Units.gridUnit * 4
                color: "transparent"
                border.color: Kirigami.Theme.textColor
                border.width: 1
                radius: 4

                // Dynamic layout visualization
                Loader {
                    anchors.fill: parent
                    anchors.margins: 2
                    sourceComponent: {
                        switch (layoutCard.layoutType) {
                            case "horizontal": return horizontalLayout
                            case "vertical": return verticalLayout
                            case "grid": return gridLayout
                            case "multi-monitor": return multiMonitorLayout
                            default: return horizontalLayout
                        }
                    }
                }
            }

            Controls.Label {
                text: layoutCard.title
                font.bold: true
                Layout.alignment: Qt.AlignHCenter
            }

            Controls.Label {
                text: layoutCard.description
                Layout.alignment: Qt.AlignHCenter
                opacity: 0.7
            }
        }
    }

    // Layout visualization components
    Component {
        id: horizontalLayout
        RowLayout {
            spacing: 2
            Repeater {
                model: root.instanceCount
                Rectangle {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    color: index === 0 ? Kirigami.Theme.highlightColor : Kirigami.Theme.positiveBackgroundColor
                    opacity: 0.5
                    radius: 2
                    Controls.Label {
                        anchors.centerIn: parent
                        text: (index + 1).toString()
                        font.bold: true
                    }
                }
            }
        }
    }

    Component {
        id: verticalLayout
        ColumnLayout {
            spacing: 2
            Repeater {
                model: root.instanceCount
                Rectangle {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    color: index === 0 ? Kirigami.Theme.highlightColor : Kirigami.Theme.positiveBackgroundColor
                    opacity: 0.5
                    radius: 2
                    Controls.Label {
                        anchors.centerIn: parent
                        text: (index + 1).toString()
                        font.bold: true
                    }
                }
            }
        }
    }

    Component {
        id: gridLayout
        GridLayout {
            columns: 2
            rows: 2
            rowSpacing: 2
            columnSpacing: 2
            Repeater {
                model: Math.min(root.instanceCount, 4)
                Rectangle {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    color: index === 0 ? Kirigami.Theme.highlightColor : Kirigami.Theme.positiveBackgroundColor
                    opacity: 0.5
                    radius: 2
                    Controls.Label {
                        anchors.centerIn: parent
                        text: (index + 1).toString()
                        font.bold: true
                    }
                }
            }
        }
    }

    Component {
        id: multiMonitorLayout
        RowLayout {
            spacing: Kirigami.Units.smallSpacing
            Repeater {
                model: root.instanceCount
                Rectangle {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    color: "transparent"
                    border.color: Kirigami.Theme.textColor
                    border.width: 1
                    radius: 4
                    Rectangle {
                        anchors.fill: parent
                        anchors.margins: 2
                        color: index === 0 ? Kirigami.Theme.highlightColor : Kirigami.Theme.positiveBackgroundColor
                        opacity: 0.5
                        radius: 2
                        Controls.Label {
                            anchors.centerIn: parent
                            text: (index + 1).toString()
                            font.bold: true
                        }
                    }
                }
            }
        }
    }
}
