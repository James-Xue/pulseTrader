import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Pulse.Client

/// On-demand market snapshot for one symbol.
///
/// This is the only read-only method that takes parameters, so it is fetched
/// explicitly instead of being polled.
ColumnLayout {
    id: root

    property var controller
    property var snapshot: ({})

    spacing: 0

    Connections {
        target: root.controller
        function onMarketReceived(market) {
            root.snapshot = market
        }
    }

    RowLayout {
        Layout.fillWidth: true
        Layout.margins: Style.pad
        spacing: Style.gap

        ComboBox {
            id: symbolBox
            Layout.preferredWidth: 220
            model: root.controller ? root.controller.symbols : []
        }

        TextField {
            id: symbolField
            Layout.preferredWidth: 180
            placeholderText: qsTr("or type a symbol")
        }

        Button {
            text: qsTr("Fetch")
            enabled: root.controller && root.controller.connectionState === EngineController.Ready
            onClicked: {
                const symbol = symbolField.text.length > 0
                             ? symbolField.text
                             : (symbolBox.currentText || "")
                if (root.controller && symbol.length > 0) {
                    root.controller.fetchMarket(symbol)
                }
            }
        }

        Item { Layout.fillWidth: true }
    }

    ScrollView {
        Layout.fillWidth: true
        Layout.fillHeight: true
        Layout.leftMargin: Style.pad
        Layout.rightMargin: Style.pad
        Layout.bottomMargin: Style.pad

        TextArea {
            readOnly: true
            wrapMode: TextArea.WrapAnywhere
            color: Style.text
            font.family: "monospace"
            font.pixelSize: 11
            text: Object.keys(root.snapshot).length === 0
                  ? qsTr("No snapshot loaded. Pick a symbol and press Fetch.")
                  : JSON.stringify(root.snapshot, null, 2)
        }
    }
}
