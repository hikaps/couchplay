// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 CouchPlay Contributors

import QtQuick
import QtQuick.Layouts
import QtQuick.Controls as Controls
import org.kde.kirigami as Kirigami

/**
 * A ComboBox for selecting a launch preset.
 * 
 * Usage:
 *   PresetSelector {
 *       presetManager: presetManager
 *       currentPresetId: instanceConfig.presetId
 *       onPresetSelected: (presetId) => sessionManager.setInstancePreset(index, presetId)
 *   }
 */
Controls.ComboBox {
    id: root

    required property var presetManager
    property string currentPresetId: "steam"

    signal presetSelected(string presetId)

    // Model from PresetManager
    model: presetManager ? presetManager.presets : []
    textRole: "name"
    valueRole: "id"

    // Display icon alongside text
    delegate: Controls.ItemDelegate {
        required property var modelData
        required property int index

        width: parent ? parent.width : implicitWidth
        text: modelData.name
        icon.name: modelData.iconName || "application-x-executable"
        highlighted: root.highlightedIndex === index

        contentItem: RowLayout {
            spacing: Kirigami.Units.smallSpacing
            
            Kirigami.Icon {
                source: modelData.iconName || "application-x-executable"
                Layout.preferredWidth: Kirigami.Units.iconSizes.small
                Layout.preferredHeight: Kirigami.Units.iconSizes.small
            }
            
            Controls.Label {
                text: modelData.name
                Layout.fillWidth: true
                elide: Text.ElideRight
            }
            
            // Show badge for builtin presets
            Controls.Label {
                visible: modelData.isBuiltin
                text: i18nc("@info badge for builtin presets", "Built-in")
                font.pointSize: Kirigami.Theme.smallFont.pointSize
                opacity: 0.7
            }
        }
    }

    // Set initial selection based on currentPresetId
    Component.onCompleted: {
        updateCurrentIndex()
    }

    onCurrentPresetIdChanged: {
        updateCurrentIndex()
    }

    // When user selects a different preset
    onActivated: function(index) {
        if (index >= 0 && model && index < model.length) {
            let preset = model[index]
            if (preset && preset.id) {
                presetSelected(preset.id)
            }
        }
    }

    function updateCurrentIndex() {
        if (!model || model.length === 0) {
            return
        }
        
        for (let i = 0; i < model.length; i++) {
            if (model[i].id === currentPresetId) {
                currentIndex = i
                return
            }
        }
        
        // Default to first preset (Steam) if not found
        currentIndex = 0
    }
}
