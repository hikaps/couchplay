// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 CouchPlay Contributors

import QtQuick
import QtQuick.Layouts
import QtQuick.Controls as Controls
import org.kde.kirigami as Kirigami

/**
 * InfoCard - A simple card with a header title and content slot.
 * 
 * Uses AbstractCard to avoid content clipping issues with nested layouts.
 * 
 * Use for:
 * - System Requirements sections
 * - "How It Works" explanations
 * - Tips and hints
 * - Any informational content with a heading
 */
Kirigami.AbstractCard {
    id: root

    required property string title
    property int headingLevel: 3
    
    // Content slot - children go here
    default property alias content: contentContainer.data

    contentItem: ColumnLayout {
        spacing: Kirigami.Units.largeSpacing

        Kirigami.Heading {
            text: root.title
            level: root.headingLevel
        }

        ColumnLayout {
            id: contentContainer
            spacing: Kirigami.Units.largeSpacing
            Layout.fillWidth: true
        }
    }
}
