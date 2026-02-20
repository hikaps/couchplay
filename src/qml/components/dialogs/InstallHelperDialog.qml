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
    preferredWidth: Kirigami.Units.gridUnit * 30

    // Detect if running inside Flatpak by checking executable path
    // In Flatpak, the app runs from /app/bin/
    readonly property bool isFlatpak: {
        var args = Qt.application.arguments;
        return args.length > 0 && args[0].indexOf("/app/") === 0;
    }

    ColumnLayout {
        spacing: Kirigami.Units.largeSpacing

        Controls.Label {
            text: root.isFlatpak
                ? i18nc("@info", "To install the CouchPlay Helper from Flatpak, run the following command in a terminal:")
                : i18nc("@info", "To install the CouchPlay Helper, run the following command in a terminal:")
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
                text: root.isFlatpak
                    ? "mkdir -p ~/.local/share/couchplay && flatpak run --command=bash io.github.hikaps.couchplay -c \"cp /app/libexec/couchplay-helper /app/share/couchplay/install-helper.sh ~/.local/share/couchplay/\" && sudo ~/.local/share/couchplay/install-helper.sh install"
                    : "sudo /usr/share/couchplay/install-helper.sh"
                font.family: "monospace"
                wrapMode: Text.Wrap
                Layout.fillWidth: true
                horizontalAlignment: Text.AlignLeft
            }
        }

        Kirigami.InlineMessage {
            Layout.fillWidth: true
            text: root.isFlatpak
                ? i18nc("@info", "This exports the helper from the Flatpak sandbox, then installs it as a D-Bus system service with PolicyKit rules.")
                : i18nc("@info", "This will install a D-Bus system service and PolicyKit rules for secure privilege escalation.")
            type: Kirigami.MessageType.Information
            visible: true
        }
    }
}