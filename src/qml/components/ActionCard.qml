// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 CouchPlay Contributors

import QtQuick
import QtQuick.Layouts
import QtQuick.Controls as Controls
import org.kde.kirigami as Kirigami

/**
 * ActionCard - A clickable call-to-action card with large icon and centered content.
 * 
 * Uses AbstractCard to avoid content clipping issues.
 * 
 * Use for:
 * - Quick action buttons on home/dashboard pages
 * - Feature cards
 * - Navigation shortcuts with visual icons
 */
Kirigami.AbstractCard {
    id: root

    required property string iconName
    required property string title
    property string description: ""
    property int badgeCount: 0
    property bool centerContent: true

    signal clicked()

    // Hover effect for visual feedback
    HoverHandler {
        id: hoverHandler
    }

    background: Rectangle {
        color: hoverHandler.hovered 
            ? Qt.alpha(Kirigami.Theme.hoverColor, 0.5)
            : Kirigami.Theme.backgroundColor
        radius: Kirigami.Units.smallSpacing
        border.width: 1
        border.color: Qt.alpha(Kirigami.Theme.textColor, 0.1)

        Behavior on color {
            ColorAnimation { duration: Kirigami.Units.shortDuration }
        }
    }

    contentItem: ColumnLayout {
        spacing: Kirigami.Units.smallSpacing

        // Icon row with optional badge
        RowLayout {
            Layout.fillWidth: true
            Layout.alignment: root.centerContent ? Qt.AlignHCenter : Qt.AlignLeft

            Kirigami.Icon {
                source: root.iconName
                Layout.preferredWidth: Kirigami.Units.iconSizes.huge
                Layout.preferredHeight: Kirigami.Units.iconSizes.huge
            }

            // Badge indicator
            Rectangle {
                visible: root.badgeCount > 0
                width: badgeLabel.implicitWidth + Kirigami.Units.smallSpacing * 2
                height: badgeLabel.implicitHeight + Kirigami.Units.smallSpacing
                radius: height / 2
                color: Kirigami.Theme.highlightColor

                Controls.Label {
                    id: badgeLabel
                    anchors.centerIn: parent
                    text: root.badgeCount.toString()
                    color: Kirigami.Theme.highlightedTextColor
                    font: Kirigami.Theme.smallFont
                }
            }
        }

        // Title
        Controls.Label {
            text: root.title
            font.bold: true
            Layout.alignment: root.centerContent ? Qt.AlignHCenter : Qt.AlignLeft
            Layout.fillWidth: true
            horizontalAlignment: root.centerContent ? Text.AlignHCenter : Text.AlignLeft
            wrapMode: Text.WordWrap
        }

        // Description (optional)
        Controls.Label {
            visible: root.description !== ""
            text: root.description
            wrapMode: Text.WordWrap
            horizontalAlignment: root.centerContent ? Text.AlignHCenter : Text.AlignLeft
            Layout.fillWidth: true
            opacity: 0.7
        }
    }

    // Make entire card clickable
    TapHandler {
        onTapped: root.clicked()
    }
}
