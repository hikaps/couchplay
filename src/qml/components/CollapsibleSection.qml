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

    Controls.Button {
        Layout.fillWidth: true
        Layout.alignment: Qt.AlignLeft
        flat: true
        display: Controls.AbstractButton.TextBesideIcon
        icon.name: root.expanded ? "arrow-down" : "arrow-right"
        text: root.title
        onClicked: root.expanded = !root.expanded
    }

    ColumnLayout {
        id: contentContainer
        Layout.fillWidth: true
        Layout.topMargin: Kirigami.Units.smallSpacing
        visible: root.expanded
        opacity: root.expanded ? 1.0 : 0.0
        spacing: Kirigami.Units.smallSpacing

        Behavior on opacity {
            OpacityAnimator {
                duration: Kirigami.Units.shortDuration
            }
        }
    }
}
