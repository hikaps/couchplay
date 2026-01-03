// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 CouchPlay Contributors

import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

/**
 * MonitorSelector - Dropdown for selecting and configuring monitors
 * 
 * Displays available monitors with resolution info and allows
 * selection for instance placement.
 */
ColumnLayout {
    id: root

    property var monitors: [] // List of {name, width, height, x, y, primary, refreshRate}
    property int selectedIndex: -1

    signal monitorSelected(int index)

    spacing: Kirigami.Units.smallSpacing

    QQC2.Label {
        text: qsTr("Target Monitor")
        font.bold: true
    }

    QQC2.ComboBox {
        id: monitorCombo
        Layout.fillWidth: true
        model: root.monitors
        textRole: "name"
        currentIndex: root.selectedIndex

        delegate: QQC2.ItemDelegate {
            width: monitorCombo.width
            contentItem: RowLayout {
                spacing: Kirigami.Units.smallSpacing

                Kirigami.Icon {
                    source: modelData.primary ? "monitor-primary" : "monitor"
                    Layout.preferredWidth: Kirigami.Units.iconSizes.small
                    Layout.preferredHeight: Kirigami.Units.iconSizes.small
                }

                ColumnLayout {
                    spacing: 0
                    Layout.fillWidth: true

                    QQC2.Label {
                        text: modelData.name
                        Layout.fillWidth: true
                    }

                    QQC2.Label {
                        text: qsTr("%1x%2 @ %3Hz").arg(modelData.width).arg(modelData.height).arg(modelData.refreshRate || 60)
                        font: Kirigami.Theme.smallFont
                        opacity: 0.7
                        Layout.fillWidth: true
                    }
                }

                Kirigami.Chip {
                    text: qsTr("Primary")
                    visible: modelData.primary
                    checkable: false
                }
            }

            highlighted: monitorCombo.highlightedIndex === index
        }

        onCurrentIndexChanged: {
            if (currentIndex >= 0) {
                root.monitorSelected(currentIndex)
            }
        }
    }

    // Quick info about selected monitor
    Kirigami.FormLayout {
        visible: root.selectedIndex >= 0 && root.monitors.length > 0

        QQC2.Label {
            Kirigami.FormData.label: qsTr("Resolution:")
            text: {
                if (root.selectedIndex >= 0 && root.monitors[root.selectedIndex]) {
                    const m = root.monitors[root.selectedIndex]
                    return qsTr("%1 x %2").arg(m.width).arg(m.height)
                }
                return "-"
            }
        }

        QQC2.Label {
            Kirigami.FormData.label: qsTr("Position:")
            text: {
                if (root.selectedIndex >= 0 && root.monitors[root.selectedIndex]) {
                    const m = root.monitors[root.selectedIndex]
                    return qsTr("(%1, %2)").arg(m.x).arg(m.y)
                }
                return "-"
            }
        }

        QQC2.Label {
            Kirigami.FormData.label: qsTr("Refresh Rate:")
            text: {
                if (root.selectedIndex >= 0 && root.monitors[root.selectedIndex]) {
                    return qsTr("%1 Hz").arg(root.monitors[root.selectedIndex].refreshRate || 60)
                }
                return "-"
            }
        }
    }
}
