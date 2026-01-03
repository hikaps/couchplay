// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2024 hikaps

import QtQuick
import QtQuick.Layouts
import QtQuick.Controls as Controls
import org.kde.kirigami as Kirigami

Kirigami.ScrollablePage {
    id: root
    title: i18nc("@title", "Settings")

    Kirigami.FormLayout {
        // General Settings
        Kirigami.Separator {
            Kirigami.FormData.isSection: true
            Kirigami.FormData.label: i18nc("@title:group", "General")
        }

        Controls.CheckBox {
            Kirigami.FormData.label: i18nc("@option:check", "Hide KDE panels during session:")
            checked: true
        }

        Controls.CheckBox {
            Kirigami.FormData.label: i18nc("@option:check", "Kill Steam before starting:")
            checked: true
        }

        Controls.CheckBox {
            Kirigami.FormData.label: i18nc("@option:check", "Restore last session on startup:")
            checked: false
        }

        // Gamescope Settings
        Kirigami.Separator {
            Kirigami.FormData.isSection: true
            Kirigami.FormData.label: i18nc("@title:group", "Gamescope")
        }

        Controls.ComboBox {
            Kirigami.FormData.label: i18nc("@label", "Default scaling mode:")
            model: ["Fit", "Fill", "Stretch", "Integer"]
        }

        Controls.ComboBox {
            Kirigami.FormData.label: i18nc("@label", "Default filter:")
            model: ["Linear", "Nearest", "FSR", "NIS"]
        }

        Controls.CheckBox {
            Kirigami.FormData.label: i18nc("@option:check", "Enable Steam integration (-e):")
            checked: true
        }

        // Audio Settings
        Kirigami.Separator {
            Kirigami.FormData.isSection: true
            Kirigami.FormData.label: i18nc("@title:group", "Audio")
        }

        Controls.Label {
            Kirigami.FormData.label: i18nc("@label", "Audio server:")
            text: "PipeWire"
        }

        Controls.Button {
            Kirigami.FormData.label: i18nc("@label", "Multi-user audio:")
            text: i18nc("@action:button", "Configure")
            icon.name: "configure"
        }

        // Helper Service
        Kirigami.Separator {
            Kirigami.FormData.isSection: true
            Kirigami.FormData.label: i18nc("@title:group", "Helper Service")
        }

        Controls.Label {
            Kirigami.FormData.label: i18nc("@label", "Status:")
            text: i18nc("@info", "Not installed")
            color: Kirigami.Theme.negativeTextColor
        }

        Controls.Button {
            text: i18nc("@action:button", "Install Helper")
            icon.name: "run-install"
        }
    }
}
