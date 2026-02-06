// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2024 hikaps

import QtQuick
import QtQuick.Layouts
import QtQuick.Controls as Controls
import org.kde.kirigami as Kirigami
import io.github.hikaps.couchplay 1.0

Kirigami.Dialog {
    id: root
    title: isAppImage ? i18nc("@title:dialog", "Setup Required") : i18nc("@title:dialog", "Install Helper Service")
    standardButtons: Kirigami.Dialog.Close
    preferredWidth: Kirigami.Units.gridUnit * 30

    property bool isAppImage: false // Will be overridden by context property or binding

    ColumnLayout {
        spacing: Kirigami.Units.largeSpacing

        Controls.Label {
            text: isAppImage 
                ? i18nc("@info", "CouchPlay needs to install a system helper service to manage input devices and users. This requires administrator privileges.")
                : i18nc("@info", "To install the CouchPlay Helper, run the following command in a terminal:")
            wrapMode: Text.WordWrap
            Layout.fillWidth: true
        }

        // AppImage Installation UI
        ColumnLayout {
            visible: isAppImage
            Layout.fillWidth: true
            spacing: Kirigami.Units.largeSpacing

            Controls.Button {
                Layout.alignment: Qt.AlignHCenter
                text: i18nc("@action:button", "Install Service")
                icon.name: "system-run"
                onClicked: {
                    if (AppImageHelper.installHelper()) {
                        message.type = Kirigami.MessageType.Positive
                        message.text = i18nc("@info", "Service installed successfully! Please restart the application.")
                        root.standardButtons = Kirigami.Dialog.Ok
                    } else {
                        message.type = Kirigami.MessageType.Error
                        message.text = i18nc("@info", "Installation failed. Please try running the command manually.")
                    }
                    message.visible = true
                }
            }
        }

        // Manual Installation UI
        Rectangle {
            visible: !isAppImage || message.type === Kirigami.MessageType.Error
            Layout.fillWidth: true
            Layout.preferredHeight: commandLabel.implicitHeight + Kirigami.Units.largeSpacing * 2
            color: Kirigami.Theme.alternateBackgroundColor
            radius: Kirigami.Units.smallSpacing

            Controls.Label {
                id: commandLabel
                anchors.centerIn: parent
                anchors.margins: Kirigami.Units.largeSpacing
                text: isAppImage 
                    ? "sudo " + AppImageHelper.getAppDir() + "/usr/libexec/install-helper.sh install"
                    : "sudo /usr/share/couchplay/install-helper.sh install"
                font.family: "monospace"
                elide: Text.ElideMiddle
                width: parent.width - Kirigami.Units.largeSpacing * 2
            }
        }

        Kirigami.InlineMessage {
            id: message
            Layout.fillWidth: true
            text: i18nc("@info", "This will install a D-Bus system service and PolicyKit rules for secure privilege escalation.")
            type: Kirigami.MessageType.Information
            visible: true
        }
    }
}