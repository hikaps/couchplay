// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2024 hikaps

import QtQuick
import QtQuick.Layouts
import QtQuick.Controls as Controls
import org.kde.kirigami as Kirigami

Kirigami.Dialog {
    id: root
    title: i18nc("@title:dialog", "Add Preset from Application")
    standardButtons: Kirigami.Dialog.Close
    preferredWidth: Kirigami.Units.gridUnit * 30
    preferredHeight: Kirigami.Units.gridUnit * 25

    required property var presetManager

    ColumnLayout {
        spacing: Kirigami.Units.largeSpacing

        Controls.TextField {
            id: appSearchField
            Layout.fillWidth: true
            placeholderText: i18nc("@info:placeholder", "Search applications...")
            onTextChanged: appListView.filterText = text.toLowerCase()
        }

        Controls.ScrollView {
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.preferredHeight: Kirigami.Units.gridUnit * 15

            ListView {
                id: appListView
                clip: true

                property string filterText: ""

                model: {
                    if (filterText === "") {
                        return root.presetManager.availableApplications
                    }
                    return root.presetManager.availableApplications.filter(function(app) {
                        return app.name.toLowerCase().includes(filterText)
                    })
                }

                delegate: Controls.ItemDelegate {
                    width: appListView.width
                    highlighted: ListView.isCurrentItem

                    contentItem: RowLayout {
                        spacing: Kirigami.Units.smallSpacing

                        Kirigami.Icon {
                            source: modelData.iconName || "application-x-executable"
                            Layout.preferredWidth: Kirigami.Units.iconSizes.medium
                            Layout.preferredHeight: Kirigami.Units.iconSizes.medium
                        }

                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 0

                            Controls.Label {
                                text: modelData.name
                                Layout.fillWidth: true
                                elide: Text.ElideRight
                            }

                            Controls.Label {
                                text: modelData.command
                                Layout.fillWidth: true
                                elide: Text.ElideMiddle
                                opacity: 0.7
                                font.pointSize: Kirigami.Theme.smallFont.pointSize
                            }
                        }

                        Controls.Button {
                            text: i18nc("@action:button", "Add")
                            icon.name: "list-add"
                            onClicked: {
                                let id = root.presetManager.addPresetFromDesktopFile(modelData.desktopFilePath)
                                if (id !== "") {
                                    applicationWindow().showPassiveNotification(
                                        i18nc("@info", "Added preset: %1", modelData.name))
                                    root.close()
                                } else {
                                    applicationWindow().showPassiveNotification(
                                        i18nc("@info", "Failed to add preset"), "long")
                                }
                            }
                        }
                    }
                }

                Kirigami.PlaceholderMessage {
                    anchors.centerIn: parent
                    visible: appListView.count === 0
                    text: appSearchField.text !== ""
                          ? i18nc("@info", "No applications match your search")
                          : i18nc("@info", "No applications found")
                    icon.name: "application-x-executable"
                }
            }
        }

        Kirigami.InlineMessage {
            Layout.fillWidth: true
            text: i18nc("@info", "Select an installed application to add as a launch preset. You can then select it when configuring session instances.")
            type: Kirigami.MessageType.Information
            visible: true
        }
    }
}