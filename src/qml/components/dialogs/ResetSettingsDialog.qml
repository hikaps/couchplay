// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2024 hikaps

import QtQuick
import QtQuick.Layouts
import QtQuick.Controls as Controls
import org.kde.kirigami as Kirigami

Kirigami.PromptDialog {
    id: root
    
    required property var settingsManager
    
    title: i18nc("@title:dialog", "Reset Settings")
    subtitle: i18nc("@info", "Reset all settings to their default values?")
    standardButtons: Kirigami.Dialog.Yes | Kirigami.Dialog.Cancel

    onAccepted: {
        if (root.settingsManager) {
            root.settingsManager.resetToDefaults()
        }
        applicationWindow().showPassiveNotification(
            i18nc("@info", "Settings reset to defaults"))
    }
}