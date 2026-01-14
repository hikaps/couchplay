// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 CouchPlay Contributors

import QtQuick
import QtQuick.Layouts
import QtQuick.Controls as Controls
import org.kde.kirigami as Kirigami

/**
 * SelectableCard - A card with selection state, hover effects, and optional actions.
 * 
 * Uses AbstractCard to avoid content clipping issues with nested layouts.
 * 
 * Use for:
 * - Profile selection cards
 * - Layout selection cards
 * - Any card where user picks from multiple options
 */
Kirigami.AbstractCard {
    id: root

    // Required properties
    required property string title
    
    // Optional properties
    property string subtitle: ""
    property string iconName: ""
    property bool selected: false
    property bool showHoverEffect: true
    property bool compact: false  // Reduces padding/spacing
    property string selectedLabel: i18nc("@info", "Selected")
    
    // Content slot for custom content above actions
    default property alias content: contentContainer.data
    
    // Action buttons: array of {text, icon, highlighted, flat, tooltip, onClicked}
    property var actions: []
    
    // Signals
    signal clicked()

    // Hover detection (non-blocking - doesn't interfere with child mouse events)
    HoverHandler {
        id: hoverHandler
    }

    // Animated background with selection and hover states
    background: Rectangle {
        color: {
            if (root.selected) {
                return Qt.alpha(Kirigami.Theme.highlightColor, 0.12)
            } else if (root.showHoverEffect && hoverHandler.hovered) {
                return Qt.alpha(Kirigami.Theme.hoverColor, 0.5)
            } else {
                return Kirigami.Theme.backgroundColor
            }
        }
        radius: Kirigami.Units.smallSpacing
        border.width: root.selected ? 2 : 1
        border.color: root.selected 
            ? Kirigami.Theme.highlightColor 
            : Qt.alpha(Kirigami.Theme.textColor, 0.1)

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
        spacing: root.compact ? Kirigami.Units.smallSpacing : Kirigami.Units.mediumSpacing

        // Header row with icon and title
        RowLayout {
            Layout.fillWidth: true
            spacing: Kirigami.Units.mediumSpacing
            visible: root.title !== "" || root.iconName !== ""

            // Icon container (optional)
            Rectangle {
                visible: root.iconName !== ""
                Layout.preferredWidth: root.compact ? Kirigami.Units.iconSizes.medium : Kirigami.Units.iconSizes.large
                Layout.preferredHeight: Layout.preferredWidth
                radius: Kirigami.Units.smallSpacing
                color: Qt.alpha(Kirigami.Theme.highlightColor, 0.15)

                Kirigami.Icon {
                    anchors.centerIn: parent
                    source: root.iconName
                    width: parent.width * 0.7
                    height: width
                    color: Kirigami.Theme.highlightColor
                }
            }

            // Title and subtitle
            ColumnLayout {
                Layout.fillWidth: true
                spacing: 2

                Controls.Label {
                    visible: root.title !== ""
                    text: root.title
                    font.bold: true
                    font.pointSize: root.compact 
                        ? Kirigami.Theme.defaultFont.pointSize 
                        : Kirigami.Theme.defaultFont.pointSize * 1.1
                    elide: Text.ElideRight
                    Layout.fillWidth: true
                }

                Controls.Label {
                    visible: root.subtitle !== ""
                    text: root.subtitle
                    opacity: 0.7
                    font.pointSize: Kirigami.Theme.smallFont.pointSize
                    elide: Text.ElideRight
                    Layout.fillWidth: true
                }
            }

            // Selected indicator chip
            Kirigami.Chip {
                visible: root.selected && root.selectedLabel !== ""
                text: root.selectedLabel
                icon.name: "checkmark"
                closable: false
                checkable: false
            }
        }

        // Custom content slot
        Item {
            id: contentContainer
            Layout.fillWidth: true
            visible: children.length > 0
            implicitHeight: childrenRect.height
        }

        // Action buttons row (if any)
        RowLayout {
            Layout.fillWidth: true
            spacing: Kirigami.Units.smallSpacing
            visible: root.actions.length > 0

            Repeater {
                model: root.actions
                delegate: Controls.Button {
                    text: modelData.text ?? ""
                    icon.name: modelData.icon ?? ""
                    highlighted: modelData.highlighted ?? false
                    flat: modelData.flat ?? false
                    display: modelData.text ? Controls.AbstractButton.TextBesideIcon : Controls.AbstractButton.IconOnly
                    onClicked: {
                        if (modelData.onClicked) {
                            modelData.onClicked()
                        }
                    }
                    
                    Controls.ToolTip.visible: modelData.tooltip && hovered
                    Controls.ToolTip.text: modelData.tooltip ?? ""
                    Controls.ToolTip.delay: 1000
                }
            }

            // Spacer to push delete button to the right
            Item { 
                Layout.fillWidth: true 
                visible: root.actions.length > 0
            }
        }
    }

    // Click handler for card body
    TapHandler {
        onTapped: root.clicked()
    }
}
