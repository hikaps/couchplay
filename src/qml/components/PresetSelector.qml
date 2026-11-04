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
    objectName: "comboPresetSelector"
    Accessible.role: Accessible.ComboBox
    Accessible.name: i18nc("@label", "Launcher")

    required property var presetManager
    property string currentPresetId: "steam"

    signal presetSelected(string presetId)

    function presetList() {
        const presets = presetManager ? presetManager.presets : []
        const result = []
        let foundCurrent = currentPresetId === ""
        for (let i = 0; i < presets.length; ++i) {
            result.push(presets[i])
            foundCurrent = foundCurrent || presets[i].id === currentPresetId
        }
        if (!foundCurrent) {
            result.unshift({
                id: currentPresetId,
                name: i18nc("@item", "%1 (unavailable)", currentPresetId),
                iconName: "dialog-warning",
                isBuiltin: false,
                unavailable: true
            })
        }
        return result
    }
    model: presetList()
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
    
    onModelChanged: {
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
        
        currentIndex = currentPresetId === "" ? 0 : -1
    }
}
