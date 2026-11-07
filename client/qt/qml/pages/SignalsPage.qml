import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Pulse.Client

/// Latest strategy signal per source, plus the aggregator consensus row when
/// the engine publishes one.
ColumnLayout {
    id: root

    property var controller

    spacing: 0

    RowLayout {
        Layout.fillWidth: true
        Layout.margins: Style.pad
        spacing: Style.gap

        Text {
            text: qsTr("Signals: %1").arg(
                      root.controller ? root.controller.signalsModel.rowCount() : 0)
            color: Style.text
            font.pixelSize: Style.fontSize
        }

        Item { Layout.fillWidth: true }

        Button {
            text: qsTr("Refresh")
            onClicked: if (root.controller) root.controller.refreshAll()
        }
    }

    ListView {
        id: list
        Layout.fillWidth: true
        Layout.fillHeight: true
        Layout.leftMargin: Style.pad
        Layout.rightMargin: Style.pad
        Layout.bottomMargin: Style.pad
        spacing: 8
        clip: true
        model: root.controller ? root.controller.signalsModel : null

        delegate: SignalCard {
            width: list.width
            source: model.source === undefined ? "" : model.source
            signalType: model.type === undefined ? "" : model.type
            confidence: model.confidence === undefined ? 0 : Number(model.confidence)
            reason: model.reason === undefined ? "" : model.reason
            publishedAt: model.ts_str === undefined ? "" : model.ts_str
        }

        EmptyState {
            visible: list.count === 0
            text: qsTr("No signals published yet")
        }
    }
}
