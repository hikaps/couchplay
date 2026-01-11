// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2024 hikaps

import QtQuick
import QtQuick.Layouts
import QtQuick.Controls as Controls
import org.kde.kirigami as Kirigami
import io.github.hikaps.couchplay 1.0

Kirigami.ScrollablePage {
    id: root
    title: i18nc("@title", "Settings")

    // Reference to session runner for settings that affect it
    property var sessionRunner: null

    // Reference to helper client for status display
    required property var helperClient

    // Computed property to avoid repeating condition
    property bool helperAvailable: helperClient?.available ?? false

    // Reference to preset manager (optional, will use internal if not provided)
    property var presetManager: null

    // Internal preset manager if not provided externally
    PresetManager {
        id: internalPresetManager
    }

    // Use provided preset manager or internal one
    readonly property var activePresetManager: root.presetManager ?? internalPresetManager

    // Settings values (would be persisted via QSettings in production)
    property bool hidePanels: true
    property bool killSteam: true
    property bool restoreSession: false
    property string scalingMode: "fit"
    property string filterMode: "linear"
    property bool steamIntegration: true
    property bool borderlessWindows: false  // Default: show window decorations for resizing

    actions: [
        Kirigami.Action {
            icon.name: "document-save"
            text: i18nc("@action:button", "Save Settings")
            onTriggered: {
                // TODO: Save to QSettings
                applicationWindow().showPassiveNotification(
                    i18nc("@info", "Settings saved"))
            }
        },
        Kirigami.Action {
            icon.name: "edit-undo"
            text: i18nc("@action:button", "Reset to Defaults")
            onTriggered: resetConfirmDialog.open()
        }
    ]

    ColumnLayout {
        spacing: Kirigami.Units.largeSpacing

        // General Settings Section
        Kirigami.FormLayout {
            Layout.fillWidth: true
            wideMode: false

            Kirigami.Separator {
                Kirigami.FormData.isSection: true
                Kirigami.FormData.label: i18nc("@title:group", "General")
            }

            Controls.CheckBox {
                id: hidePanelsCheck
                Kirigami.FormData.label: i18nc("@option:check", "Hide KDE panels during session:")
                checked: root.hidePanels
                onToggled: root.hidePanels = checked

                Controls.ToolTip.text: i18nc("@info:tooltip", "Automatically hide Plasma panels when a session starts")
                Controls.ToolTip.visible: hovered
                Controls.ToolTip.delay: 1000
            }

            Controls.CheckBox {
                id: killSteamCheck
                Kirigami.FormData.label: i18nc("@option:check", "Kill Steam before starting:")
                checked: root.killSteam
                onToggled: root.killSteam = checked

                Controls.ToolTip.text: i18nc("@info:tooltip", "Close existing Steam instances before starting a session to prevent conflicts")
                Controls.ToolTip.visible: hovered
                Controls.ToolTip.delay: 1000
            }

            Controls.CheckBox {
                id: restoreSessionCheck
                Kirigami.FormData.label: i18nc("@option:check", "Restore last session on startup:")
                checked: root.restoreSession
                onToggled: root.restoreSession = checked

                Controls.ToolTip.text: i18nc("@info:tooltip", "Automatically load the last used profile when CouchPlay starts")
                Controls.ToolTip.visible: hovered
                Controls.ToolTip.delay: 1000
            }
        }

        // Gamescope Settings Section
        Kirigami.FormLayout {
            Layout.fillWidth: true
            wideMode: false

            Kirigami.Separator {
                Kirigami.FormData.isSection: true
                Kirigami.FormData.label: i18nc("@title:group", "Gamescope")
            }

            Controls.ComboBox {
                id: scalingCombo
                Kirigami.FormData.label: i18nc("@label", "Default scaling mode:")
                model: [
                    { value: "fit", text: i18nc("@item:inlistbox", "Fit (maintain aspect ratio)") },
                    { value: "fill", text: i18nc("@item:inlistbox", "Fill (crop to fill)") },
                    { value: "stretch", text: i18nc("@item:inlistbox", "Stretch (ignore aspect ratio)") },
                    { value: "integer", text: i18nc("@item:inlistbox", "Integer (pixel-perfect)") }
                ]
                textRole: "text"
                valueRole: "value"
                currentIndex: Math.max(0, model.findIndex(item => item.value === root.scalingMode))
                onCurrentValueChanged: root.scalingMode = currentValue
            }

            Controls.ComboBox {
                id: filterCombo
                Kirigami.FormData.label: i18nc("@label", "Default upscaling filter:")
                model: [
                    { value: "linear", text: i18nc("@item:inlistbox", "Linear (smooth)") },
                    { value: "nearest", text: i18nc("@item:inlistbox", "Nearest (sharp/pixelated)") },
                    { value: "fsr", text: i18nc("@item:inlistbox", "FSR (AMD FidelityFX)") },
                    { value: "nis", text: i18nc("@item:inlistbox", "NIS (NVIDIA Image Scaling)") }
                ]
                textRole: "text"
                valueRole: "value"
                currentIndex: Math.max(0, model.findIndex(item => item.value === root.filterMode))
                onCurrentValueChanged: root.filterMode = currentValue
            }

            Controls.CheckBox {
                id: steamIntegrationCheck
                Kirigami.FormData.label: i18nc("@option:check", "Enable Steam integration (-e):")
                checked: root.steamIntegration
                onToggled: root.steamIntegration = checked

                Controls.ToolTip.text: i18nc("@info:tooltip", "Pass -e flag to gamescope for better Steam Deck/Big Picture integration")
                Controls.ToolTip.visible: hovered
                Controls.ToolTip.delay: 1000
            }

            Controls.CheckBox {
                id: borderlessCheck
                Kirigami.FormData.label: i18nc("@option:check", "Borderless windows:")
                checked: root.sessionRunner ? root.sessionRunner.borderlessWindows : root.borderlessWindows
                onToggled: {
                    root.borderlessWindows = checked
                    if (root.sessionRunner) {
                        root.sessionRunner.borderlessWindows = checked
                    }
                }

                Controls.ToolTip.text: i18nc("@info:tooltip", "Use borderless windows without decorations. Disable for resizable windows with title bars.")
                Controls.ToolTip.visible: hovered
                Controls.ToolTip.delay: 1000
            }
        }

        // Launch Presets Section
        Kirigami.FormLayout {
            Layout.fillWidth: true
            wideMode: false

            Kirigami.Separator {
                Kirigami.FormData.isSection: true
                Kirigami.FormData.label: i18nc("@title:group", "Launch Presets")
            }

            Controls.Label {
                Kirigami.FormData.label: i18nc("@label", "Available presets:")
                text: i18nc("@info", "%1 presets configured", activePresetManager.presets.length)
                opacity: 0.7
            }

            // List of current presets
            Repeater {
                model: activePresetManager.presets

                delegate: RowLayout {
                    Kirigami.FormData.label: index === 0 ? " " : ""
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
                        text: i18nc("@info", "Builtin")
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

            // Add preset button
            Controls.Button {
                Kirigami.FormData.label: " "
                text: i18nc("@action:button", "Add from Application...")
                icon.name: "list-add"
                onClicked: {
                    activePresetManager.scanApplications()
                    addPresetDialog.open()
                }
            }
        }

        // Audio Settings Section
        Kirigami.FormLayout {
            Layout.fillWidth: true
            wideMode: false

            Kirigami.Separator {
                Kirigami.FormData.isSection: true
                Kirigami.FormData.label: i18nc("@title:group", "Audio")
            }

            RowLayout {
                Kirigami.FormData.label: i18nc("@label", "Audio server:")
                spacing: Kirigami.Units.smallSpacing

                Kirigami.Icon {
                    source: "audio-volume-high"
                    Layout.preferredWidth: Kirigami.Units.iconSizes.small
                    Layout.preferredHeight: Kirigami.Units.iconSizes.small
                }

                Controls.Label {
                    text: "PipeWire"
                }

                Kirigami.Chip {
                    text: i18nc("@info", "Detected")
                    icon.name: "dialog-ok-apply"
                    closable: false
                    checkable: false
                }
            }

            Controls.Label {
                Kirigami.FormData.label: i18nc("@label", "Multi-user audio:")
                text: i18nc("@info", "PipeWire TCP forwarding will be used for secondary users")
                wrapMode: Text.WordWrap
                opacity: 0.7
            }
        }

        // Helper Service Section
        Kirigami.FormLayout {
            Layout.fillWidth: true
            wideMode: false

            Kirigami.Separator {
                Kirigami.FormData.isSection: true
                Kirigami.FormData.label: i18nc("@title:group", "Helper Service")
            }

            RowLayout {
                Kirigami.FormData.label: i18nc("@label", "Status:")
                spacing: Kirigami.Units.smallSpacing

                Kirigami.Icon {
                    source: root.helperAvailable ? "dialog-ok-apply" : "dialog-error"
                    Layout.preferredWidth: Kirigami.Units.iconSizes.small
                    Layout.preferredHeight: Kirigami.Units.iconSizes.small
                }

                Controls.Label {
                    text: root.helperAvailable ? i18nc("@info", "Connected") : i18nc("@info", "Not available")
                    color: root.helperAvailable ? Kirigami.Theme.positiveTextColor : Kirigami.Theme.negativeTextColor
                }
            }

            Controls.Button {
                visible: !root.helperAvailable
                Kirigami.FormData.label: " "
                text: i18nc("@action:button", "Install Helper")
                icon.name: "run-install"
                onClicked: installHelperDialog.open()
            }
        }

        // Keyboard Shortcuts Section
        Kirigami.FormLayout {
            Layout.fillWidth: true
            wideMode: false

            Kirigami.Separator {
                Kirigami.FormData.isSection: true
                Kirigami.FormData.label: i18nc("@title:group", "Keyboard Shortcuts")
            }

            RowLayout {
                Kirigami.FormData.label: i18nc("@label", "Stop session:")
                spacing: Kirigami.Units.smallSpacing

                Controls.Label {
                    text: "Meta+Shift+Escape"
                    font.family: "monospace"
                }

                Controls.Button {
                    text: i18nc("@action:button", "Configure...")
                    icon.name: "configure-shortcuts"
                    onClicked: {
                        // Open KDE Global Shortcuts settings for couchplay
                        Qt.openUrlExternally("systemsettings://kcm_keys?search=couchplay")
                    }
                }
            }

            Controls.Label {
                Kirigami.FormData.label: " "
                text: i18nc("@info", "You can also use Alt+Tab to switch away from gamescope windows.")
                wrapMode: Text.WordWrap
                opacity: 0.7
                Layout.fillWidth: true
            }
        }

        // Helper info card
        Kirigami.InlineMessage {
            Layout.fillWidth: true
            text: i18nc("@info", "The CouchPlay Helper is a privileged system service required for creating users and managing device permissions. It uses PolicyKit for secure authorization.")
            type: Kirigami.MessageType.Information
            visible: !root.helperAvailable
            
            actions: [
                Kirigami.Action {
                    icon.name: "help-about"
                    text: i18nc("@action:button", "Learn More")
                    onTriggered: Qt.openUrlExternally("https://github.com/hikaps/couchplay#helper-setup")
                }
            ]
        }

        // About Section
        Kirigami.FormLayout {
            Layout.fillWidth: true
            Layout.topMargin: Kirigami.Units.largeSpacing
            wideMode: false

            Kirigami.Separator {
                Kirigami.FormData.isSection: true
                Kirigami.FormData.label: i18nc("@title:group", "About")
            }

            Controls.Label {
                Kirigami.FormData.label: i18nc("@label", "Application:")
                text: "CouchPlay"
            }

            Controls.Label {
                Kirigami.FormData.label: i18nc("@label", "Version:")
                text: "0.1.0-dev"
            }

            Controls.Label {
                Kirigami.FormData.label: i18nc("@label", "License:")
                text: "GPL-3.0-or-later"
            }

            RowLayout {
                Kirigami.FormData.label: i18nc("@label", "Source code:")
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

        // Dependencies info
        Kirigami.Card {
            Layout.fillWidth: true
            Layout.topMargin: Kirigami.Units.largeSpacing

            header: Kirigami.Heading {
                text: i18nc("@title", "System Requirements")
                level: 3
                padding: Kirigami.Units.largeSpacing
            }

            contentItem: GridLayout {
                columns: 2
                columnSpacing: Kirigami.Units.largeSpacing
                rowSpacing: Kirigami.Units.smallSpacing

                // Gamescope
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
                    text: i18nc("@info", "Required for window compositing")
                    opacity: 0.7
                }

                // Steam
                RowLayout {
                    spacing: Kirigami.Units.smallSpacing
                    Kirigami.Icon {
                        source: "dialog-ok-apply"
                        color: Kirigami.Theme.positiveTextColor
                        Layout.preferredWidth: Kirigami.Units.iconSizes.small
                        Layout.preferredHeight: Kirigami.Units.iconSizes.small
                    }
                    Controls.Label {
                        text: "Steam"
                        font.bold: true
                    }
                }
                Controls.Label {
                    text: i18nc("@info", "For launching games")
                    opacity: 0.7
                }

                // PipeWire
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
                    text: i18nc("@info", "For multi-user audio")
                    opacity: 0.7
                }

                // KDE Plasma
                RowLayout {
                    spacing: Kirigami.Units.smallSpacing
                    Kirigami.Icon {
                        source: "dialog-ok-apply"
                        color: Kirigami.Theme.positiveTextColor
                        Layout.preferredWidth: Kirigami.Units.iconSizes.small
                        Layout.preferredHeight: Kirigami.Units.iconSizes.small
                    }
                    Controls.Label {
                        text: "KDE Plasma"
                        font.bold: true
                    }
                }
                Controls.Label {
                    text: i18nc("@info", "Recommended desktop environment")
                    opacity: 0.7
                }
            }
        }

        // Spacer
        Item {
            Layout.fillHeight: true
        }
    }

    // Reset confirmation dialog
    Kirigami.PromptDialog {
        id: resetConfirmDialog
        title: i18nc("@title:dialog", "Reset Settings")
        subtitle: i18nc("@info", "Reset all settings to their default values?")
        standardButtons: Kirigami.Dialog.Yes | Kirigami.Dialog.Cancel

        onAccepted: {
            root.hidePanels = true
            root.killSteam = true
            root.restoreSession = false
            root.scalingMode = "fit"
            root.filterMode = "linear"
            root.steamIntegration = true
            root.borderlessWindows = false
            applicationWindow().showPassiveNotification(
                i18nc("@info", "Settings reset to defaults"))
        }
    }

    // Install helper dialog
    Kirigami.Dialog {
        id: installHelperDialog
        title: i18nc("@title:dialog", "Install Helper Service")
        standardButtons: Kirigami.Dialog.Close
        preferredWidth: Kirigami.Units.gridUnit * 25

        ColumnLayout {
            spacing: Kirigami.Units.largeSpacing

            Controls.Label {
                text: i18nc("@info", "To install the CouchPlay Helper, run the following command in a terminal:")
                wrapMode: Text.WordWrap
                Layout.fillWidth: true
            }

            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: commandLabel.implicitHeight + Kirigami.Units.largeSpacing * 2
                color: Kirigami.Theme.alternateBackgroundColor
                radius: Kirigami.Units.smallSpacing

                Controls.Label {
                    id: commandLabel
                    anchors.centerIn: parent
                    anchors.margins: Kirigami.Units.largeSpacing
                    text: "sudo /usr/share/couchplay/install-helper.sh"
                    font.family: "monospace"
                }
            }

            Kirigami.InlineMessage {
                Layout.fillWidth: true
                text: i18nc("@info", "This will install a D-Bus system service and PolicyKit rules for secure privilege escalation.")
                type: Kirigami.MessageType.Information
                visible: true
            }
        }
    }

    // Delete preset confirmation dialog
    Kirigami.PromptDialog {
        id: deletePresetDialog
        title: i18nc("@title:dialog", "Remove Preset")
        subtitle: i18nc("@info", "Remove the preset \"%1\"?", deletePresetDialog.presetName)
        standardButtons: Kirigami.Dialog.Yes | Kirigami.Dialog.Cancel

        property string presetId: ""
        property string presetName: ""

        onAccepted: {
            if (activePresetManager.removeCustomPreset(presetId)) {
                applicationWindow().showPassiveNotification(
                    i18nc("@info", "Preset removed"))
            } else {
                applicationWindow().showPassiveNotification(
                    i18nc("@info", "Failed to remove preset"), "long")
            }
        }
    }

    // Add preset from application dialog
    Kirigami.Dialog {
        id: addPresetDialog
        title: i18nc("@title:dialog", "Add Preset from Application")
        standardButtons: Kirigami.Dialog.Close
        preferredWidth: Kirigami.Units.gridUnit * 30
        preferredHeight: Kirigami.Units.gridUnit * 25

        ColumnLayout {
            spacing: Kirigami.Units.largeSpacing

            Controls.TextField {
                id: appSearchField
                Layout.fillWidth: true
                placeholderText: i18nc("@info:placeholder", "Search applications...")
                onTextChanged: appListView.filterText = text.toLowerCase()
            }

            Controls.ScrollView {
                Layout.fillWidth: true
                Layout.fillHeight: true
                Layout.preferredHeight: Kirigami.Units.gridUnit * 15

                ListView {
                    id: appListView
                    clip: true

                    property string filterText: ""

                    model: {
                        if (filterText === "") {
                            return activePresetManager.availableApplications
                        }
                        return activePresetManager.availableApplications.filter(function(app) {
                            return app.name.toLowerCase().includes(filterText)
                        })
                    }

                    delegate: Controls.ItemDelegate {
                        width: appListView.width
                        highlighted: ListView.isCurrentItem

                        contentItem: RowLayout {
                            spacing: Kirigami.Units.smallSpacing

                            Kirigami.Icon {
                                source: modelData.iconName || "application-x-executable"
                                Layout.preferredWidth: Kirigami.Units.iconSizes.medium
                                Layout.preferredHeight: Kirigami.Units.iconSizes.medium
                            }

                            ColumnLayout {
                                Layout.fillWidth: true
                                spacing: 0

                                Controls.Label {
                                    text: modelData.name
                                    Layout.fillWidth: true
                                    elide: Text.ElideRight
                                }

                                Controls.Label {
                                    text: modelData.command
                                    Layout.fillWidth: true
                                    elide: Text.ElideMiddle
                                    opacity: 0.7
                                    font.pointSize: Kirigami.Theme.smallFont.pointSize
                                }
                            }

                            Controls.Button {
                                text: i18nc("@action:button", "Add")
                                icon.name: "list-add"
                                onClicked: {
                                    let id = activePresetManager.addPresetFromDesktopFile(modelData.desktopFilePath)
                                    if (id !== "") {
                                        applicationWindow().showPassiveNotification(
                                            i18nc("@info", "Added preset: %1", modelData.name))
                                        addPresetDialog.close()
                                    } else {
                                        applicationWindow().showPassiveNotification(
                                            i18nc("@info", "Failed to add preset"), "long")
                                    }
                                }
                            }
                        }
                    }

                    Kirigami.PlaceholderMessage {
                        anchors.centerIn: parent
                        visible: appListView.count === 0
                        text: appSearchField.text !== ""
                              ? i18nc("@info", "No applications match your search")
                              : i18nc("@info", "No applications found")
                        icon.name: "application-x-executable"
                    }
                }
            }

            Kirigami.InlineMessage {
                Layout.fillWidth: true
                text: i18nc("@info", "Select an installed application to add as a launch preset. You can then select it when configuring session instances.")
                type: Kirigami.MessageType.Information
                visible: true
            }
        }
    }
}
