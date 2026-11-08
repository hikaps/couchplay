// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 CouchPlay Contributors

import QtQuick
import QtQuick.Layouts
import QtQuick.Controls as Controls
import org.kde.kirigami as Kirigami
import QtQuick.Dialogs

import "../components" as Components

Kirigami.ScrollablePage {
    id: root

    title: i18nc("@title", "New Session")

    required property var sessionManager
    required property var sessionRunner
    required property var deviceManager
    required property var monitorManager
    required property var userManager
    required property var presetManager
    required property var steamConfigManager
    required property var heroicConfigManager
    required property var audioManager

    property int instanceCount: sessionManager ? sessionManager.instanceCount : 2
    property string layoutMode: sessionManager ? sessionManager.currentLayout : "horizontal"

    function selectedHostFile(fileUrl) {
        let path = fileUrl.toString()
        if (path.startsWith("file://")) path = path.substring(7)
        path = decodeURIComponent(path)
        return presetManager ? presetManager.resolveHostPath(path) : path
    }
    property string gridSubLayout: sessionManager ? sessionManager.currentGridSubLayout : ""

    property int instancesRevision: 0
    property int devicesRevision: 0
    property int presetsRevision: 0

    Connections {
        target: root.sessionManager
        function onInstancesChanged() {
            root.instancesRevision++
        }
    }
    Connections {
        target: root.presetManager
        function onPresetsChanged() {
            root.presetsRevision++
        }
    }

    
    Connections {
        target: root.deviceManager
        function onDevicesChanged() {
            root.devicesRevision++
        }
        function onPendingDevicesChanged() {
            root.devicesRevision++
        }
    }

    // Get available users for a specific instance
    function getAvailableUsers(forIndex) {
        // Reference instancesRevision to create binding dependency
        void(root.instancesRevision)
        if (!root.userManager || !root.sessionManager) return []
        let allUsers = root.userManager.users
        let assignedToOthers = root.sessionManager.getAssignedUsers(forIndex)
        let filtered = allUsers.filter(user => !user.isCurrent && !assignedToOthers.includes(user.username))
        // Prepend a "None" entry so users can deselect their choice
        return [{ username: "" }].concat(filtered)
    }

    // Connector of the effective main screen: the saved one if connected, else the primary monitor.
    property string mainScreenConnector: {
        const monitors = monitorManager ? monitorManager.monitors : []
        const saved = sessionManager ? sessionManager.currentMainScreen : ""
        if (saved !== "") {
            for (let i = 0; i < monitors.length; i++) {
                if (monitors[i].connector === saved) return saved
            }
        }
        for (let i = 0; i < monitors.length; i++) {
            if (monitors[i].primary) return monitors[i].connector
        }
        return monitors.length > 0 ? monitors[0].connector : saved
    }

    // Number of players placed on the main screen (display === "main")
    property int mainPlayerCount: {
        void(instancesRevision)
        if (!sessionManager) return instanceCount
        let n = 0
        for (let i = 0; i < instanceCount; i++) {
            const display = sessionManager.getInstanceConfig(i)?.display || "main"
            if (display === "main") n++
        }
        return n
    }

    function getMainScreenSize() {
        if (!monitorManager) return { width: 1920, height: 1080 }
        let monitors = monitorManager.monitors
        for (let i = 0; i < monitors.length; i++) {
            if (monitors[i].connector === mainScreenConnector) {
                return { width: monitors[i].width, height: monitors[i].height }
            }
        }
        if (monitors.length > 0) {
            return { width: monitors[0].width, height: monitors[0].height }
        }
        return { width: 1920, height: 1080 }
    }

    function recalculateResolutions() {
        if (!sessionManager) return
        let screenSize = getMainScreenSize()
        sessionManager.recalculateOutputResolutions(screenSize.width, screenSize.height)
    }

    Connections {
        target: root.sessionManager
        function onCurrentMainScreenChanged() { root.recalculateResolutions() }
    }

    Connections {
        target: root.monitorManager
        function onMonitorsChanged() { root.recalculateResolutions() }
    }

    onLayoutModeChanged: {
        if (sessionManager) {
            sessionManager.currentLayout = layoutMode
            recalculateResolutions()
        }
    }

    onGridSubLayoutChanged: {
        if (sessionManager && sessionManager.currentLayout === "grid") {
            sessionManager.currentGridSubLayout = gridSubLayout
            recalculateResolutions()
        }
    }

    onInstanceCountChanged: {
        if (sessionManager && sessionManager.instanceCount !== instanceCount) {
            sessionManager.instanceCount = instanceCount
            recalculateResolutions()
        }
    }

    actions: [
        Kirigami.Action {
            objectName: "actionStartSession"
            Accessible.role: Accessible.Button
            Accessible.name: sessionRunner && sessionRunner.active
                ? i18nc("@action:button", "Stop Session")
                : i18nc("@action:button", "Start Session")
            Accessible.onPressAction: triggered()
            icon.name: "media-playback-start"
            text: sessionRunner && sessionRunner.active
                ? i18nc("@action:button", "Stop Session")
                : i18nc("@action:button", "Start Session")
            onTriggered: {
                if (sessionRunner.active) {
                    sessionRunner.stop()
                } else {
                    sessionRunner.start()
                }
            }
        },
        Kirigami.Action {
            objectName: "actionAssignDevices"
            Accessible.role: Accessible.Button
            Accessible.name: i18nc("@action:button", "Assign Devices")
            Accessible.onPressAction: triggered()
            icon.name: "go-next"
            text: i18nc("@action:button", "Assign Devices")
            onTriggered: {
                applicationWindow().pushDeviceAssignmentPage()
            }
        },
        Kirigami.Action {
            objectName: "actionSaveProfile"
            Accessible.role: Accessible.Button
            Accessible.name: i18nc("@action:button", "Save Profile")
            Accessible.onPressAction: triggered()
            icon.name: "document-save"
            text: i18nc("@action:button", "Save Profile")
            onTriggered: saveProfileDialog.open()
        }
    ]

    Kirigami.PromptDialog {
        id: saveProfileDialog
        objectName: "dialogSaveProfile"
        title: i18nc("@title:dialog", "Save Profile")
        subtitle: sessionManager?.currentProfileName 
            ? i18nc("@info", "Save changes to '%1' or enter a new name", sessionManager.currentProfileName)
            : i18nc("@info", "Enter a name for this session profile")
        standardButtons: Kirigami.Dialog.Save | Kirigami.Dialog.Cancel

        Controls.TextField {
            id: profileNameField
            objectName: "fieldProfileName"
            Accessible.role: Accessible.EditableText
            Accessible.name: i18nc("@label", "Profile name")
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

    FileDialog {
        id: preSessionScriptDialog
        objectName: "dialogPreSessionScript"
        title: i18nc("@title:dialog", "Select Pre-session Script")
        fileMode: FileDialog.OpenFile
        onAccepted: {
            const path = root.selectedHostFile(selectedFile)
            if (path && sessionManager) sessionManager.preSessionExecutable = path
        }
    }

    FileDialog {
        id: postSessionScriptDialog
        objectName: "dialogPostSessionScript"
        title: i18nc("@title:dialog", "Select Post-session Script")
        fileMode: FileDialog.OpenFile
        onAccepted: {
            const path = root.selectedHostFile(selectedFile)
            if (path && sessionManager) sessionManager.postSessionExecutable = path
        }
    }

    ColumnLayout {
        spacing: Kirigami.Units.largeSpacing

        Kirigami.FormLayout {
            Layout.fillWidth: true

            Kirigami.Heading {
                text: i18nc("@title", "Automation")
                level: 2
                Kirigami.FormData.label: i18nc("@title", "Automation")
            }

            RowLayout {
                Layout.fillWidth: true
                Kirigami.FormData.label: i18nc("@label", "Pre-session script:")

                Controls.TextField {
                    objectName: "fieldPreSessionScript"
                    Accessible.role: Accessible.EditableText
                    Accessible.name: i18nc("@label", "Pre-session script")
                    readOnly: true
                    text: sessionManager?.preSessionExecutable ?? ""
                    placeholderText: i18nc("@info:placeholder", "Optional executable")
                    Layout.fillWidth: true
                }

                Controls.Button {
                    objectName: "btnBrowsePreSessionScript"
                    text: i18nc("@action:button", "Browse...")
                    Accessible.role: Accessible.Button
                    Accessible.name: text
                    onClicked: preSessionScriptDialog.open()
                }

                Controls.Button {
                    objectName: "btnClearPreSessionScript"
                    text: i18nc("@action:button", "Clear")
                    Accessible.role: Accessible.Button
                    Accessible.name: text
                    enabled: (sessionManager?.preSessionExecutable ?? "") !== ""
                    onClicked: if (sessionManager) sessionManager.preSessionExecutable = ""
                }
            }

            RowLayout {
                Layout.fillWidth: true
                Kirigami.FormData.label: i18nc("@label", "Post-session script:")

                Controls.TextField {
                    objectName: "fieldPostSessionScript"
                    Accessible.role: Accessible.EditableText
                    Accessible.name: i18nc("@label", "Post-session script")
                    readOnly: true
                    text: sessionManager?.postSessionExecutable ?? ""
                    placeholderText: i18nc("@info:placeholder", "Optional executable")
                    Layout.fillWidth: true
                }

                Controls.Button {
                    objectName: "btnBrowsePostSessionScript"
                    text: i18nc("@action:button", "Browse...")
                    Accessible.role: Accessible.Button
                    Accessible.name: text
                    onClicked: postSessionScriptDialog.open()
                }

                Controls.Button {
                    objectName: "btnClearPostSessionScript"
                    text: i18nc("@action:button", "Clear")
                    Accessible.role: Accessible.Button
                    Accessible.name: text
                    enabled: (sessionManager?.postSessionExecutable ?? "") !== ""
                    onClicked: if (sessionManager) sessionManager.postSessionExecutable = ""
                }
            }
        }

        Kirigami.InlineMessage {
            Layout.fillWidth: true
            visible: sessionRunner?.running ?? false
            type: Kirigami.MessageType.Positive
            text: i18nc("@info", "Session running: %1 instance(s) active", 
                       sessionRunner ? sessionRunner.runningInstanceCount : 0)
            
            actions: [
                Kirigami.Action {
                    objectName: "actionStop"
                    Accessible.role: Accessible.Button
                    Accessible.name: i18nc("@action:button", "Stop")
                    Accessible.onPressAction: triggered()
                    text: i18nc("@action:button", "Stop")
                    icon.name: "media-playback-stop"
                    onTriggered: sessionRunner.stop()
                }
            ]
        }

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

        RowLayout {
            Layout.fillWidth: true
            spacing: Kirigami.Units.largeSpacing

            Controls.Label {
                text: i18nc("@label", "Number of players:")
            }

            Controls.SpinBox {
                id: playerCountSpin
                objectName: "spinPlayerCount"
                Accessible.role: Accessible.SpinBox
                Accessible.name: i18nc("@label", "Number of players")
                from: 2
                to: 4
                value: root.instanceCount
                onValueModified: root.instanceCount = value
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: Kirigami.Units.largeSpacing

            Controls.Label {
                text: i18nc("@label", "Main screen:")
            }

            Controls.ComboBox {
                id: mainScreenCombo
                objectName: "comboMainScreen"
                Accessible.role: Accessible.ComboBox
                Accessible.name: i18nc("@label", "Main screen")
                Layout.fillWidth: true
                textRole: "displayString"
                valueRole: "connector"
                model: root.monitorManager ? root.monitorManager.monitors : []

                function updateIndex() {
                    const idx = indexOfValue(root.mainScreenConnector)
                    currentIndex = idx >= 0 ? idx : 0
                }

                onModelChanged: updateIndex()
                Component.onCompleted: updateIndex()

                Connections {
                    target: root
                    function onMainScreenConnectorChanged() { mainScreenCombo.updateIndex() }
                }

                onActivated: {
                    if (root.sessionManager && currentValue) {
                        root.sessionManager.currentMainScreen = currentValue
                    }
                }
            }
        }

        Controls.Label {
            objectName: "labelMainScreenArrangement"
            text: i18nc("@info", "Arrangement for the %1 players on the main screen", root.mainPlayerCount)
            wrapMode: Text.WordWrap
            Layout.fillWidth: true
            Layout.topMargin: Kirigami.Units.smallSpacing
            opacity: 0.7
        }

        RowLayout {
            spacing: Kirigami.Units.largeSpacing
            Layout.fillWidth: true
            Layout.topMargin: Kirigami.Units.smallSpacing
            visible: root.mainPlayerCount > 1

            LayoutCard {
                objectName: "cardLayoutHorizontal"
                Layout.fillWidth: true
                layoutType: "horizontal"
                selected: layoutMode === "horizontal"
                title: i18nc("@option", "Side by Side")
                description: i18nc("@info", "Split screen horizontally")
                instanceCount: root.mainPlayerCount
                onClicked: layoutMode = "horizontal"
            }

            LayoutCard {
                objectName: "cardLayoutVertical"
                Layout.fillWidth: true
                layoutType: "vertical"
                selected: layoutMode === "vertical"
                title: i18nc("@option", "Top and Bottom")
                description: i18nc("@info", "Split screen vertically")
                instanceCount: root.mainPlayerCount
                onClicked: layoutMode = "vertical"
            }
            LayoutCard {
                objectName: "cardLayoutGrid"
                Layout.fillWidth: true
                layoutType: "grid"
                selected: layoutMode === "grid"
                title: i18nc("@option", "Grid")
                description: root.mainPlayerCount === 3
                    ? (root.gridSubLayout === "grid-2x2"
                        ? i18nc("@info", "2×2 grid with one empty cell")
                        : root.gridSubLayout === "left-right"
                            ? i18nc("@info", "Player 1 left, players 2–3 right")
                            : i18nc("@info", "3×1 horizontal layout"))
                    : i18nc("@info", "2×2 grid layout")
                instanceCount: root.mainPlayerCount
                visible: root.mainPlayerCount > 2
                onClicked: layoutMode = "grid"
            }
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.topMargin: Kirigami.Units.smallSpacing
            Layout.leftMargin: Kirigami.Units.largeSpacing
            visible: root.mainPlayerCount > 1 && layoutMode === "grid" && root.mainPlayerCount === 3
            spacing: Kirigami.Units.mediumSpacing

            Controls.Label {
                text: i18nc("@label", "Grid arrangement:")
                opacity: 0.7
            }

            Repeater {
                model: ListModel {
                    ListElement { modeValue: "horizontal"; modeLabel: "3×1 Horizontal" }
                    ListElement { modeValue: "grid-2x2"; modeLabel: "2×2 Grid" }
                    ListElement { modeValue: "left-right"; modeLabel: "Left + Right" }
                }

                Controls.Button {
                    text: model.modeLabel
                    flat: true
                    checked: root.gridSubLayout === model.modeValue
                    checkable: true

                    onClicked: root.gridSubLayout = model.modeValue

                    Rectangle {
                        anchors.fill: parent
                        anchors.margins: -Kirigami.Units.smallSpacing
                        color: "transparent"
                        border.width: parent.checked ? 2 : 0
                        border.color: Kirigami.Theme.highlightColor
                        radius: Kirigami.Units.smallSpacing
                    }
                }
            }
        }

        Kirigami.Heading {
            text: i18nc("@title", "Instance Configuration")
            level: 2
            Layout.topMargin: Kirigami.Units.largeSpacing
        }

        Repeater {
            model: root.instanceCount

            delegate: Kirigami.AbstractCard {
                id: instanceCard
                Layout.fillWidth: true
                Layout.bottomMargin: Kirigami.Units.smallSpacing

                required property int index
                
                // Pass managers through to avoid root access issues in bindings
                property var cardPresetManager: root.presetManager
                property var cardSessionManager: root.sessionManager
                
                property int cardRevision: root?.instancesRevision ?? 0
                property int cardPresetRevision: root?.presetsRevision ?? 0
                
                property string currentPresetId: {
                    void(cardRevision)
                    if (!cardSessionManager) return "steam"
                    let config = cardSessionManager.getInstanceConfig(instanceCard.index)
                    return config?.presetId || "steam"
                }
                
                property string displayValue: {
                    void(cardRevision)
                    if (!cardSessionManager) return "main"
                    let config = cardSessionManager.getInstanceConfig(instanceCard.index)
                    return config?.display || "main"
                }

                property bool isStreaming: displayValue === "virtual"

                property bool streamToMoonlightValue: {
                    void(cardRevision)
                    if (!cardSessionManager) return false
                    let config = cardSessionManager.getInstanceConfig(instanceCard.index)
                    return config?.streamToMoonlight ?? false
                }

                // Virtual displays always stream; real monitors stream only when the toggle is on
                property bool streamsToMoonlight: displayValue === "virtual" || (displayValue !== "main" && streamToMoonlightValue)

                readonly property var streamManager: root.sessionRunner ? root.sessionRunner.streamManager : null

                // Entry of streamManager.streams for this player; only exists while a session runs
                readonly property var streamEntry: {
                    const list = streamManager ? streamManager.streams : []
                    for (let i = 0; i < list.length; ++i) {
                        if (list[i].instanceIndex === instanceCard.index) return list[i]
                    }
                    return null
                }

                // StreamManager.StreamState: 1 = Waiting, 2 = Streaming, 3 = Error
                readonly property bool streamReady: streamEntry !== null && streamEntry.state === 2

                function streamStatusText() {
                    if (streamEntry === null) return ""
                    if (streamEntry.state === 2) return i18nc("@info:status", "Ready on port %1", streamEntry.port)
                    if (streamEntry.state === 3) return i18nc("@info:status", "Failed: %1", streamEntry.error)
                    return i18nc("@info:status", "Starting")
                }
                
                
                // Pre-translated strings to avoid i18nc scoping issues inside FormLayout
                readonly property string labelUser: i18nc("@label", "User:")
                readonly property string labelLauncher: i18nc("@label", "Launcher:")
                readonly property string labelDisplay: i18nc("@label", "Display:")
                readonly property string optionMainScreen: i18nc("@item:inlistbox", "Main screen")
                readonly property string optionVirtualDisplay: i18nc("@item:inlistbox", "Virtual display (Moonlight only)")
                readonly property string labelVirtualResolution: i18nc("@label", "Virtual Resolution:")
                readonly property string optionCodecAuto: i18nc("@item:inlistbox", "Automatic")
                readonly property string labelStreamToMoonlight: i18nc("@label", "Moonlight:")
                readonly property string textStreamToMoonlight: i18nc("@option:check", "Stream to Moonlight")
                readonly property string textStreamMonitorHint: i18nc("@info", "Streaming a real monitor also shows the game on that monitor.")
                readonly property string labelStreamStatus: i18nc("@label", "Stream:")
                readonly property string textPairMoonlight: i18nc("@action:button", "Pair Moonlight…")
                readonly property string labelBitrate: i18nc("@label", "Bitrate:")
                readonly property string labelCodec: i18nc("@label", "Video Codec:")
                readonly property string labelResolution: i18nc("@label", "Window Size:")
                readonly property string labelRenderResolution: i18nc("@label", "Render Resolution:")
                readonly property string optionRenderAuto: i18nc("@item:inlistbox", "Auto (match window)")
                readonly property string labelAudioOutput: i18nc("@label", "Audio Output:")
                readonly property string optionAudioDefault: i18nc("@item:inlistbox", "Default output")
                property var cardAudioManager: root.audioManager
                readonly property string labelRefreshRate: i18nc("@label", "Refresh Rate:")
                readonly property string labelScaling: i18nc("@label", "Scaling:")
                readonly property string labelGame: i18nc("@label", "Game:")
                readonly property string labelDevices: i18nc("@label", "Devices:")
                readonly property string labelPlayerData: i18nc("@action:button", "Player data")
                readonly property string tooltipPlayerData: i18nc("@info:tooltip", "Open a folder where you can place per-player files (e.g. configs). They are copied into this player's copy of the shared directories at session start.")


                readonly property string labelPrivateTemporaryDirectories: i18nc("@option:check", "Private temporary directories")
                readonly property string tooltipPrivateTemporaryDirectories: i18nc("@info:tooltip", "Use private /tmp and /var/tmp directories for this instance. They are removed when the instance stops and are not part of the shared-data snapshot.")

                readonly property string textNoneAssigned: i18nc("@info", "None assigned")
                readonly property string textAssign: i18nc("@action:button", "Assign...")

                header: RowLayout {
                    spacing: Kirigami.Units.smallSpacing

                    Kirigami.Heading {
                        text: i18nc("@title", "Player %1", instanceCard.index + 1)
                        level: 3
                        padding: Kirigami.Units.smallSpacing
                    }

                    Kirigami.Icon {
                        visible: instanceCard.streamsToMoonlight
                        source: "network-wireless"
                        Layout.preferredWidth: Kirigami.Units.iconSizes.small
                        Layout.preferredHeight: Kirigami.Units.iconSizes.small
                        opacity: 0.7
                        Controls.ToolTip.visible: hovered
                        Controls.ToolTip.text: i18nc("@info:tooltip", "Streaming via Moonlight")
                        Controls.ToolTip.delay: Kirigami.Units.toolTipDelay

                        HoverHandler { id: streamIconHover }
                        // Workaround: Kirigami.Icon doesn't natively expose hovered
                        property bool hovered: streamIconHover.hovered
                    }

                    Item { Layout.fillWidth: true }
                }

                contentItem: ColumnLayout {
                    id: cardContentLayout
                    spacing: Kirigami.Units.smallSpacing
                    clip: true

                    Kirigami.FormLayout {
                        Layout.fillWidth: true
                        wideMode: (root?.width ?? 0) > Kirigami.Units.gridUnit * 30

                        Controls.ComboBox {
                            id: userCombo
                            objectName: "comboUser"
                            Accessible.role: Accessible.ComboBox
                            Accessible.name: instanceCard.labelUser
                            Kirigami.FormData.label: instanceCard.labelUser
                            Layout.fillWidth: true

                            model: root.getAvailableUsers(instanceCard.index)
                            textRole: "username"
                            valueRole: "username"

                            onModelChanged: {
                                let config = root.sessionManager?.getInstanceConfig(instanceCard.index)
                                let currentUsername = config?.username ?? ""
                                if (currentUsername && model) {
                                    for (let i = 0; i < model.length; i++) {
                                        if (model[i].username === currentUsername) {
                                            currentIndex = i
                                            return
                                        }
                                    }
                                }
                                currentIndex = -1
                            }

                            displayText: count === 0
                                ? i18nc("@info", "No users available")
                                : (currentIndex >= 0 ? currentText : i18nc("@info", "Select a user..."))

                            onActivated: {
                                if (root.sessionManager && currentValue) {
                                    root.sessionManager.setInstanceUser(instanceCard.index, currentValue)
                                }
                            }
                        }

                        Controls.Label {
                            visible: userCombo.currentIndex < 0 && userCombo.count > 0
                            text: i18nc("@info:status", "Please select a user for this instance")
                            color: Kirigami.Theme.neutralTextColor
                            font.italic: true
                            wrapMode: Text.WordWrap
                            Layout.fillWidth: true
                        }

                        Components.PresetSelector {
                            id: presetSelector
                            objectName: "comboLauncher"
                            Kirigami.FormData.label: instanceCard.labelLauncher
                            Layout.fillWidth: true
                            presetManager: instanceCard.cardPresetManager
                            currentPresetId: instanceCard.currentPresetId

                            onPresetSelected: function(presetId) {
                                if (instanceCard.cardSessionManager) {
                                    instanceCard.cardSessionManager.setInstancePreset(instanceCard.index, presetId)
                                }
                            }
                        }
                        Kirigami.InlineMessage {
                            objectName: "sandboxedLauncherWarning"
                            Layout.fillWidth: true
                            visible: {
                                if (!instanceCard.cardPresetManager || instanceCard.currentPresetId === "") return false
                                const preset = instanceCard.cardPresetManager.getPreset(instanceCard.currentPresetId)
                                return preset && preset.id !== "" && preset.sandboxed
                            }

                            text: i18nc("@info", "Flatpak sandboxing may block applications launched by this launcher. Use a native launcher for generic child-process support.")
                            type: Kirigami.MessageType.Warning
                        }


                        Controls.Button {
                            objectName: "btnPlayerDataFolder"
                            visible: {
                                void(instanceCard.cardRevision)
                                void(instanceCard.cardPresetRevision)
                                return instanceCard.cardSessionManager
                                    ? instanceCard.cardSessionManager.hasPlayerDataStagingDirectories(instanceCard.index)
                                    : false
                            }
                            icon.name: "folder-open"
                            text: instanceCard.labelPlayerData
                            display: Controls.AbstractButton.TextBesideIcon
                            Layout.fillWidth: true
                            Accessible.role: Accessible.Button
                            Accessible.name: instanceCard.labelPlayerData
                            Controls.ToolTip.text: instanceCard.tooltipPlayerData
                            Controls.ToolTip.visible: hovered
                            Controls.ToolTip.delay: 1000
                            onClicked: {
                                if (!instanceCard.cardSessionManager)
                                    return
                                if (!instanceCard.cardSessionManager.openPlayerDataFolder(instanceCard.index)) {
                                    // If the desktop opener fails, retain the path as a manual fallback.
                                    const path = instanceCard.cardSessionManager.playerDataFolderPath(instanceCard.index)
                                    if (path)
                                        applicationWindow().showPassiveNotification(
                                            i18nc("@info:status", "Player data folder: %1", path), 10000)
                                }
                            }
                        }

                        Controls.ComboBox {
                            id: displayCombo
                            objectName: "comboDisplay"
                            Accessible.role: Accessible.ComboBox
                            Accessible.name: instanceCard.labelDisplay
                            Kirigami.FormData.label: instanceCard.labelDisplay
                            Layout.fillWidth: true
                            textRole: "text"
                            valueRole: "value"

                            function buildModel() {
                                void(instanceCard.cardRevision)
                                const mainConnector = root.mainScreenConnector
                                const saved = instanceCard.displayValue
                                let entries = [{ text: instanceCard.optionMainScreen, value: "main" }]
                                let known = false
                                const monitors = root.monitorManager ? root.monitorManager.monitors : []
                                for (let i = 0; i < monitors.length; ++i) {
                                    if (monitors[i].connector === mainConnector) continue
                                    const value = "monitor:" + monitors[i].connector
                                    entries.push({ text: monitors[i].displayString, value: value })
                                    if (value === saved) known = true
                                }
                                // Keep a saved monitor that is disconnected (or now the main screen) visible so the validation message stays explainable.
                                if (saved.startsWith("monitor:") && !known) entries.push({ text: saved.substring(8), value: saved })
                                entries.push({ text: instanceCard.optionVirtualDisplay, value: "virtual" })
                                return entries
                            }

                            function updateIndex() {
                                const idx = indexOfValue(instanceCard.displayValue)
                                currentIndex = idx >= 0 ? idx : 0
                            }

                            model: buildModel()
                            onModelChanged: updateIndex()
                            Component.onCompleted: updateIndex()

                            Connections {
                                target: instanceCard
                                function onDisplayValueChanged() { displayCombo.updateIndex() }
                            }

                            onActivated: {
                                if (instanceCard.cardSessionManager) {
                                    let config = instanceCard.cardSessionManager.getInstanceConfig(instanceCard.index)
                                    config.display = currentValue
                                    instanceCard.cardSessionManager.setInstanceConfig(instanceCard.index, config)
                                    root.recalculateResolutions()
                                }
                            }
                        }

                        ColumnLayout {
                            Kirigami.FormData.label: instanceCard.labelStreamToMoonlight
                            visible: instanceCard.displayValue !== "main"
                            Layout.fillWidth: true
                            spacing: Kirigami.Units.smallSpacing

                            Controls.Switch {
                                objectName: "switchStreamToMoonlight"
                                Accessible.role: Accessible.CheckBox
                                Accessible.name: instanceCard.textStreamToMoonlight
                                text: instanceCard.textStreamToMoonlight
                                enabled: instanceCard.displayValue !== "virtual"
                                checked: instanceCard.streamsToMoonlight
                                onToggled: {
                                    if (instanceCard.cardSessionManager) {
                                        let config = instanceCard.cardSessionManager.getInstanceConfig(instanceCard.index)
                                        config.streamToMoonlight = checked
                                        instanceCard.cardSessionManager.setInstanceConfig(instanceCard.index, config)
                                    }
                                }
                            }

                            Controls.Label {
                                visible: instanceCard.displayValue !== "virtual"
                                text: instanceCard.textStreamMonitorHint
                                wrapMode: Text.WordWrap
                                opacity: 0.7
                                Layout.fillWidth: true
                            }
                        }
                        Components.GameSelector {
                            objectName: "comboGame"
                            Kirigami.FormData.label: instanceCard.labelGame
                            presetManager: instanceCard.cardPresetManager
                            sessionManager: instanceCard.cardSessionManager
                            instanceIndex: instanceCard.index
                            steamConfigManager: root.steamConfigManager
                            heroicConfigManager: root.heroicConfigManager
                        }

                        // Resolution is auto-calculated from monitor size and layout
                        Controls.Label {
                            Kirigami.FormData.label: instanceCard.labelResolution
                            visible: !instanceCard.isStreaming
                            text: root.sessionManager ? (root.sessionManager.getInstanceConfig(instanceCard.index).outputWidth + " x " + root.sessionManager.getInstanceConfig(instanceCard.index).outputHeight) : "1920 x 1080"
                            opacity: 0.8
                        }
                        // Lower than the window size renders faster; gamescope scales it up to the window.
                        Controls.ComboBox {
                            id: renderResolutionCombo
                            objectName: "comboRenderResolution"
                            Accessible.role: Accessible.ComboBox
                            Accessible.name: instanceCard.labelRenderResolution
                            Kirigami.FormData.label: instanceCard.labelRenderResolution
                            visible: !instanceCard.isStreaming
                            Layout.fillWidth: true
                            textRole: "text"
                            valueRole: "value"
                            model: [
                                { text: instanceCard.optionRenderAuto, value: "" },
                                { text: "1280x720", value: "1280x720" },
                                { text: "1920x1080", value: "1920x1080" },
                                { text: "2560x1440", value: "2560x1440" }
                            ]

                            function updateIndex() {
                                if (!instanceCard.cardSessionManager) { currentIndex = 0; return }
                                let res = instanceCard.cardSessionManager.getInstanceConfig(instanceCard.index).renderResolution || ""
                                let idx = indexOfValue(res)
                                currentIndex = idx >= 0 ? idx : 0
                            }

                            Component.onCompleted: { updateIndex() }

                            Connections {
                                target: instanceCard
                                function onCardRevisionChanged() { renderResolutionCombo.updateIndex() }
                            }

                            onActivated: {
                                if (instanceCard.cardSessionManager) {
                                    let config = instanceCard.cardSessionManager.getInstanceConfig(instanceCard.index)
                                    config.renderResolution = currentValue
                                    instanceCard.cardSessionManager.setInstanceConfig(instanceCard.index, config)
                                }
                            }
                        }
                        // Audio device for this player; players share the host's PipeWire-Pulse, so a host sink name works.
                        Controls.ComboBox {
                            id: audioOutputCombo
                            objectName: "comboAudioOutput"
                            Accessible.role: Accessible.ComboBox
                            Accessible.name: instanceCard.labelAudioOutput
                            Kirigami.FormData.label: instanceCard.labelAudioOutput
                            visible: !instanceCard.streamsToMoonlight
                            Layout.fillWidth: true
                            textRole: "text"
                            valueRole: "value"

                            function savedDevice() {
                                if (!instanceCard.cardSessionManager) return ""
                                return instanceCard.cardSessionManager.getInstanceConfig(instanceCard.index).audioDevice || ""
                            }

                            function buildModel() {
                                let entries = [{ text: instanceCard.optionAudioDefault, value: "" }]
                                let known = false
                                const saved = savedDevice()
                                const devices = instanceCard.cardAudioManager ? instanceCard.cardAudioManager.outputDevices : []
                                for (let i = 0; i < devices.length; ++i) {
                                    entries.push({ text: devices[i].description, value: devices[i].name })
                                    if (devices[i].name === saved) known = true
                                }
                                // Keep a saved device that is currently unplugged visible instead of silently dropping it.
                                if (saved !== "" && !known) entries.push({ text: saved, value: saved })
                                return entries
                            }

                            function updateIndex() {
                                const idx = indexOfValue(savedDevice())
                                currentIndex = idx >= 0 ? idx : 0
                            }

                            model: buildModel()
                            onModelChanged: updateIndex()
                            Component.onCompleted: updateIndex()

                            Connections {
                                target: instanceCard
                                function onCardRevisionChanged() {
                                    audioOutputCombo.model = audioOutputCombo.buildModel()
                                    audioOutputCombo.updateIndex()
                                }
                            }

                            Connections {
                                target: instanceCard.cardAudioManager
                                function onOutputDevicesChanged() {
                                    audioOutputCombo.model = audioOutputCombo.buildModel()
                                }
                            }

                            onActivated: {
                                if (instanceCard.cardSessionManager) {
                                    let config = instanceCard.cardSessionManager.getInstanceConfig(instanceCard.index)
                                    config.audioDevice = currentValue
                                    instanceCard.cardSessionManager.setInstanceConfig(instanceCard.index, config)
                                }
                            }
                        }
                        Controls.ComboBox {
                            id: virtualResolutionCombo
                            objectName: "comboVirtualResolution"
                            Accessible.role: Accessible.ComboBox
                            Accessible.name: instanceCard.labelVirtualResolution
                            Kirigami.FormData.label: instanceCard.labelVirtualResolution
                            visible: instanceCard.isStreaming
                            Layout.fillWidth: true
                            model: ["1280x720", "1920x1080", "2560x1440", "3840x2160"]

                            function updateIndex() {
                                if (!instanceCard.cardSessionManager) { currentIndex = 1; return }
                                let res = instanceCard.cardSessionManager.getInstanceConfig(instanceCard.index).virtualResolution || "1920x1080"
                                let idx = model.indexOf(res)
                                currentIndex = idx >= 0 ? idx : 1
                            }

                            Component.onCompleted: { updateIndex() }

                            Connections {
                                target: instanceCard
                                function onCardRevisionChanged() { virtualResolutionCombo.updateIndex() }
                            }

                            onActivated: {
                                if (instanceCard.cardSessionManager) {
                                    let config = instanceCard.cardSessionManager.getInstanceConfig(instanceCard.index)
                                    config.virtualResolution = currentText
                                    instanceCard.cardSessionManager.setInstanceConfig(instanceCard.index, config)
                                }
                            }
                        }

                        RowLayout {
                            Kirigami.FormData.label: instanceCard.labelBitrate
                            visible: instanceCard.streamsToMoonlight
                            Layout.fillWidth: true
                            spacing: Kirigami.Units.smallSpacing
                            
                            Controls.Slider {
                                id: bitrateSlider
                                objectName: "sliderStreamBitrate"
                                Accessible.role: Accessible.Slider
                                Accessible.name: instanceCard.labelBitrate
                                Layout.fillWidth: true
                                from: 5000
                                to: 80000
                                stepSize: 1000

                                function updateValue() {
                                    if (!instanceCard.cardSessionManager) { value = 20000; return }
                                    let config = instanceCard.cardSessionManager.getInstanceConfig(instanceCard.index)
                                    value = config.streamBitrate || 20000
                                }

                                Component.onCompleted: { updateValue() }

                                Connections {
                                    target: instanceCard
                                    function onCardRevisionChanged() { bitrateSlider.updateValue() }
                                }

                                onMoved: {
                                    if (instanceCard.cardSessionManager) {
                                        let config = instanceCard.cardSessionManager.getInstanceConfig(instanceCard.index)
                                        config.streamBitrate = value
                                        instanceCard.cardSessionManager.setInstanceConfig(instanceCard.index, config)
                                    }
                                }
                            }
                            
                            Controls.Label {
                                text: i18nc("@unit", "%1 Mbps", (bitrateSlider.value / 1000).toFixed(0))
                                Layout.preferredWidth: Kirigami.Units.gridUnit * 4
                                horizontalAlignment: Text.AlignRight
                            }
                        }
                        
                        Controls.ComboBox {
                            id: codecCombo
                            objectName: "comboCodec"
                            Accessible.role: Accessible.ComboBox
                            Accessible.name: instanceCard.labelCodec
                            Kirigami.FormData.label: instanceCard.labelCodec
                            visible: instanceCard.streamsToMoonlight
                            Layout.fillWidth: true
                            textRole: "text"
                            valueRole: "value"
                            model: [
                                { text: instanceCard.optionCodecAuto, value: "auto" },
                                { text: "H.264", value: "h264" },
                                { text: "HEVC", value: "hevc" },
                                { text: "AV1", value: "av1" }
                            ]

                            function updateIndex() {
                                if (!instanceCard.cardSessionManager) { currentIndex = 0; return }
                                let codec = instanceCard.cardSessionManager.getInstanceConfig(instanceCard.index).streamCodec || "auto"
                                let idx = indexOfValue(codec)
                                currentIndex = idx >= 0 ? idx : 0
                            }

                            Component.onCompleted: { updateIndex() }

                            Connections {
                                target: instanceCard
                                function onCardRevisionChanged() { codecCombo.updateIndex() }
                            }

                            onActivated: {
                                if (instanceCard.cardSessionManager) {
                                    let config = instanceCard.cardSessionManager.getInstanceConfig(instanceCard.index)
                                    config.streamCodec = currentValue
                                    instanceCard.cardSessionManager.setInstanceConfig(instanceCard.index, config)
                                }
                            }
                        }

                        RowLayout {
                            Kirigami.FormData.label: instanceCard.labelStreamStatus
                            visible: instanceCard.streamsToMoonlight && instanceCard.streamEntry !== null
                            Layout.fillWidth: true
                            spacing: Kirigami.Units.smallSpacing

                            Controls.Label {
                                objectName: "labelStreamStatus"
                                text: instanceCard.streamStatusText()
                                wrapMode: Text.WordWrap
                                Layout.fillWidth: true
                            }

                            Controls.Button {
                                objectName: "buttonPairMoonlight"
                                Accessible.role: Accessible.Button
                                Accessible.name: instanceCard.textPairMoonlight
                                text: instanceCard.textPairMoonlight
                                icon.name: "network-connect"
                                enabled: instanceCard.streamReady
                                onClicked: pairingSheet.open()
                            }
                        }

                        Components.MoonlightPairingSheet {
                            id: pairingSheet
                            streamManager: instanceCard.streamManager
                            instanceIndex: instanceCard.index
                        }

                        RowLayout {
                            Kirigami.FormData.label: instanceCard.labelDevices
                            spacing: Kirigami.Units.smallSpacing

                            Controls.Label {
                                text: {
                                    // Reference devicesRevision to force re-evaluation on device changes
                                    void(root.devicesRevision)
                                    if (!root.deviceManager) return instanceCard.textNoneAssigned
                                    var paths = root.deviceManager.getDevicePathsForInstance(instanceCard.index)
                                    if (paths.length === 0) return instanceCard.textNoneAssigned
                                    return paths.length === 1
                                        ? (paths.length + " device")
                                        : (paths.length + " devices")
                                }
                                opacity: 0.7
                            }

                            Controls.Button {
                                objectName: "btnAssignDevices"
                                Accessible.role: Accessible.Button
                                Accessible.name: instanceCard.textAssign
                                Accessible.onPressAction: clicked()
                                text: instanceCard.textAssign
                                flat: true
                                onClicked: applicationWindow().pushDeviceAssignmentPage()
                            }
                        }
                        Controls.CheckBox {
                            objectName: "checkPrivateTemporaryDirectories"
                            Kirigami.FormData.label: instanceCard.labelPrivateTemporaryDirectories
                            Accessible.role: Accessible.CheckBox
                            Accessible.name: instanceCard.labelPrivateTemporaryDirectories
                            checked: {
                                void(instanceCard.cardRevision)
                                return instanceCard.cardSessionManager
                                    ? instanceCard.cardSessionManager.getInstanceConfig(instanceCard.index).privateTemporaryDirectories
                                    : false
                            }
                            onClicked: {
                                if (instanceCard.cardSessionManager) {
                                    let config = instanceCard.cardSessionManager.getInstanceConfig(instanceCard.index)
                                    config.privateTemporaryDirectories = checked
                                    instanceCard.cardSessionManager.setInstanceConfig(instanceCard.index, config)
                                }
                            }
                            Controls.ToolTip.text: instanceCard.tooltipPrivateTemporaryDirectories
                            Controls.ToolTip.visible: hovered
                            Controls.ToolTip.delay: Kirigami.Units.toolTipDelay
                        }

                        Controls.SpinBox {
                            id: refreshSpin
                            objectName: "spinRefreshRate"
                            Accessible.role: Accessible.SpinBox
                            Accessible.name: instanceCard.labelRefreshRate
                            Kirigami.FormData.label: instanceCard.labelRefreshRate
                            from: 30
                            to: 240
                            value: root.sessionManager ? root.sessionManager.getInstanceConfig(instanceCard.index).refreshRate : 60
                            textFromValue: function(value) { return value + " Hz" }
                            valueFromText: function(text) { return parseInt(text) }

                            onValueModified: {
                                if (root.sessionManager) {
                                    var config = root.sessionManager.getInstanceConfig(instanceCard.index)
                                    config.refreshRate = value
                                    root.sessionManager.setInstanceConfig(instanceCard.index, config)
                                }
                            }
                        }

                        Controls.ComboBox {
                            objectName: "comboScaling"
                            Accessible.role: Accessible.ComboBox
                            Accessible.name: instanceCard.labelScaling
                            Kirigami.FormData.label: instanceCard.labelScaling
                            model: ["fit", "stretch", "integer", "auto"]
                            currentIndex: 0
                            Layout.fillWidth: true
                        }

                    }


                    // Streaming status indicator
                    Kirigami.InlineMessage {
                        Layout.fillWidth: true
                        visible: instanceCard.isStreaming && root.sessionRunner && root.sessionRunner.active
                        type: Kirigami.MessageType.Information
                        text: i18nc("@info", "Sunshine streaming is active on port %1",
                                    root.sessionManager ? root.sessionManager.effectiveSunshinePort(instanceCard.index) : 47989)
                    }

                    // Warning for missing devices
                    Kirigami.InlineMessage {
                        Layout.fillWidth: true
                        visible: {
                            if (!root) return false
                            void(root.devicesRevision)
                            if (!root.deviceManager) return false
                            let pending = root.deviceManager.pendingDevices
                            for (let i = 0; i < pending.length; i++) {
                                // QVariant may convert int to different JS type
                                if (pending[i].instanceIndex == instanceCard.index) return true
                            }
                            return false
                        }
                        type: Kirigami.MessageType.Warning
                        text: {
                            if (!root) return ""
                            void(root.devicesRevision)
                            if (!root.deviceManager) return ""
                            let pending = root.deviceManager.pendingDevices
                            let names = []
                            for (let i = 0; i < pending.length; i++) {
                                // QVariant may convert int to different JS type
                                if (pending[i].instanceIndex == instanceCard.index) {
                                    names.push(pending[i].name)
                                }
                            }
                            return i18nc("@info", "%1 not connected", names.join(", "))
                        }
                    }

                    Kirigami.InlineMessage {
                        Layout.fillWidth: true
                        visible: {
                            if (!root) return false
                            void(root.devicesRevision)
                            if (!root.deviceManager) return false

                            let assignedPaths = root.deviceManager.getDevicePathsForInstance(instanceCard.index)
                            if (assignedPaths.length > 0) return false

                            // Disconnected warning takes priority
                            let pending = root.deviceManager.pendingDevices
                            for (let i = 0; i < pending.length; i++) {
                                if (pending[i].instanceIndex == instanceCard.index) return false
                            }

                            return true
                        }
                        type: Kirigami.MessageType.Information
                        text: i18nc("@info", "No controller assigned to this player")
                    }
                }
            }
        }

        Kirigami.Separator {
            Layout.fillWidth: true
            Layout.topMargin: Kirigami.Units.largeSpacing
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: Kirigami.Units.largeSpacing

            Controls.Button {
                objectName: "btnAutoAssign"
                Accessible.role: Accessible.Button
                Accessible.name: i18nc("@action:button", "Auto-Assign Controllers")
                Accessible.onPressAction: clicked()
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
        }
    }

    component LayoutCard: Kirigami.AbstractCard {
        id: layoutCard

        required property string layoutType
        required property bool selected
        required property string title
        required property string description
        required property int instanceCount

        Accessible.role: Accessible.Button
        Accessible.name: layoutCard.title
        Accessible.onPressAction: layoutCard.clicked()

        background: Rectangle {
            color: layoutCard.selected 
                ? Qt.alpha(Kirigami.Theme.highlightColor, 0.15) 
                : Kirigami.Theme.backgroundColor
            radius: Kirigami.Units.smallSpacing
            border.width: layoutCard.selected ? 3 : 1
            border.color: layoutCard.selected 
                ? Kirigami.Theme.highlightColor 
                : Qt.alpha(Kirigami.Theme.textColor, 0.15)

            Behavior on color {
                ColorAnimation { duration: Kirigami.Units.shortDuration }
            }
            Behavior on border.width {
                NumberAnimation { duration: Kirigami.Units.shortDuration }
            }
            Behavior on border.color {
                ColorAnimation { duration: Kirigami.Units.shortDuration }
            }
        }

        contentItem: ColumnLayout {
            spacing: Kirigami.Units.smallSpacing

            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: Kirigami.Units.gridUnit * 4
                color: "transparent"
                border.color: layoutCard.selected 
                    ? Kirigami.Theme.highlightColor 
                    : Kirigami.Theme.textColor
                border.width: 1
                radius: 4

                Behavior on border.color {
                    ColorAnimation { duration: Kirigami.Units.shortDuration }
                }

                Loader {
                    anchors.fill: parent
                    anchors.margins: 2
                    sourceComponent: {
                        switch (layoutCard.layoutType) {
                            case "horizontal": return horizontalLayout
                            case "vertical": return verticalLayout
                            case "grid": return gridLayout
                            default: return horizontalLayout
                        }
                    }
                }
            }

            Controls.Label {
                text: layoutCard.title
                font.bold: true
                Layout.alignment: Qt.AlignHCenter
                color: layoutCard.selected ? Kirigami.Theme.highlightColor : Kirigami.Theme.textColor
                
                Behavior on color {
                    ColorAnimation { duration: Kirigami.Units.shortDuration }
                }
            }

            Controls.Label {
                text: layoutCard.description
                Layout.alignment: Qt.AlignHCenter
                opacity: 0.7
            }
        }
    }

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
        Item {
            anchors.fill: parent

            readonly property bool isLeftRight: root.instanceCount === 3 && root.gridSubLayout === "left-right"

            readonly property int effectiveCols: {
                if (isLeftRight) return 2;
                if (root.instanceCount <= 2) return root.instanceCount;
                if (root.instanceCount === 3) {
                    return root.gridSubLayout === "grid-2x2" ? 2 : 3;
                }
                return 2;
            }
            readonly property int effectiveRows: {
                if (isLeftRight) return 2;
                if (root.instanceCount <= 2) return 1;
                if (root.instanceCount === 3) {
                    return root.gridSubLayout === "grid-2x2" ? 2 : 1;
                }
                return 2;
            }

            GridLayout {
                anchors.fill: parent
                columns: parent.effectiveCols
                rowSpacing: 2
                columnSpacing: 2
                visible: !parent.isLeftRight
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

            Row {
                anchors.fill: parent
                spacing: 2
                visible: parent.isLeftRight
                Rectangle {
                    width: parent.width * 2 / 5
                    height: parent.height
                    color: Kirigami.Theme.highlightColor
                    opacity: 0.5
                    radius: 2
                    Controls.Label {
                        anchors.centerIn: parent
                        text: "1"
                        font.bold: true
                    }
                }
                Column {
                    width: parent.width * 3 / 5 - 2
                    height: parent.height
                    spacing: 2
                    Repeater {
                        model: 2
                        Rectangle {
                            width: parent.width
                            height: (parent.parent.height - 2) / 2
                            color: Kirigami.Theme.positiveBackgroundColor
                            opacity: 0.5
                            radius: 2
                            Controls.Label {
                                anchors.centerIn: parent
                                text: (index + 2).toString()
                                font.bold: true
                            }
                        }
                    }
                }
            }
        }
    }
}
