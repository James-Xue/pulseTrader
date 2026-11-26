import QtQuick
import QtQuick.Controls
import Pulse.Client

/// Read-only text that the user can select and copy.
///
/// QML's `Text` draws glyphs only: it has no selection model and no clipboard
/// bridge. This item is a `TextEdit` configured as a display element instead:
/// 1. `readOnly` keeps the content immutable while the editor still owns a
///    cursor and a selection model.
/// 2. `selectByMouse` + `persistentSelection` allow drag-selection that stays
///    visible after focus moves elsewhere (typical while reading values).
/// 3. Selecting text copies it to the clipboard automatically (debounced until
///    the drag settles); Ctrl+C and the right-click menu remain available.
/// 4. `elide` is accepted for source compatibility with `Text` call sites but
///    has no effect - TextEdit cannot elide. `clip` truncates instead.
///
/// Use this in place of `Text` for any value the user may want to copy. Keep
/// plain `Text` inside controls (buttons, switches, combo boxes), where the
/// element is part of the control's rendering and selection would fight the
/// control's own mouse handling.
TextEdit {
    id: root

    readOnly: true
    selectByMouse: true
    selectByKeyboard: true
    persistentSelection: true
    activeFocusOnPress: true
    cursorVisible: false
    textFormat: TextEdit.PlainText
    wrapMode: TextEdit.NoWrap
    color: Style.text
    selectionColor: Style.selection
    selectedTextColor: "white"
    padding: 0
    clip: true

    /// Compatibility shim: `Text` call sites set `elide`, which does not exist
    /// on TextEdit. Accepting the assignment keeps those call sites unchanged;
    /// the text is clipped instead of elided.
    property int elide: Text.ElideNone

    // Copy-as-you-select. The timer restarts while the drag is still moving,
    // so the clipboard receives the final selection rather than every
    // intermediate one; an empty selection never overwrites the clipboard.
    Timer {
        id: autoCopy
        interval: 300
        repeat: false
        onTriggered: {
            if (root.selectedText.length > 0) {
                root.copy()
            }
        }
    }

    onSelectedTextChanged: {
        if (selectedText.length > 0) {
            autoCopy.restart()
        } else {
            autoCopy.stop()
        }
    }

    // Right-click opens the copy menu. The MouseArea accepts only the right
    // button, so left-button drags fall through to TextEdit for selection.
    MouseArea {
        anchors.fill: parent
        acceptedButtons: Qt.RightButton
        onClicked: function (mouse) {
            contextMenu.popup(root, mouse.x, mouse.y)
        }
    }

    Menu {
        id: contextMenu

        MenuItem {
            text: qsTr("Copy")
            enabled: root.selectedText.length > 0
            onTriggered: root.copy()
        }
        MenuItem {
            text: qsTr("Select All")
            onTriggered: root.selectAll()
        }
    }
}
