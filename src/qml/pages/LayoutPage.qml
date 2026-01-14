// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2024 hikaps

import QtQuick
import QtQuick.Layouts
import QtQuick.Controls as Controls
import org.kde.kirigami as Kirigami

import "../components" as Components

import "../components"

Kirigami.ScrollablePage {
    id: root
    title: i18nc("@title", "Layout")

    required property var monitorManager
    property int playerCount: 2
    property string selectedLayout: "horizontal"

    actions: [
        Kirigami.Action {
            icon.name: "view-refresh"
            text: i18nc("@action:button", "Refresh Monitors")
            onTriggered: monitorManager.refresh()
        }
    ]

    ColumnLayout {
        spacing: Kirigami.Units.largeSpacing

        // Header
        ColumnLayout {
            Layout.fillWidth: true
            spacing: Kirigami.Units.smallSpacing

            Kirigami.Heading {
                text: i18nc("@title", "Screen Layout Configuration")
                level: 2
            }

            Controls.Label {
                text: i18nc("@info", "Configure how game instances are positioned across your displays.")
                wrapMode: Text.WordWrap
                Layout.fillWidth: true
                opacity: 0.7
            }
        }

        // Monitor info
        Kirigami.InlineMessage {
            Layout.fillWidth: true
            text: {
                const count = monitorManager.monitorCount
                if (count === 0) {
                    return i18nc("@info", "No monitors detected.")
                } else if (count === 1) {
                    return i18nc("@info", "1 monitor detected. All instances will share this display.")
                } else {
                    return i18nc("@info", "%1 monitors detected. You can use multi-monitor layouts.", count)
                }
            }
            type: monitorManager.monitorCount === 0 ? Kirigami.MessageType.Warning : Kirigami.MessageType.Information
            visible: true
        }

        // Detected monitors
        Kirigami.FormLayout {
            Layout.fillWidth: true

            Kirigami.Separator {
                Kirigami.FormData.isSection: true
                Kirigami.FormData.label: i18nc("@title", "Detected Monitors")
            }
        }

        Repeater {
            model: monitorManager.monitors

            delegate: Kirigami.Card {
                Layout.fillWidth: true

                property bool isPrimary: modelData.primary

                contentItem: RowLayout {
                    spacing: Kirigami.Units.largeSpacing

                    Kirigami.Icon {
                        source: "video-display"
                        Layout.preferredWidth: Kirigami.Units.iconSizes.medium
                        Layout.preferredHeight: Kirigami.Units.iconSizes.medium
                    }

                    ColumnLayout {
                        spacing: Kirigami.Units.smallSpacing
                        Layout.fillWidth: true

                        Controls.Label {
                            text: modelData.name
                            font.bold: true
                        }

                        Controls.Label {
                            text: i18nc("@info", "%1x%2 @ %3Hz", modelData.width, modelData.height, modelData.refreshRate)
                            font.family: "monospace"
                            opacity: 0.7
                        }
                    }

                    Kirigami.Chip {
                        text: isPrimary ? i18nc("@info", "Primary") : i18nc("@info", "Secondary")
                        icon.name: isPrimary ? "starred-symbolic" : "video-display"
                        closable: false
                        checkable: false
                    }
                }
            }
        }

        // Layout configuration
        Kirigami.FormLayout {
            Layout.fillWidth: true
            Layout.topMargin: Kirigami.Units.largeSpacing

            Kirigami.Separator {
                Kirigami.FormData.isSection: true
                Kirigami.FormData.label: i18nc("@title", "Layout Configuration")
            }

            Controls.SpinBox {
                id: playerCountSpinner
                Kirigami.FormData.label: i18nc("@label", "Number of players:")
                from: 2
                to: 4
                value: root.playerCount
                onValueChanged: root.playerCount = value
            }

            Controls.ComboBox {
                id: layoutCombo
                Kirigami.FormData.label: i18nc("@label", "Layout type:")
                model: [
                    { value: "horizontal", text: i18nc("@item:inlistbox", "Horizontal Split"), icon: "view-split-left-right" },
                    { value: "vertical", text: i18nc("@item:inlistbox", "Vertical Split"), icon: "view-split-top-bottom" },
                    { value: "grid", text: i18nc("@item:inlistbox", "Grid (2x2)"), icon: "view-grid" },
                    { value: "multi-monitor", text: i18nc("@item:inlistbox", "Multi-Monitor"), icon: "video-display" }
                ]
                textRole: "text"
                valueRole: "value"
                currentIndex: Math.max(0, model.findIndex(item => item.value === root.selectedLayout))
                onCurrentValueChanged: root.selectedLayout = currentValue

                delegate: Controls.ItemDelegate {
                    width: layoutCombo.width
                    contentItem: RowLayout {
                        spacing: Kirigami.Units.smallSpacing
                        Kirigami.Icon {
                            source: modelData.icon
                            Layout.preferredWidth: Kirigami.Units.iconSizes.small
                            Layout.preferredHeight: Kirigami.Units.iconSizes.small
                        }
                        Controls.Label {
                            text: modelData.text
                            Layout.fillWidth: true
                        }
                    }
                    highlighted: layoutCombo.highlightedIndex === index
                }
            }
        }

        // Layout preview
        Kirigami.Card {
            Layout.fillWidth: true
            Layout.preferredHeight: Kirigami.Units.gridUnit * 15

            header: Kirigami.Heading {
                text: i18nc("@title", "Layout Preview")
                level: 3
                padding: Kirigami.Units.largeSpacing
            }

            contentItem: Item {
                // Preview area
                Rectangle {
                    anchors.fill: parent
                    anchors.margins: Kirigami.Units.smallSpacing
                    color: Kirigami.Theme.alternateBackgroundColor
                    radius: Kirigami.Units.smallSpacing

                    // Calculate preview layout
                    property var layoutRects: {
                        const w = width - 20
                        const h = height - 20
                        const count = root.playerCount
                        let rects = []

                        switch (root.selectedLayout) {
                            case "horizontal":
                                const hw = w / count
                                for (let i = 0; i < count; i++) {
                                    rects.push({ x: 10 + i * hw, y: 10, width: hw - 4, height: h - 4 })
                                }
                                break
                            case "vertical":
                                const vh = h / count
                                for (let i = 0; i < count; i++) {
                                    rects.push({ x: 10, y: 10 + i * vh, width: w - 4, height: vh - 4 })
                                }
                                break
                            case "grid":
                                const cols = count <= 2 ? count : 2
                                const rows = Math.ceil(count / cols)
                                const gw = w / cols
                                const gh = h / rows
                                for (let i = 0; i < count; i++) {
                                    const col = i % cols
                                    const row = Math.floor(i / cols)
                                    rects.push({ x: 10 + col * gw, y: 10 + row * gh, width: gw - 4, height: gh - 4 })
                                }
                                break
                            case "multi-monitor":
                                // Show side-by-side monitors
                                const mw = w / Math.max(count, monitorManager.monitorCount)
                                for (let i = 0; i < count; i++) {
                                    rects.push({ x: 10 + i * mw, y: 10, width: mw - 4, height: h - 4 })
                                }
                                break
                        }
                        return rects
                    }

                    // Player windows
                    Repeater {
                        model: parent.layoutRects

                        Rectangle {
                            x: modelData.x
                            y: modelData.y
                            width: modelData.width
                            height: modelData.height
                            radius: Kirigami.Units.smallSpacing

                            color: {
                                const colors = [
                                    Qt.rgba(0.2, 0.4, 0.8, 0.7),
                                    Qt.rgba(0.8, 0.3, 0.2, 0.7),
                                    Qt.rgba(0.2, 0.7, 0.3, 0.7),
                                    Qt.rgba(0.7, 0.5, 0.2, 0.7)
                                ]
                                return colors[index % colors.length]
                            }
                            border.color: Kirigami.Theme.textColor
                            border.width: 1

                            Controls.Label {
                                anchors.centerIn: parent
                                text: i18nc("@info", "Player %1", index + 1)
                                font.bold: true
                                color: "white"
                            }
                        }
                    }
                }
            }
        }

        // Layout explanations
        Components.InfoCard {
            title: i18nc("@title", "Layout Types")
            Layout.fillWidth: true

            RowLayout {
                spacing: Kirigami.Units.largeSpacing
                Kirigami.Icon {
                    source: "view-split-left-right"
                    Layout.preferredWidth: Kirigami.Units.iconSizes.medium
                    Layout.preferredHeight: Kirigami.Units.iconSizes.medium
                }
                ColumnLayout {
                    spacing: 0
                    Layout.fillWidth: true
                    Controls.Label {
                        text: i18nc("@title", "Horizontal Split")
                        font.bold: true
                    }
                    Controls.Label {
                        text: i18nc("@info", "Players side by side. Best for racing games and platformers.")
                        wrapMode: Text.WordWrap
                        opacity: 0.7
                        Layout.fillWidth: true
                    }
                }
            }

            RowLayout {
                spacing: Kirigami.Units.largeSpacing
                Kirigami.Icon {
                    source: "view-split-top-bottom"
                    Layout.preferredWidth: Kirigami.Units.iconSizes.medium
                    Layout.preferredHeight: Kirigami.Units.iconSizes.medium
                }
                ColumnLayout {
                    spacing: 0
                    Layout.fillWidth: true
                    Controls.Label {
                        text: i18nc("@title", "Vertical Split")
                        font.bold: true
                    }
                    Controls.Label {
                        text: i18nc("@info", "Players stacked vertically. Good for ultrawide monitors.")
                        wrapMode: Text.WordWrap
                        opacity: 0.7
                        Layout.fillWidth: true
                    }
                }
            }

            RowLayout {
                spacing: Kirigami.Units.largeSpacing
                Kirigami.Icon {
                    source: "view-grid"
                    Layout.preferredWidth: Kirigami.Units.iconSizes.medium
                    Layout.preferredHeight: Kirigami.Units.iconSizes.medium
                }
                ColumnLayout {
                    spacing: 0
                    Layout.fillWidth: true
                    Controls.Label {
                        text: i18nc("@title", "Grid (2x2)")
                        font.bold: true
                    }
                    Controls.Label {
                        text: i18nc("@info", "Four equal quadrants. Ideal for 4-player sessions on one screen.")
                        wrapMode: Text.WordWrap
                        opacity: 0.7
                        Layout.fillWidth: true
                    }
                }
            }

            RowLayout {
                spacing: Kirigami.Units.largeSpacing
                Kirigami.Icon {
                    source: "video-display"
                    Layout.preferredWidth: Kirigami.Units.iconSizes.medium
                    Layout.preferredHeight: Kirigami.Units.iconSizes.medium
                }
                ColumnLayout {
                    spacing: 0
                    Layout.fillWidth: true
                    Controls.Label {
                        text: i18nc("@title", "Multi-Monitor")
                        font.bold: true
                    }
                    Controls.Label {
                        text: i18nc("@info", "Each player gets their own monitor. The best experience with multiple displays.")
                        wrapMode: Text.WordWrap
                        opacity: 0.7
                        Layout.fillWidth: true
                    }
                }
            }
        }
    }
}
