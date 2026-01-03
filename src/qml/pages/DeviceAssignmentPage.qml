// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2024 hikaps

import QtQuick
import QtQuick.Layouts
import QtQuick.Controls as Controls
import org.kde.kirigami as Kirigami

Kirigami.ScrollablePage {
    id: root
    title: i18nc("@title", "Assign Devices")

    Kirigami.PlaceholderMessage {
        anchors.centerIn: parent
        text: i18nc("@info", "Device Assignment")
        explanation: i18nc("@info", "Drag and drop controllers and keyboards to assign them to each instance.")
        icon.name: "input-gamepad"
    }
}
