// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2024 hikaps

import QtQuick
import QtQuick.Layouts
import QtQuick.Controls as Controls
import org.kde.kirigami as Kirigami

Kirigami.Dialog {
    id: root
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