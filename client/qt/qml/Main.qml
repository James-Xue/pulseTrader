import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtCore
import Pulse.Client

/// Root window in a macOS-like shape.
///
/// The window is frameless so the traffic lights and the sidebar can sit in a
/// single chrome band, the way a native macOS app looks. Everything the native
/// title bar used to provide is reimplemented here: dragging
/// (startSystemMove), double-click to maximize, edge and corner resizing
/// (WindowResizer), and the three window buttons.
///
/// Two content states share the chrome: the login view until the control
/// socket is Ready, and the dashboard shell after that. The login view is
/// skipped entirely when PULSE_CLIENT_HOST started the auto-connect path.
ApplicationWindow {
    id: root

    width: 1100
    height: 720
    minimumWidth: 860
    minimumHeight: 560
    visible: true
    title: qsTr("pulseTrader")
    // Transparent so the rounded chrome below defines the visible shape; the
    // compositor composites the four corners away.
    color: "transparent"
    flags: Qt.Window | Qt.FramelessWindowHint

    /// Controls inherit this palette, so TextField/CheckBox/Switch follow the
    /// colour scheme instead of staying light in dark mode.
    palette.window: Style.windowBackground
    palette.windowText: Style.text
    palette.base: Style.controlBackground
    palette.alternateBase: Style.controlBackgroundHover
    palette.text: Style.text
    palette.button: Style.controlBackground
    palette.buttonText: Style.text
    palette.highlight: Style.accent
    palette.highlightedText: "white"
    palette.placeholderText: Style.tertiaryText
    palette.toolTipBase: Style.controlBackground
    palette.toolTipText: Style.text

    /// Show the shell once the aggregate connection state is Ready. The
    /// auto-connect path (PULSE_CLIENT_HOST) keeps the shell visible from the
    /// first frame so headless captures and scripts never record the login
    /// form; the login view is only for interactive launches.
    readonly property bool showShell: engine.autoConnect
                                      || engine.connectionState === EngineController.Ready

    EngineController {
        id: engine
    }

    /// Colour scheme persistence. Style is a singleton, so the value is copied
    /// in at startup and written back whenever the toggle flips it. The stored
    /// default is dark; a user who picks light keeps that choice across runs.
    Settings {
        id: appearance
        category: "appearance"
        property bool dark: true
    }

    Connections {
        target: Style
        function onDarkChanged() { appearance.dark = Style.dark }
    }

    // Resize zones sit above the content so a click near an edge never lands
    // in a page.
    WindowResizer {
        anchors.fill: parent
        target: root
        z: 100
    }

    // Hairline around the rounded chrome so the window stays defined against a
    // light wallpaper or another window. A plain Rectangle takes no input, so
    // it does not block clicks.
    Rectangle {
        anchors.fill: parent
        radius: Style.windowRadius
        color: "transparent"
        border.width: 1
        border.color: Style.separator
        z: 2
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        // -- Title bar ------------------------------------------------------
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: Style.titleBarHeight
            color: Style.windowBackground
            // Owns the two upper corners of the window.
            topLeftRadius: Style.windowRadius
            topRightRadius: Style.windowRadius

            MouseArea {
                anchors.fill: parent
                onPressed: root.startSystemMove()
                onDoubleClicked: {
                    if (root.visibility === Window.Maximized) {
                        root.showNormal()
                    } else {
                        root.showMaximized()
                    }
                }
            }

            TrafficLights {
                anchors.left: parent.left
                anchors.leftMargin: 14
                anchors.verticalCenter: parent.verticalCenter
                target: root
            }

            Text {
                anchors.centerIn: parent
                text: root.title
                color: Style.secondaryText
                font.pixelSize: Style.fontSizeSmall
                font.weight: Font.DemiBold
            }

            Row {
                anchors.right: parent.right
                anchors.rightMargin: Style.pad
                anchors.verticalCenter: parent.verticalCenter
                spacing: Style.gap

                StatusBar {
                    anchors.verticalCenter: parent.verticalCenter
                    controller: engine
                }

                ThemeToggle {
                    anchors.verticalCenter: parent.verticalCenter
                }
            }
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 1
            color: Style.separator
        }

        ErrorBanner {
            Layout.fillWidth: true
            message: engine.lastError
            visible: root.showShell && engine.lastError.length > 0
        }

        // -- Login or shell -------------------------------------------------
        StackLayout {
            id: content
            Layout.fillWidth: true
            Layout.fillHeight: true
            currentIndex: root.showShell ? 1 : 0

            LoginPage {
                id: login
                controller: engine
            }

            // -- Sidebar + pages --------------------------------------------
            RowLayout {
                Layout.fillWidth: true
                Layout.fillHeight: true
                spacing: 0

                Sidebar {
                    Layout.preferredWidth: Style.sidebarWidth
                    Layout.fillHeight: true
                    // Owns the lower-left corner.
                    bottomLeftRadius: Style.windowRadius
                    currentIndex: pages.currentIndex
                    onSelected: function (index) { pages.currentIndex = index }
                }

                Rectangle {
                    Layout.preferredWidth: 1
                    Layout.fillHeight: true
                    color: Style.separator
                }

                // Owns the lower-right corner. The pages draw on the window
                // background so the white card surfaces stay distinguishable.
                Rectangle {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    color: Style.windowBackground
                    bottomRightRadius: Style.windowRadius

                    StackLayout {
                        id: pages
                        anchors.fill: parent

                        DashboardPage { controller: engine }
                        PositionsPage { controller: engine }
                        OrdersPage { controller: engine }
                        SignalsPage { controller: engine }
                        MarketPage { controller: engine }
                        SettingsPage { controller: engine }
                    }
                }
            }
        }
    }

    Component.onCompleted: {
        // Restore the saved scheme before the first frame so the window does
        // not flash light before switching to dark.
        Style.dark = appearance.dark
    }
}
