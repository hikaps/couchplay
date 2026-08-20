// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2024 hikaps

import QtQuick
import QtQuick.Layouts
import QtQuick.Controls as Controls
import org.kde.kirigami as Kirigami
import io.github.hikaps.couchplay 1.0

import "../components/dialogs" as Dialogs

Kirigami.ScrollablePage {
    id: root
    title: i18nc("@title", "Settings")

    property var sessionRunner: null

    required property var helperClient

    property bool helperAvailable: helperClient?.available ?? false

    property var presetManager: null

    property var steamConfigManager: null

    property var settingsManager: null

    property var heroicConfigManager: null

    property var audioManager: null

    property var updateChecker: null

    PresetManager {
        id: internalPresetManager
    }

    readonly property var activePresetManager: root.presetManager ?? internalPresetManager

    readonly property bool hidePanels: settingsManager?.hidePanels ?? true
    readonly property bool killSteam: settingsManager?.killSteam ?? true
    readonly property bool restoreSession: settingsManager?.restoreSession ?? false
    readonly property string scalingMode: settingsManager?.scalingMode ?? "fit"
    readonly property string filterMode: settingsManager?.filterMode ?? "linear"
    readonly property bool borderlessWindows: settingsManager?.borderlessWindows ?? false

    actions: [
        Kirigami.Action {
            objectName: "actionResetDefaults"
            icon.name: "edit-undo"
            text: i18nc("@action:button", "Reset to Defaults")
            Accessible.role: Accessible.Button
            Accessible.name: text
            Accessible.onPressAction: triggered()
            onTriggered: resetConfirmDialog.open()
        }
    ]

    ColumnLayout {
        spacing: Kirigami.Units.mediumSpacing

        Kirigami.FormLayout {
            Layout.alignment: Qt.AlignLeft | Qt.AlignTop
            wideMode: root.width > Kirigami.Units.gridUnit * 30

            Kirigami.Separator {
                Kirigami.FormData.isSection: true
                Kirigami.FormData.label: i18nc("@title:group", "General")
            }

            Controls.CheckBox {
                id: hidePanelsCheck
                objectName: "checkHidePanels"
                Kirigami.FormData.label: i18nc("@option:check", "Hide panels during session")
                Accessible.role: Accessible.CheckBox
                Accessible.name: Kirigami.FormData.label
                checked: root.hidePanels
                onToggled: if (root.settingsManager) root.settingsManager.hidePanels = checked

                Controls.ToolTip.text: i18nc("@info:tooltip", "Automatically hide Plasma panels when a session starts")
                Controls.ToolTip.visible: hovered
                Controls.ToolTip.delay: 1000
            }

            Controls.CheckBox {
                id: killSteamOption
                objectName: "checkKillSteam"
                Kirigami.FormData.label: i18nc("@option:check", "Close Steam before starting")
                Accessible.role: Accessible.CheckBox
                Accessible.name: Kirigami.FormData.label
                checked: root.killSteam
                onToggled: if (root.settingsManager) root.settingsManager.killSteam = checked

                Controls.ToolTip.text: i18nc("@info:tooltip", "Prevents conflicts by closing existing Steam instances before session start")
                Controls.ToolTip.visible: hovered
                Controls.ToolTip.delay: 1000
            }

            Controls.CheckBox {
                id: restoreSessionCheck
                objectName: "checkRestoreSession"
                Kirigami.FormData.label: i18nc("@option:check", "Restore last session on startup")
                Accessible.role: Accessible.CheckBox
                Accessible.name: Kirigami.FormData.label
                checked: root.restoreSession
                onToggled: if (root.settingsManager) root.settingsManager.restoreSession = checked

                Controls.ToolTip.text: i18nc("@info:tooltip", "Automatically load the last used profile when CouchPlay starts")
                Controls.ToolTip.visible: hovered
                Controls.ToolTip.delay: 1000
            }
        }

        Kirigami.FormLayout {
            Layout.alignment: Qt.AlignLeft | Qt.AlignTop
            wideMode: root.width > Kirigami.Units.gridUnit * 30

            Kirigami.Separator {
                Kirigami.FormData.isSection: true
                Kirigami.FormData.label: i18nc("@title:group", "Gamescope")
            }

            Controls.ComboBox {
                id: scalingCombo
                objectName: "comboScaling"
                Kirigami.FormData.label: i18nc("@label", "Scaling mode")
                Accessible.role: Accessible.ComboBox
                Accessible.name: Kirigami.FormData.label
                model: [
                    { value: "fit", text: i18nc("@item:inlistbox", "Fit (maintain aspect ratio)") },
                    { value: "fill", text: i18nc("@item:inlistbox", "Fill (crop to fill)") },
                    { value: "stretch", text: i18nc("@item:inlistbox", "Stretch (ignore aspect ratio)") },
                    { value: "integer", text: i18nc("@item:inlistbox", "Integer (pixel-perfect)") }
                ]
                textRole: "text"
                valueRole: "value"
                currentIndex: Math.max(0, model.findIndex(item => item.value === root.scalingMode))
                onCurrentValueChanged: if (root.settingsManager) root.settingsManager.scalingMode = currentValue
            }

            Controls.ComboBox {
                id: filterCombo
                objectName: "comboFilter"
                Kirigami.FormData.label: i18nc("@label", "Upscaling filter")
                Accessible.role: Accessible.ComboBox
                Accessible.name: Kirigami.FormData.label
                model: [
                    { value: "linear", text: i18nc("@item:inlistbox", "Linear (smooth)") },
                    { value: "nearest", text: i18nc("@item:inlistbox", "Nearest (sharp/pixelated)") },
                    { value: "fsr", text: i18nc("@item:inlistbox", "FSR (AMD FidelityFX)") },
                    { value: "nis", text: i18nc("@item:inlistbox", "NIS (NVIDIA Image Scaling)") }
                ]
                textRole: "text"
                valueRole: "value"
                currentIndex: Math.max(0, model.findIndex(item => item.value === root.filterMode))
                onCurrentValueChanged: if (root.settingsManager) root.settingsManager.filterMode = currentValue
            }

            Controls.CheckBox {
                id: borderlessCheck
                objectName: "checkBorderless"
                Kirigami.FormData.label: i18nc("@option:check", "Borderless windows")
                Accessible.role: Accessible.CheckBox
                Accessible.name: Kirigami.FormData.label
                checked: root.borderlessWindows
                onToggled: if (root.settingsManager) root.settingsManager.borderlessWindows = checked

                Controls.ToolTip.text: i18nc("@info:tooltip", "Disable for resizable windows with title bars")
                Controls.ToolTip.visible: hovered
                Controls.ToolTip.delay: 1000
            }
        }
        Kirigami.FormLayout {
            Layout.alignment: Qt.AlignLeft | Qt.AlignTop
            wideMode: root.width > Kirigami.Units.gridUnit * 30

            Kirigami.Separator {
                Kirigami.FormData.isSection: true
                Kirigami.FormData.label: i18nc("@title:group", "Launch Presets")
            }

            ColumnLayout {
                Kirigami.FormData.label: i18nc("@label", "Presets")
                Layout.fillWidth: true
                spacing: Kirigami.Units.smallSpacing

                Repeater {
                    model: activePresetManager.presets

                    delegate: RowLayout {
                        spacing: Kirigami.Units.smallSpacing
                        Layout.fillWidth: true

                        Kirigami.Icon {
                            source: modelData.iconName || "application-x-executable"
                            Layout.preferredWidth: Kirigami.Units.iconSizes.small
                            Layout.preferredHeight: Kirigami.Units.iconSizes.small
                        }

                        Controls.Label {
                            text: modelData.name
                            Layout.fillWidth: true
                            elide: Text.ElideRight
                        }

                        Kirigami.Chip {
                            visible: modelData.isBuiltin
                            text: i18nc("@info", "Built-in")
                            closable: false
                            checkable: false
                        }

                        Kirigami.Chip {
                            visible: modelData.steamIntegration
                            text: i18nc("@info", "Steam")
                            icon.name: "steam"
                            closable: false
                            checkable: false
                        }

                        Controls.Button {
                            objectName: "btnEditPreset"
                            Accessible.role: Accessible.Button
                            Accessible.name: i18nc("@action:button", "Edit preset")
                            Accessible.onPressAction: clicked()
                            icon.name: "document-edit"
                            display: Controls.AbstractButton.IconOnly
                            Controls.ToolTip.text: i18nc("@info:tooltip", "Edit preset")
                            Controls.ToolTip.visible: hovered
                            Controls.ToolTip.delay: 1000
                            visible: !modelData.isBuiltin
                            onClicked: {
                                editPresetDialog.presetId = modelData.id
                                editPresetDialog.presetName = modelData.name
                                editPresetDialog.setDirectoriesFromBackend(activePresetManager.getSharedDirectories(modelData.id))
                                editPresetDialog.open()
                            }
                        }

                        Controls.Button {
                            objectName: "btnRemovePreset"
                            Accessible.role: Accessible.Button
                            Accessible.name: i18nc("@action:button", "Remove preset")
                            Accessible.onPressAction: clicked()
                            visible: !modelData.isBuiltin
                            icon.name: "edit-delete"
                            display: Controls.AbstractButton.IconOnly
                            Controls.ToolTip.text: i18nc("@info:tooltip", "Remove preset")
                            Controls.ToolTip.visible: hovered
                            Controls.ToolTip.delay: 1000
                            onClicked: {
                                deletePresetDialog.presetId = modelData.id
                                deletePresetDialog.presetName = modelData.name
                                deletePresetDialog.open()
                            }
                        }
                    }
                }

                Controls.Button {
                    objectName: "btnAddPreset"
                    Accessible.role: Accessible.Button
                    Accessible.name: text
                    Accessible.onPressAction: clicked()
                    text: i18nc("@action:button", "Add from Application...")
                    icon.name: "list-add"
                    onClicked: {
                        activePresetManager.scanApplications()
                        addPresetDialog.open()
                    }
                }
            }
        }
        Kirigami.FormLayout {
            Layout.alignment: Qt.AlignLeft | Qt.AlignTop
            wideMode: root.width > Kirigami.Units.gridUnit * 30
            visible: root.settingsManager !== null && (root.settingsManager.ignoredDevices.length > 0)

            Kirigami.Separator {
                Kirigami.FormData.isSection: true
                Kirigami.FormData.label: i18nc("@title:group", "Ignored Devices")
            }

            ColumnLayout {
                Kirigami.FormData.label: i18nc("@label", "Devices")
                Layout.fillWidth: true
                spacing: Kirigami.Units.smallSpacing

                Repeater {
                    model: root.settingsManager ? root.settingsManager.ignoredDevices : []

                    delegate: RowLayout {
                        spacing: Kirigami.Units.smallSpacing
                        Layout.fillWidth: true

                        Kirigami.Icon {
                            source: "dialog-cancel"
                            Layout.preferredWidth: Kirigami.Units.iconSizes.small
                            Layout.preferredHeight: Kirigami.Units.iconSizes.small
                            color: Kirigami.Theme.negativeTextColor
                        }

                        Controls.Label {
                            text: modelData
                            Layout.fillWidth: true
                            elide: Text.ElideMiddle
                            font.family: "monospace"
                        }

                        Controls.Button {
                            objectName: "btnUnignoreDevice"
                            Accessible.role: Accessible.Button
                            Accessible.name: i18nc("@action:button", "Stop ignoring this device")
                            Accessible.onPressAction: clicked()
                            icon.name: "list-remove"
                            display: Controls.AbstractButton.IconOnly
                            Controls.ToolTip.text: i18nc("@info:tooltip", "Stop ignoring this device")
                            Controls.ToolTip.visible: hovered
                            Controls.ToolTip.delay: 1000
                            onClicked: {
                                if (root.settingsManager) {
                                    root.settingsManager.removeIgnoredDevice(modelData)
                                }
                            }
                        }
                    }
                }
            }
        }
        Kirigami.FormLayout {
            Layout.alignment: Qt.AlignLeft | Qt.AlignTop
            wideMode: root.width > Kirigami.Units.gridUnit * 30
            visible: root.steamConfigManager !== null

            Kirigami.Separator {
                Kirigami.FormData.isSection: true
                Kirigami.FormData.label: i18nc("@title:group", "Steam")
            }

            RowLayout {
                Kirigami.FormData.label: i18nc("@label", "Detected:")
                spacing: Kirigami.Units.smallSpacing

                Kirigami.Icon {
                    source: root.steamConfigManager && root.steamConfigManager.steamDetected ? "dialog-ok-apply" : "dialog-error"
                    Layout.preferredWidth: Kirigami.Units.iconSizes.small
                    Layout.preferredHeight: Kirigami.Units.iconSizes.small
                }

                Controls.Label {
                    text: root.steamConfigManager && root.steamConfigManager.steamDetected
                          ? i18nc("@info", "Yes (%1 non-Steam shortcuts)", root.steamConfigManager.shortcutCount)
                          : i18nc("@info", "Not found")
                    color: root.steamConfigManager && root.steamConfigManager.steamDetected
                           ? Kirigami.Theme.positiveTextColor
                           : Kirigami.Theme.negativeTextColor
                }

                Controls.Button {
                    objectName: "btnReloadSteam"
                    Accessible.role: Accessible.Button
                    Accessible.name: i18nc("@action:button", "Reload")
                    Accessible.onPressAction: clicked()
                    visible: root.steamConfigManager && root.steamConfigManager.steamDetected
                    text: i18nc("@action:button", "Reload")
                    icon.name: "view-refresh"
                    onClicked: {
                        root.steamConfigManager.loadShortcuts()
                        applicationWindow().showPassiveNotification(
                            i18nc("@info", "Loaded %1 shortcuts", root.steamConfigManager.shortcutCount))
                    }
                }
            }

            Controls.CheckBox {
                id: syncShortcutsCheck
                objectName: "checkSyncSteamShortcuts"
                Kirigami.FormData.label: i18nc("@option:check", "Sync shortcuts to players:")
                Accessible.role: Accessible.CheckBox
                Accessible.name: Kirigami.FormData.label
                checked: root.steamConfigManager ? root.steamConfigManager.syncShortcutsEnabled : false
                onToggled: {
                    if (root.steamConfigManager) {
                        root.steamConfigManager.syncShortcutsEnabled = checked
                    }
                }

                Controls.ToolTip.text: i18nc("@info:tooltip", "Copy your non-Steam game shortcuts (Heroic, Lutris, etc. added to Steam) to gaming users at session start.")
                Controls.ToolTip.visible: hovered
                Controls.ToolTip.delay: 1000
            }

            Controls.CheckBox {
                id: shareLibraryCheck
                Kirigami.FormData.label: i18nc("@option:check", "Share game library with players:")
                checked: root.steamConfigManager ? root.steamConfigManager.shareLibraryEnabled : false
                onToggled: {
                    if (root.steamConfigManager) {
                        root.steamConfigManager.shareLibraryEnabled = checked
                    }
                }

                Controls.ToolTip.text: i18nc("@info:tooltip", "Share your downloaded Steam games with gaming users via bind mounts. Each gaming user must own the game on their Steam account to play it. Disk space is saved because games are only downloaded once.")
                Controls.ToolTip.visible: hovered
                Controls.ToolTip.delay: 1000
            }
        }

        Kirigami.FormLayout {
            Layout.alignment: Qt.AlignLeft | Qt.AlignTop
            wideMode: root.width > Kirigami.Units.gridUnit * 30
            visible: root.heroicConfigManager !== null

            Kirigami.Separator {
                Kirigami.FormData.isSection: true
                Kirigami.FormData.label: i18nc("@title:group", "Heroic Games Launcher")
            }

            RowLayout {
                Kirigami.FormData.label: i18nc("@label", "Detected:")
                spacing: Kirigami.Units.smallSpacing

                Kirigami.Icon {
                    source: root.heroicConfigManager && root.heroicConfigManager.heroicDetected ? "dialog-ok-apply" : "dialog-error"
                    Layout.preferredWidth: Kirigami.Units.iconSizes.small
                    Layout.preferredHeight: Kirigami.Units.iconSizes.small
                }

                Controls.Label {
                    text: root.heroicConfigManager && root.heroicConfigManager.heroicDetected
                          ? i18nc("@info", "Yes (%1 games)", root.heroicConfigManager.gameCount)
                          : i18nc("@info", "Not found")
                    color: root.heroicConfigManager && root.heroicConfigManager.heroicDetected
                           ? Kirigami.Theme.positiveTextColor
                           : Kirigami.Theme.negativeTextColor
                }

                Kirigami.Chip {
                    visible: root.heroicConfigManager && root.heroicConfigManager.heroicDetected && root.heroicConfigManager.isFlatpak
                    text: i18nc("@info", "Flatpak")
                    closable: false
                    checkable: false
                }

                Controls.Button {
                    objectName: "btnReloadHeroic"
                    Accessible.role: Accessible.Button
                    Accessible.name: i18nc("@action:button", "Reload")
                    Accessible.onPressAction: clicked()
                    visible: root.heroicConfigManager && root.heroicConfigManager.heroicDetected
                    text: i18nc("@action:button", "Reload")
                    icon.name: "view-refresh"
                    onClicked: {
                        root.heroicConfigManager.loadGames()
                        applicationWindow().showPassiveNotification(
                            i18nc("@info", "Loaded %1 games from Heroic", root.heroicConfigManager.gameCount))
                    }
                }
            }

            Controls.Label {
                Kirigami.FormData.label: i18nc("@label", "Installation:")
                visible: root.heroicConfigManager && root.heroicConfigManager.heroicDetected
                text: root.heroicConfigManager && root.heroicConfigManager.isFlatpak
                      ? i18nc("@info", "Flatpak (com.heroicgameslauncher.hgl)")
                      : i18nc("@info", "Native")
                opacity: 0.7
            }

            Controls.CheckBox {
                id: heroicSyncShortcutsCheck
                objectName: "checkSyncHeroicShortcuts"
                Kirigami.FormData.label: i18nc("@option:check", "Sync shortcuts to players:")
                Accessible.role: Accessible.CheckBox
                Accessible.name: Kirigami.FormData.label
                checked: root.heroicConfigManager ? root.heroicConfigManager.syncShortcutsEnabled : false
                onToggled: {
                    if (root.heroicConfigManager) {
                        root.heroicConfigManager.syncShortcutsEnabled = checked
                    }
                }

                Controls.ToolTip.text: i18nc("@info:tooltip", "Copy Heroic game shortcuts (.desktop files) to gaming users at session start. Game directories are shared using filesystem ACLs.")
                Controls.ToolTip.visible: hovered
                Controls.ToolTip.delay: 1000
            }
        }

        Kirigami.InlineMessage {
            Layout.fillWidth: true
            visible: (root.steamConfigManager !== null && root.steamConfigManager.steamDetected) 
                      || (root.heroicConfigManager !== null && root.heroicConfigManager.heroicDetected)
            text: i18nc("@info", "When sync is enabled, shortcuts are copied to gaming users at session start. When library sharing is enabled, downloaded games are shared via bind mounts. Access to game directories is granted using filesystem ACLs.")
            type: Kirigami.MessageType.Information
        }


        Kirigami.FormLayout {
            Layout.alignment: Qt.AlignLeft | Qt.AlignTop
            wideMode: root.width > Kirigami.Units.gridUnit * 30

            Kirigami.Separator {
                Kirigami.FormData.isSection: true
                Kirigami.FormData.label: i18nc("@title:group", "Audio")
            }

            RowLayout {
                Kirigami.FormData.label: i18nc("@label", "Detected")
                spacing: Kirigami.Units.smallSpacing

                Kirigami.Icon {
                    source: root.audioManager && root.audioManager.audioServer !== "" ? "dialog-ok-apply" : "dialog-error"
                    color: root.audioManager && root.audioManager.audioServer !== "" ? Kirigami.Theme.positiveTextColor : Kirigami.Theme.negativeTextColor
                    Layout.preferredWidth: Kirigami.Units.iconSizes.small
                    Layout.preferredHeight: Kirigami.Units.iconSizes.small
                }

                Controls.Label {
                    text: root.audioManager && root.audioManager.audioServer !== ""
                          ? root.audioManager.audioServer
                          : i18nc("@info", "Not detected")
                    color: root.audioManager && root.audioManager.audioServer !== ""
                           ? Kirigami.Theme.positiveTextColor
                           : Kirigami.Theme.negativeTextColor
                }
            }

            Controls.Label {
                Kirigami.FormData.label: i18nc("@label", "Multi-user routing")
                text: {
                    if (!root.audioManager || root.audioManager.audioServer === "") {
                        return i18nc("@info", "Not available — no audio server detected")
                    }
                    if (root.audioManager.multiUserConfigured) {
                        return i18nc("@info", "Ready — socket ACLs configured")
                    }
                    return i18nc("@info", "Will be configured automatically at session start")
                }
                wrapMode: Text.WordWrap
                opacity: 0.7
                color: {
                    if (!root.audioManager || root.audioManager.audioServer === "") {
                        return Kirigami.Theme.negativeTextColor
                    }
                    if (root.audioManager.multiUserConfigured) {
                        return Kirigami.Theme.positiveTextColor
                    }
                    return Kirigami.Theme.textColor
                }
            }
        }

        Kirigami.FormLayout {
            Layout.alignment: Qt.AlignLeft | Qt.AlignTop
            wideMode: root.width > Kirigami.Units.gridUnit * 30

            Kirigami.Separator {
                Kirigami.FormData.isSection: true
                Kirigami.FormData.label: i18nc("@title:group", "Privileged Helper")
            }

            RowLayout {
                Kirigami.FormData.label: i18nc("@label", "Status")
                spacing: Kirigami.Units.smallSpacing

                Kirigami.Icon {
                    source: root.helperAvailable ? "dialog-ok-apply" : "dialog-error"
                    Layout.preferredWidth: Kirigami.Units.iconSizes.small
                    Layout.preferredHeight: Kirigami.Units.iconSizes.small
                }

                Controls.Label {
                    text: root.helperAvailable ? i18nc("@info", "Connected") : i18nc("@info", "Not installed")
                    color: root.helperAvailable ? Kirigami.Theme.positiveTextColor : Kirigami.Theme.negativeTextColor
                }
            }

            Controls.Button {
                objectName: "btnInstallHelper"
                Accessible.role: Accessible.Button
                Accessible.name: text
                Accessible.onPressAction: clicked()
                visible: !root.helperAvailable
                Kirigami.FormData.label: " "
                text: i18nc("@action:button", "Install Helper...")
                icon.name: "run-install"
                onClicked: installHelperDialog.open()
            }
        }

        Kirigami.FormLayout {
            Layout.alignment: Qt.AlignLeft | Qt.AlignTop
            wideMode: root.width > Kirigami.Units.gridUnit * 30

            Kirigami.Separator {
                Kirigami.FormData.isSection: true
                Kirigami.FormData.label: i18nc("@title:group", "Keyboard Shortcuts")
            }

            RowLayout {
                Kirigami.FormData.label: i18nc("@label", "Stop session")
                spacing: Kirigami.Units.smallSpacing

                Controls.Label {
                    text: "Meta+Shift+Escape"
                    font.family: "monospace"
                }

                Controls.Button {
                    objectName: "btnConfigureShortcuts"
                    Accessible.role: Accessible.Button
                    Accessible.name: text
                    Accessible.onPressAction: clicked()
                    text: i18nc("@action:button", "Configure...")
                    icon.name: "configure-shortcuts"
                    onClicked: {
                        Qt.openUrlExternally("systemsettings://kcm_keys?search=couchplay")
                    }
                }
            }

            Controls.Label {
                Kirigami.FormData.label: i18nc("@label", "Tip")
                text: i18nc("@info", "Use Alt+Tab to switch away from gamescope windows")
                wrapMode: Text.WordWrap
                opacity: 0.7
            }
        }

        Kirigami.InlineMessage {
            Layout.fillWidth: true
            text: i18nc("@info", "The CouchPlay Helper is a privileged system service required for creating users and managing device permissions. It uses PolicyKit for secure authorization.")
            type: Kirigami.MessageType.Information
            visible: !root.helperAvailable

            actions: [
                Kirigami.Action {
                    objectName: "actionLearnMore"
                    Accessible.role: Accessible.Button
                    Accessible.name: i18nc("@action:button", "Learn More")
                    Accessible.onPressAction: triggered()
                    icon.name: "help-about"
                    text: i18nc("@action:button", "Learn More")
                    onTriggered: Qt.openUrlExternally("https://github.com/hikaps/couchplay#helper-setup")
                }
            ]
        }

        Kirigami.FormLayout {
            Layout.alignment: Qt.AlignLeft | Qt.AlignTop
            wideMode: root.width > Kirigami.Units.gridUnit * 30

            Kirigami.Separator {
                Kirigami.FormData.isSection: true
                Kirigami.FormData.label: i18nc("@title:group", "Software Update")
            }

            RowLayout {
                Kirigami.FormData.label: i18nc("@label", "Status")
                spacing: Kirigami.Units.smallSpacing

                Kirigami.Icon {
                    source: {
                        let s = root.updateChecker ? root.updateChecker.state : -1
                        if (s === UpdateChecker.Checking) return "view-refresh"
                        if (s === UpdateChecker.UpToDate) return "dialog-ok-apply"
                        if (s === UpdateChecker.Error) return "dialog-error"
                        return "dialog-information"
                    }
                    color: {
                        let s = root.updateChecker ? root.updateChecker.state : -1
                        if (s === UpdateChecker.UpToDate) return Kirigami.Theme.positiveTextColor
                        if (s === UpdateChecker.Error) return Kirigami.Theme.negativeTextColor
                        return Kirigami.Theme.textColor
                    }
                    Layout.preferredWidth: Kirigami.Units.iconSizes.small
                    Layout.preferredHeight: Kirigami.Units.iconSizes.small
                }

                Controls.Label {
                    objectName: "labelUpdateStatus"
                    text: {
                        if (!root.updateChecker || root.updateChecker.state === UpdateChecker.Idle) {
                            return i18nc("@info", "Not checked yet")
                        }
                        if (root.updateChecker.state === UpdateChecker.Checking) {
                            return i18nc("@info", "Checking…")
                        }
                        if (root.updateChecker.state === UpdateChecker.UpToDate) {
                            return i18nc("@info", "Up to date (%1)", root.updateChecker.currentVersion)
                        }
                        if (root.updateChecker.state === UpdateChecker.UpdateAvailable) {
                            return i18nc("@info", "Update available: %1", root.updateChecker.latestVersion)
                        }
                        return i18nc("@info", "Could not check for updates")
                    }
                    color: {
                        if (!root.updateChecker) return Kirigami.Theme.textColor
                        if (root.updateChecker.state === UpdateChecker.UpToDate) return Kirigami.Theme.positiveTextColor
                        if (root.updateChecker.state === UpdateChecker.Error) return Kirigami.Theme.negativeTextColor
                        return Kirigami.Theme.textColor
                    }
                }
            }

            Controls.Label {
                Kirigami.FormData.label: i18nc("@label", "Latest release")
                text: root.updateChecker && root.updateChecker.latestVersion !== "" ? root.updateChecker.latestVersion : "—"
            }

            Controls.Button {
                objectName: "btnCheckUpdates"
                Kirigami.FormData.label: " "
                Accessible.role: Accessible.Button
                Accessible.name: text
                Accessible.onPressAction: clicked()
                text: i18nc("@action:button", "Check Now")
                icon.name: "view-refresh"
                enabled: root.updateChecker && root.updateChecker.state !== UpdateChecker.Checking
                onClicked: root.updateChecker.checkForUpdates()
            }

            Controls.CheckBox {
                objectName: "checkUpdateAutomatically"
                Kirigami.FormData.label: i18nc("@option:check", "Check for updates automatically")
                Accessible.role: Accessible.CheckBox
                Accessible.name: Kirigami.FormData.label
                checked: root.settingsManager ? root.settingsManager.checkForUpdatesAutomatically : true
                onToggled: if (root.settingsManager) root.settingsManager.checkForUpdatesAutomatically = checked

                Controls.ToolTip.text: i18nc("@info:tooltip", "Checks GitHub for a new release when CouchPlay starts. No data is sent beyond the request itself.")
                Controls.ToolTip.visible: hovered
                Controls.ToolTip.delay: 1000
            }
        }

        Kirigami.InlineMessage {
            Layout.fillWidth: true
            visible: root.updateChecker !== null && root.updateChecker.updateAvailable
            type: Kirigami.MessageType.Information
            text: {
                if (root.updateChecker.installMethod === "flatpak") {
                    return i18nc("@info", "Re-run the install command from the README to download the new version. Your settings and profiles are preserved.")
                }
                if (root.updateChecker.installMethod === "steamos") {
                    return i18nc("@info", "Re-run the install command from the README to update the app and its system extension. Stop any running session first.")
                }
                return i18nc("@info", "Re-run the install command from the README (add --tarball to keep the native binary), or download from the releases page.")
            }

            actions: [
                Kirigami.Action {
                    objectName: "actionOpenReleases"
                    Accessible.role: Accessible.Button
                    Accessible.name: i18nc("@action:button", "Open Releases Page")
                    Accessible.onPressAction: triggered()
                    icon.name: "link"
                    text: i18nc("@action:button", "Open Releases Page")
                    onTriggered: Qt.openUrlExternally(root.updateChecker.releasesUrl)
                }
            ]
        }

        Kirigami.FormLayout {
            Layout.alignment: Qt.AlignLeft | Qt.AlignTop
            wideMode: root.width > Kirigami.Units.gridUnit * 30

            Kirigami.Separator {
                Kirigami.FormData.isSection: true
                Kirigami.FormData.label: i18nc("@title:group", "About")
            }

            Controls.Label {
                Kirigami.FormData.label: i18nc("@label", "Application")
                text: "CouchPlay"
            }

            Controls.Label {
                Kirigami.FormData.label: i18nc("@label", "Version")
                text: Qt.application.version
            }

            Controls.Label {
                Kirigami.FormData.label: i18nc("@label", "License")
                text: "GPL-3.0-or-later"
            }

            RowLayout {
                Kirigami.FormData.label: i18nc("@label", "Source")
                spacing: Kirigami.Units.smallSpacing

                Controls.Label {
                    text: "github.com/hikaps/couchplay"
                    color: Kirigami.Theme.linkColor

                    MouseArea {
                        anchors.fill: parent
                        cursorShape: Qt.PointingHandCursor
                        onClicked: Qt.openUrlExternally("https://github.com/hikaps/couchplay")
                    }
                }

                Kirigami.Icon {
                    source: "link"
                    Layout.preferredWidth: Kirigami.Units.iconSizes.small
                    Layout.preferredHeight: Kirigami.Units.iconSizes.small
                }
            }
        }

        Kirigami.AbstractCard {
            Layout.fillWidth: true
            Layout.bottomMargin: Kirigami.Units.largeSpacing

            header: Kirigami.Heading {
                text: i18nc("@title", "System Requirements")
                level: 3
                padding: Kirigami.Units.largeSpacing
            }

            contentItem: GridLayout {
                columns: 2
                columnSpacing: Kirigami.Units.largeSpacing
                rowSpacing: Kirigami.Units.smallSpacing

                RowLayout {
                    spacing: Kirigami.Units.smallSpacing
                    Kirigami.Icon {
                        source: "dialog-ok-apply"
                        color: Kirigami.Theme.positiveTextColor
                        Layout.preferredWidth: Kirigami.Units.iconSizes.small
                        Layout.preferredHeight: Kirigami.Units.iconSizes.small
                    }
                    Controls.Label {
                        text: "Gamescope"
                        font.bold: true
                    }
                }
                Controls.Label {
                    text: i18nc("@info", "Required for multi-instance compositing")
                    opacity: 0.7
                    Layout.fillWidth: true
                }

                RowLayout {
                    spacing: Kirigami.Units.smallSpacing
                    Kirigami.Icon {
                        source: "dialog-ok-apply"
                        color: Kirigami.Theme.positiveTextColor
                        Layout.preferredWidth: Kirigami.Units.iconSizes.small
                        Layout.preferredHeight: Kirigami.Units.iconSizes.small
                    }
                    Controls.Label {
                        text: "PipeWire"
                        font.bold: true
                    }
                }
                Controls.Label {
                    text: i18nc("@info", "Required for multi-user audio routing")
                    opacity: 0.7
                    Layout.fillWidth: true
                }

                RowLayout {
                    spacing: Kirigami.Units.smallSpacing
                    Kirigami.Icon {
                        source: "dialog-information"
                        Layout.preferredWidth: Kirigami.Units.iconSizes.small
                        Layout.preferredHeight: Kirigami.Units.iconSizes.small
                    }
                    Controls.Label {
                        text: "KDE Plasma"
                    }
                }
                Controls.Label {
                    text: i18nc("@info", "Recommended for best integration")
                    opacity: 0.7
                    Layout.fillWidth: true
                }

                RowLayout {
                    spacing: Kirigami.Units.smallSpacing
                    Kirigami.Icon {
                        source: "dialog-information"
                        Layout.preferredWidth: Kirigami.Units.iconSizes.small
                        Layout.preferredHeight: Kirigami.Units.iconSizes.small
                    }
                    Controls.Label {
                        text: "Steam"
                    }
                }
                Controls.Label {
                    text: i18nc("@info", "Optional, for Steam games and shortcuts")
                    opacity: 0.7
                    Layout.fillWidth: true
                }
            }
        }
    }

    Dialogs.ResetSettingsDialog {
        id: resetConfirmDialog
        settingsManager: root.settingsManager
    }

    Dialogs.InstallHelperDialog {
        id: installHelperDialog
    }

    Dialogs.EditPresetDialog {
        id: editPresetDialog
        presetManager: activePresetManager
        steamConfigManager: root.steamConfigManager
    }

    Dialogs.DeletePresetDialog {
        id: deletePresetDialog
        presetManager: activePresetManager
    }

    Dialogs.AddPresetDialog {
        id: addPresetDialog
        presetManager: activePresetManager
    }
}
