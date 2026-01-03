// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 CouchPlay Contributors

import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

/**
 * LayoutPreview - Visual preview of screen layout configuration
 * 
 * Shows a scaled representation of monitors and instance windows,
 * allowing visual arrangement and configuration.
 */
Item {
    id: root

    property var monitors: [] // List of {name, x, y, width, height, primary}
    property var instances: [] // List of {index, x, y, width, height, monitorIndex}
    property int selectedInstance: -1
    property real scaleFactor: 0.1 // Scale from real pixels to preview

    signal instanceSelected(int index)
    signal instanceMoved(int index, int x, int y)
    signal instanceResized(int index, int width, int height)

    implicitWidth: 600
    implicitHeight: 400

    // Background
    Rectangle {
        anchors.fill: parent
        color: Kirigami.Theme.alternateBackgroundColor
        radius: Kirigami.Units.smallSpacing
    }

    // Monitors
    Repeater {
        model: root.monitors

        Rectangle {
            x: modelData.x * root.scaleFactor
            y: modelData.y * root.scaleFactor
            width: modelData.width * root.scaleFactor
            height: modelData.height * root.scaleFactor
            color: "transparent"
            border.color: modelData.primary ? Kirigami.Theme.highlightColor : Kirigami.Theme.textColor
            border.width: modelData.primary ? 2 : 1
            opacity: 0.5

            QQC2.Label {
                anchors.centerIn: parent
                text: modelData.name
                font: Kirigami.Theme.smallFont
                opacity: 0.7
            }
        }
    }

    // Instance windows
    Repeater {
        model: root.instances

        Rectangle {
            id: instanceRect

            x: modelData.x * root.scaleFactor
            y: modelData.y * root.scaleFactor
            width: modelData.width * root.scaleFactor
            height: modelData.height * root.scaleFactor

            color: {
                const colors = [
                    Qt.rgba(0.2, 0.4, 0.8, 0.6),  // Blue
                    Qt.rgba(0.8, 0.3, 0.2, 0.6),  // Red
                    Qt.rgba(0.2, 0.7, 0.3, 0.6),  // Green
                    Qt.rgba(0.7, 0.5, 0.2, 0.6)   // Orange
                ]
                return colors[modelData.index % colors.length]
            }
            border.color: root.selectedInstance === modelData.index 
                ? Kirigami.Theme.highlightColor 
                : Kirigami.Theme.textColor
            border.width: root.selectedInstance === modelData.index ? 3 : 1
            radius: Kirigami.Units.smallSpacing

            QQC2.Label {
                anchors.centerIn: parent
                text: qsTr("P%1").arg(modelData.index + 1)
                font.bold: true
                color: "white"
            }

            MouseArea {
                anchors.fill: parent
                drag.target: parent
                onClicked: root.instanceSelected(modelData.index)
                onReleased: {
                    if (drag.active) {
                        root.instanceMoved(
                            modelData.index,
                            Math.round(instanceRect.x / root.scaleFactor),
                            Math.round(instanceRect.y / root.scaleFactor)
                        )
                    }
                }
            }

            // Resize handle
            Rectangle {
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                width: 16
                height: 16
                color: Kirigami.Theme.highlightColor
                opacity: resizeArea.containsMouse ? 1.0 : 0.5
                visible: root.selectedInstance === modelData.index

                MouseArea {
                    id: resizeArea
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.SizeFDiagCursor

                    property point startPos
                    property size startSize

                    onPressed: (mouse) => {
                        startPos = Qt.point(mouse.x, mouse.y)
                        startSize = Qt.size(instanceRect.width, instanceRect.height)
                    }

                    onPositionChanged: (mouse) => {
                        if (pressed) {
                            const dx = mouse.x - startPos.x
                            const dy = mouse.y - startPos.y
                            instanceRect.width = Math.max(50, startSize.width + dx)
                            instanceRect.height = Math.max(50, startSize.height + dy)
                        }
                    }

                    onReleased: {
                        root.instanceResized(
                            modelData.index,
                            Math.round(instanceRect.width / root.scaleFactor),
                            Math.round(instanceRect.height / root.scaleFactor)
                        )
                    }
                }
            }
        }
    }

    // Scale indicator
    RowLayout {
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.margins: Kirigami.Units.smallSpacing

        QQC2.Label {
            text: qsTr("Scale: %1%").arg(Math.round(root.scaleFactor * 100))
            font: Kirigami.Theme.smallFont
            opacity: 0.7
        }
    }
}
