// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2024 hikaps

import QtQuick
import QtQuick.Layouts
import QtQuick.Controls as Controls
import org.kde.kirigami as Kirigami

Kirigami.ScrollablePage {
    id: root
    title: i18nc("@title", "Settings")

    property bool helperInstalled: false
    property bool helperRunning: false

    // Settings values (would be persisted via QSettings in production)
    property bool hidePanels: true
    property bool killSteam: true
    property bool restoreSession: false
    property string scalingMode: "fit"
    property string filterMode: "linear"
    property bool steamIntegration: true

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
                currentIndex: {
                    for (let i = 0; i < model.length; i++) {
                        if (model[i].value === root.scalingMode) return i
                    }
                    return 0
                }
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
                currentIndex: {
                    for (let i = 0; i < model.length; i++) {
                        if (model[i].value === root.filterMode) return i
                    }
                    return 0
                }
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
        }

        // Audio Settings Section
        Kirigami.FormLayout {
            Layout.fillWidth: true

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

            Kirigami.Separator {
                Kirigami.FormData.isSection: true
                Kirigami.FormData.label: i18nc("@title:group", "Helper Service")
            }

            RowLayout {
                Kirigami.FormData.label: i18nc("@label", "Status:")
                spacing: Kirigami.Units.smallSpacing

                Kirigami.Icon {
                    source: root.helperInstalled ? 
                        (root.helperRunning ? "dialog-ok-apply" : "dialog-warning") : 
                        "dialog-error"
                    Layout.preferredWidth: Kirigami.Units.iconSizes.small
                    Layout.preferredHeight: Kirigami.Units.iconSizes.small
                }

                Controls.Label {
                    text: {
                        if (!root.helperInstalled) {
                            return i18nc("@info", "Not installed")
                        } else if (root.helperRunning) {
                            return i18nc("@info", "Running")
                        } else {
                            return i18nc("@info", "Installed but not running")
                        }
                    }
                    color: {
                        if (!root.helperInstalled) {
                            return Kirigami.Theme.negativeTextColor
                        } else if (root.helperRunning) {
                            return Kirigami.Theme.positiveTextColor
                        } else {
                            return Kirigami.Theme.neutralTextColor
                        }
                    }
                }
            }

            Controls.Button {
                Kirigami.FormData.label: " "
                text: root.helperInstalled ? i18nc("@action:button", "Reinstall Helper") : i18nc("@action:button", "Install Helper")
                icon.name: "run-install"
                onClicked: installHelperDialog.open()
            }
        }

        // Helper info card
        Kirigami.InlineMessage {
            Layout.fillWidth: true
            text: i18nc("@info", "The CouchPlay Helper is a privileged system service required for creating users and managing device permissions. It uses PolicyKit for secure authorization.")
            type: Kirigami.MessageType.Information
            visible: !root.helperInstalled
            
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
}
