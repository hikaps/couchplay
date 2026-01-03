// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2024 hikaps

import QtQuick
import QtQuick.Layouts
import QtQuick.Controls as Controls
import org.kde.kirigami as Kirigami

Kirigami.ScrollablePage {
    id: root
    title: i18nc("@title", "Layout")

    Kirigami.PlaceholderMessage {
        anchors.centerIn: parent
        text: i18nc("@info", "Screen Layout")
        explanation: i18nc("@info", "Configure how game instances are positioned on your screen.")
        icon.name: "view-split-left-right"
    }
}
