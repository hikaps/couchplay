// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 CouchPlay Contributors

import QtQuick
import QtQuick.Layouts
import QtQuick.Controls as Controls
import org.kde.kirigami as Kirigami

/**
 * CollapsibleSection - A collapsible content section with animated toggle.
 *
 * Usage:
 *   Components.CollapsibleSection {
 *       title: "Advanced Settings"
 *       expanded: false  // default: true
 *
 *       Controls.CheckBox { text: "Option 1" }
 *       Controls.ComboBox { model: ["a", "b"] }
 *   }
 */
ColumnLayout {
    id: root

    required property string title
    property bool expanded: true

    spacing: 0

    default property alias content: contentContainer.data

    RowLayout {
        Layout.fillWidth: true
        spacing: Kirigami.Units.smallSpacing

        Kirigami.Icon {
            source: root.expanded ? "arrow-down" : "arrow-right"
            Layout.preferredWidth: Kirigami.Units.iconSizes.small
            Layout.preferredHeight: Kirigami.Units.iconSizes.small
            opacity: 0.7
        }

        Controls.Label {
            text: root.title
            font.weight: Font.Medium
            opacity: 0.8
        }

        Kirigami.Separator {
            Layout.fillWidth: true
            Layout.alignment: Qt.AlignVCenter
            opacity: 0.3
        }

        MouseArea {
            anchors.fill: parent
            cursorShape: Qt.PointingHandCursor
            onClicked: root.expanded = !root.expanded
        }
    }

    ColumnLayout {
        id: contentContainer
        Layout.fillWidth: true
        Layout.topMargin: root.expanded ? Kirigami.Units.smallSpacing : 0
        Layout.preferredHeight: root.expanded ? -1 : 0
        Layout.minimumHeight: root.expanded ? -1 : 0
        visible: root.expanded
        spacing: Kirigami.Units.smallSpacing
    }
}
