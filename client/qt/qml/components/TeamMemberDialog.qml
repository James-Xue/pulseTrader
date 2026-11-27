import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Pulse.Client

/// Skill-card style member details, opened by clicking a name on the Team page.
///
/// The GT team JSON carries three optional fields per member:
///   summary    — one-line skill description (brief mode)
///   details    — the full rule text (detailed mode)
///   indicators — short chips naming the indicators the member actually uses
/// Members without them fall back to the older `description` field.
Dialog {
    id: root

    property var member: ({})
    /// 0 = brief, 1 = detailed. Reset by the caller on every open.
    property int mode: 0

    modal: true
    padding: 0
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
    anchors.centerIn: Overlay.overlay
    width: Math.min(620, Overlay.overlay ? Overlay.overlay.width - 40 : 620)
    height: Math.min(540, Overlay.overlay ? Overlay.overlay.height - 40 : 540)

    background: Rectangle {
        color: Style.windowBackground
        radius: Style.radius
        border.width: 1
        border.color: Style.separator
    }

    contentItem: ColumnLayout {
        spacing: 0

        RowLayout {
            Layout.fillWidth: true
            Layout.margins: Style.pad
            spacing: Style.gap

            SelectableText {
                text: root.member.role === "leader"
                      ? qsTr("Leader")
                      : (root.member.member || "—")
                color: Style.text
                font.pixelSize: Style.titleSize
                font.weight: Font.DemiBold
                Layout.fillWidth: true
            }
            SelectableText {
                visible: root.member.role !== "leader"
                text: qsTr("Budget") + " "
                      + Number(root.member.budget || 0).toFixed(0)
                      + "  ·  " + qsTr("Risk/trade") + " "
                      + Number(root.member.risk_per_trade || 0).toFixed(2)
                color: Style.secondaryText
                font.pixelSize: Style.fontSizeSmall
            }
        }

        MacSegmentedControl {
            Layout.preferredWidth: 240
            Layout.alignment: Qt.AlignHCenter
            items: [qsTr("Brief"), qsTr("Detailed")]
            currentIndex: root.mode
            onActivated: function (index) { root.mode = index }
        }

        // Key indicators as compact chips; the same list in both modes.
        Flow {
            Layout.fillWidth: true
            Layout.leftMargin: Style.pad
            Layout.rightMargin: Style.pad
            Layout.topMargin: Style.gap
            spacing: Style.gapSmall

            Repeater {
                model: root.member.indicators || []
                delegate: Rectangle {
                    radius: Style.radiusSmall
                    color: Style.contentBackground
                    border.width: 1
                    border.color: Style.separator
                    implicitWidth: chipText.implicitWidth + 2 * Style.gapSmall
                    implicitHeight: chipText.implicitHeight + Style.gapSmall

                    SelectableText {
                        id: chipText
                        anchors.centerIn: parent
                        text: modelData
                        color: Style.secondaryText
                        font.pixelSize: Style.fontSizeSmall
                    }
                }
            }
        }

        ScrollView {
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.margins: Style.pad
            clip: true

            SelectableText {
                width: parent.width
                text: root.mode === 0
                      ? (root.member.summary || root.member.description || "")
                      : (root.member.details || root.member.description || "")
                color: Style.text
                font.pixelSize: Style.fontSize
                wrapMode: Text.WordWrap
            }
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.margins: Style.pad
            spacing: Style.gap
            Item { Layout.fillWidth: true }
            MacButton {
                text: qsTr("Close")
                onClicked: root.close()
            }
        }
    }
}
