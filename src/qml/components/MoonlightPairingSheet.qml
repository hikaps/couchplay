// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 CouchPlay Contributors

import QtQuick
import QtQuick.Layouts
import QtQuick.Controls as Controls
import org.kde.kirigami as Kirigami

/**
 * MoonlightPairingSheet - pair Moonlight clients with a player's Sunshine instance.
 *
 * Polls pending pairings while open. Newer Sunshine lists pending pairing requests (one PIN field
 * each); older Sunshine only accepts a PIN plus a client name (legacy mode).
 */
Kirigami.OverlaySheet {
    id: root
    objectName: "moonlightPairingSheet"

    required property var streamManager
    required property int instanceIndex

    // Entry of streamManager.streams for this player
    readonly property var entry: {
        const list = streamManager ? streamManager.streams : []
        for (let i = 0; i < list.length; ++i) {
            if (list[i].instanceIndex === instanceIndex) return list[i]
        }
        return null
    }
    readonly property bool legacy: entry !== null && entry.legacyPairing
    readonly property var pendingPairings: entry !== null ? entry.pendingPairings : []
    readonly property var pairedClients: entry !== null ? entry.clients : []

    property string resultMessage: ""
    property bool resultOk: false

    readonly property string textTitle: i18nc("@title:window", "Pair Moonlight")
    readonly property string textHostHint: entry !== null
        ? i18nc("@info", "Add host %1:%2 in Moonlight if it isn't discovered automatically", streamManager.hostAddress, entry.port)
        : ""
    readonly property string textEnterPin: i18nc("@info", "Start pairing in Moonlight, then enter the PIN it shows.")
    readonly property string textPinPlaceholder: i18nc("@info:placeholder", "4-digit PIN")
    readonly property string textNamePlaceholder: i18nc("@info:placeholder", "Client name")
    readonly property string textPair: i18nc("@action:button", "Pair")
    readonly property string textUnpair: i18nc("@action:button", "Unpair")
    readonly property string textNoPending: i18nc("@info", "No pairing requests yet")
    readonly property string textPairedClients: i18nc("@title:group", "Paired clients")
    readonly property string textNoClients: i18nc("@info", "No clients paired")
    readonly property string textPaired: i18nc("@info", "Client paired")

    title: textTitle

    onOpened: {
        resultMessage = ""
        streamManager.setPairingActive(instanceIndex, true)
    }
    onClosed: streamManager.setPairingActive(instanceIndex, false)

    Connections {
        target: root.streamManager

        function onPinResult(index, ok, message) {
            if (index !== root.instanceIndex) return
            root.resultOk = ok
            root.resultMessage = ok ? root.textPaired : message
        }
    }

    ColumnLayout {
        spacing: Kirigami.Units.largeSpacing

        Controls.Label {
            objectName: "labelPairingHostHint"
            text: root.textHostHint
            wrapMode: Text.WordWrap
            Layout.fillWidth: true
        }

        Kirigami.InlineMessage {
            objectName: "messagePairingResult"
            Layout.fillWidth: true
            visible: root.resultMessage.length > 0
            type: root.resultOk ? Kirigami.MessageType.Positive : Kirigami.MessageType.Error
            text: root.resultMessage
        }

        // Newer Sunshine: one row per pending pairing request
        Repeater {
            model: root.legacy ? [] : root.pendingPairings

            delegate: RowLayout {
                id: pairingRow

                required property var modelData

                Layout.fillWidth: true
                spacing: Kirigami.Units.smallSpacing

                Controls.Label {
                    text: pairingRow.modelData.name + " (" + pairingRow.modelData.address + ")"
                    elide: Text.ElideRight
                    Layout.fillWidth: true
                }

                Controls.TextField {
                    id: pinField
                    objectName: "fieldPairingPin"
                    Accessible.name: root.textPinPlaceholder
                    placeholderText: root.textPinPlaceholder
                    inputMethodHints: Qt.ImhDigitsOnly
                    validator: RegularExpressionValidator { regularExpression: /^[0-9]{4}$/ }
                    Layout.preferredWidth: Kirigami.Units.gridUnit * 7
                }

                Controls.Button {
                    objectName: "buttonPair"
                    Accessible.role: Accessible.Button
                    Accessible.name: root.textPair
                    text: root.textPair
                    enabled: pinField.acceptableInput
                    onClicked: root.streamManager.submitPin(root.instanceIndex, pairingRow.modelData.id,
                                                            pinField.text, pairingRow.modelData.name)
                }
            }
        }

        Controls.Label {
            visible: !root.legacy && root.pendingPairings.length === 0
            text: root.textNoPending
            opacity: 0.7
            Layout.fillWidth: true
        }

        // Older Sunshine: PIN and client name are entered manually
        ColumnLayout {
            visible: root.legacy
            Layout.fillWidth: true
            spacing: Kirigami.Units.smallSpacing

            Controls.Label {
                text: root.textEnterPin
                wrapMode: Text.WordWrap
                Layout.fillWidth: true
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: Kirigami.Units.smallSpacing

                Controls.TextField {
                    id: legacyNameField
                    objectName: "fieldPairingClientName"
                    Accessible.name: root.textNamePlaceholder
                    placeholderText: root.textNamePlaceholder
                    Layout.fillWidth: true
                }

                Controls.TextField {
                    id: legacyPinField
                    objectName: "fieldPairingPin"
                    Accessible.name: root.textPinPlaceholder
                    placeholderText: root.textPinPlaceholder
                    inputMethodHints: Qt.ImhDigitsOnly
                    validator: RegularExpressionValidator { regularExpression: /^[0-9]{4}$/ }
                    Layout.preferredWidth: Kirigami.Units.gridUnit * 7
                }

                Controls.Button {
                    objectName: "buttonPair"
                    Accessible.role: Accessible.Button
                    Accessible.name: root.textPair
                    text: root.textPair
                    enabled: legacyPinField.acceptableInput && legacyNameField.text.length > 0
                    onClicked: root.streamManager.submitPin(root.instanceIndex, "", legacyPinField.text,
                                                            legacyNameField.text)
                }
            }
        }

        Kirigami.Heading {
            level: 4
            text: root.textPairedClients
            Layout.fillWidth: true
        }

        Repeater {
            model: root.pairedClients

            delegate: RowLayout {
                id: clientRow

                required property var modelData

                Layout.fillWidth: true
                spacing: Kirigami.Units.smallSpacing

                Controls.Label {
                    text: clientRow.modelData.name
                    elide: Text.ElideRight
                    Layout.fillWidth: true
                }

                Controls.Button {
                    objectName: "buttonUnpair"
                    Accessible.role: Accessible.Button
                    Accessible.name: root.textUnpair
                    text: root.textUnpair
                    icon.name: "list-remove"
                    onClicked: root.streamManager.unpairClient(root.instanceIndex, clientRow.modelData.uuid)
                }
            }
        }

        Controls.Label {
            visible: root.pairedClients.length === 0
            text: root.textNoClients
            opacity: 0.7
            Layout.fillWidth: true
        }
    }
}
