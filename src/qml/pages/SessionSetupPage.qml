// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2024 hikaps

import QtQuick
import QtQuick.Layouts
import QtQuick.Controls as Controls
import org.kde.kirigami as Kirigami

Kirigami.ScrollablePage {
    id: root

    title: i18nc("@title", "New Session")

    property int instanceCount: 2
    property string layoutMode: "horizontal"

    actions: [
        Kirigami.Action {
            icon.name: "go-next"
            text: i18nc("@action:button", "Next: Assign Devices")
            onTriggered: {
                applicationWindow().pageStack.push(deviceAssignmentPage)
            }
        }
    ]

    ColumnLayout {
        spacing: Kirigami.Units.largeSpacing

        // Layout Selection
        Kirigami.Heading {
            text: i18nc("@title", "Screen Layout")
            level: 2
        }

        Controls.Label {
            text: i18nc("@info", "Choose how to arrange the game instances on your screen.")
            wrapMode: Text.WordWrap
            Layout.fillWidth: true
            opacity: 0.7
        }

        RowLayout {
            spacing: Kirigami.Units.largeSpacing
            Layout.fillWidth: true
            Layout.topMargin: Kirigami.Units.smallSpacing

            // Horizontal split
            Kirigami.Card {
                Layout.fillWidth: true
                Layout.preferredHeight: Kirigami.Units.gridUnit * 10

                highlighted: layoutMode === "horizontal"

                contentItem: ColumnLayout {
                    spacing: Kirigami.Units.smallSpacing

                    // Visual representation
                    Rectangle {
                        Layout.fillWidth: true
                        Layout.preferredHeight: Kirigami.Units.gridUnit * 4
                        color: "transparent"
                        border.color: Kirigami.Theme.textColor
                        border.width: 1
                        radius: 4

                        RowLayout {
                            anchors.fill: parent
                            anchors.margins: 2
                            spacing: 2

                            Rectangle {
                                Layout.fillWidth: true
                                Layout.fillHeight: true
                                color: Kirigami.Theme.highlightColor
                                opacity: 0.5
                                radius: 2

                                Controls.Label {
                                    anchors.centerIn: parent
                                    text: "1"
                                    font.bold: true
                                }
                            }

                            Rectangle {
                                Layout.fillWidth: true
                                Layout.fillHeight: true
                                color: Kirigami.Theme.positiveBackgroundColor
                                opacity: 0.5
                                radius: 2

                                Controls.Label {
                                    anchors.centerIn: parent
                                    text: "2"
                                    font.bold: true
                                }
                            }
                        }
                    }

                    Controls.Label {
                        text: i18nc("@option", "Side by Side")
                        font.bold: true
                        Layout.alignment: Qt.AlignHCenter
                    }

                    Controls.Label {
                        text: i18nc("@info", "Split screen horizontally")
                        Layout.alignment: Qt.AlignHCenter
                        opacity: 0.7
                    }
                }

                onClicked: layoutMode = "horizontal"
            }

            // Vertical split
            Kirigami.Card {
                Layout.fillWidth: true
                Layout.preferredHeight: Kirigami.Units.gridUnit * 10

                highlighted: layoutMode === "vertical"

                contentItem: ColumnLayout {
                    spacing: Kirigami.Units.smallSpacing

                    Rectangle {
                        Layout.fillWidth: true
                        Layout.preferredHeight: Kirigami.Units.gridUnit * 4
                        color: "transparent"
                        border.color: Kirigami.Theme.textColor
                        border.width: 1
                        radius: 4

                        ColumnLayout {
                            anchors.fill: parent
                            anchors.margins: 2
                            spacing: 2

                            Rectangle {
                                Layout.fillWidth: true
                                Layout.fillHeight: true
                                color: Kirigami.Theme.highlightColor
                                opacity: 0.5
                                radius: 2

                                Controls.Label {
                                    anchors.centerIn: parent
                                    text: "1"
                                    font.bold: true
                                }
                            }

                            Rectangle {
                                Layout.fillWidth: true
                                Layout.fillHeight: true
                                color: Kirigami.Theme.positiveBackgroundColor
                                opacity: 0.5
                                radius: 2

                                Controls.Label {
                                    anchors.centerIn: parent
                                    text: "2"
                                    font.bold: true
                                }
                            }
                        }
                    }

                    Controls.Label {
                        text: i18nc("@option", "Top and Bottom")
                        font.bold: true
                        Layout.alignment: Qt.AlignHCenter
                    }

                    Controls.Label {
                        text: i18nc("@info", "Split screen vertically")
                        Layout.alignment: Qt.AlignHCenter
                        opacity: 0.7
                    }
                }

                onClicked: layoutMode = "vertical"
            }

            // Multi-monitor
            Kirigami.Card {
                Layout.fillWidth: true
                Layout.preferredHeight: Kirigami.Units.gridUnit * 10

                highlighted: layoutMode === "multi-monitor"

                contentItem: ColumnLayout {
                    spacing: Kirigami.Units.smallSpacing

                    RowLayout {
                        Layout.fillWidth: true
                        Layout.preferredHeight: Kirigami.Units.gridUnit * 4
                        spacing: Kirigami.Units.smallSpacing

                        Rectangle {
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            color: "transparent"
                            border.color: Kirigami.Theme.textColor
                            border.width: 1
                            radius: 4

                            Rectangle {
                                anchors.fill: parent
                                anchors.margins: 2
                                color: Kirigami.Theme.highlightColor
                                opacity: 0.5
                                radius: 2

                                Controls.Label {
                                    anchors.centerIn: parent
                                    text: "1"
                                    font.bold: true
                                }
                            }
                        }

                        Rectangle {
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            color: "transparent"
                            border.color: Kirigami.Theme.textColor
                            border.width: 1
                            radius: 4

                            Rectangle {
                                anchors.fill: parent
                                anchors.margins: 2
                                color: Kirigami.Theme.positiveBackgroundColor
                                opacity: 0.5
                                radius: 2

                                Controls.Label {
                                    anchors.centerIn: parent
                                    text: "2"
                                    font.bold: true
                                }
                            }
                        }
                    }

                    Controls.Label {
                        text: i18nc("@option", "Multi-Monitor")
                        font.bold: true
                        Layout.alignment: Qt.AlignHCenter
                    }

                    Controls.Label {
                        text: i18nc("@info", "One instance per monitor")
                        Layout.alignment: Qt.AlignHCenter
                        opacity: 0.7
                    }
                }

                onClicked: layoutMode = "multi-monitor"
            }
        }

        // Instance Configuration
        Kirigami.Heading {
            text: i18nc("@title", "Instance Configuration")
            level: 2
            Layout.topMargin: Kirigami.Units.largeSpacing
        }

        Repeater {
            model: instanceCount

            delegate: Kirigami.Card {
                Layout.fillWidth: true

                header: Kirigami.Heading {
                    text: i18nc("@title", "Instance %1", index + 1)
                    level: 3
                    padding: Kirigami.Units.smallSpacing
                }

                contentItem: Kirigami.FormLayout {
                    Controls.ComboBox {
                        Kirigami.FormData.label: i18nc("@label", "User:")
                        model: ["Current User", "player2"]
                        Layout.fillWidth: true
                    }

                    Controls.ComboBox {
                        Kirigami.FormData.label: i18nc("@label", "Monitor:")
                        model: ["Primary Monitor", "Secondary Monitor"]
                        Layout.fillWidth: true
                    }

                    RowLayout {
                        Kirigami.FormData.label: i18nc("@label", "Resolution:")
                        spacing: Kirigami.Units.smallSpacing

                        Controls.SpinBox {
                            from: 640
                            to: 3840
                            value: 1920
                            stepSize: 10
                        }

                        Controls.Label {
                            text: "x"
                        }

                        Controls.SpinBox {
                            from: 480
                            to: 2160
                            value: 1080
                            stepSize: 10
                        }
                    }

                    Controls.SpinBox {
                        Kirigami.FormData.label: i18nc("@label", "Refresh Rate:")
                        from: 30
                        to: 240
                        value: 60
                        textFromValue: function(value) { return value + " Hz" }
                    }
                }
            }
        }
    }
}
