// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2024 hikaps

import QtQuick
import QtQuick.Controls as Controls
import org.kde.kirigami as Kirigami

Kirigami.PromptDialog {
    id: root
    title: i18nc("@title:dialog", "Remove Preset")
    subtitle: i18nc("@info", "Remove the preset \"%1\"?", presetName)
    standardButtons: Kirigami.Dialog.Yes | Kirigami.Dialog.Cancel

    required property var presetManager
    property string presetId: ""
    property string presetName: ""

    onAccepted: {
        if (root.presetManager.removeCustomPreset(presetId)) {
            applicationWindow().showPassiveNotification(
                i18nc("@info", "Preset removed"))
        } else {
            applicationWindow().showPassiveNotification(
                i18nc("@info", "Failed to remove preset"), "long")
        }
    }
}