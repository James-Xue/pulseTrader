pragma Singleton

import QtQuick

/// Window-wide clipboard feedback channel.
///
/// `SelectableText` calls `notifyCopied(chars)` after writing to the system
/// clipboard; the root window listens and shows a transient "copied" toast.
/// A singleton keeps the signal global without threading a controller through
/// every page.
QtObject {
    id: root

    signal copied(int chars)

    function notifyCopied(chars) {
        if (chars > 0) {
            root.copied(chars)
        }
    }
}
