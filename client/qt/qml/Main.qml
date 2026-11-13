import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtCore
import Pulse.Client

/// Root window in a macOS-like shape.
///
/// The window is frameless so the sidebar, status toggles and window controls
/// can share a single chrome band. The three window buttons sit at the
/// top-right. Everything the native title bar used to provide is
/// reimplemented here: dragging
/// (startSystemMove), double-click to maximize, edge and corner resizing
/// (WindowResizer), and the three window buttons.
///
/// Three content states share the chrome: the local account forms while the
/// vault is locked, the exchange profile once it is unlocked, and the
/// dashboard shell when the control socket is Ready. The PULSE_CLIENT_HOST
/// auto-connect path skips the first two for captures and scripts.
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
        objectName: "engineController"
    }

    /// Local account vault and the engine process the client owns. Both are
    /// destroyed with the window, and the launcher stops the engine in its
    /// destructor.
    AccountService {
        id: accounts
    }

    EngineLauncher {
        id: launcher
    }

    /// Local market-data server for downstream consumers (the gold-trader
    /// bot). It listens on loopback only and polls the engine's control
    /// socket through its own RpcClient, so it stays independent of the UI's
    /// polling and lives as long as the window does.
    DataServer {
        id: dataServer
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

            Text {
                anchors.centerIn: parent
                text: root.title
                color: Style.secondaryText
                font.pixelSize: Style.fontSizeSmall
                font.weight: Font.DemiBold
            }

            Row {
                anchors.right: parent.right
                anchors.rightMargin: 14
                anchors.verticalCenter: parent.verticalCenter
                spacing: Style.gap

                StatusBar {
                    anchors.verticalCenter: parent.verticalCenter
                    controller: engine
                }

                LanguageToggle {
                    anchors.verticalCenter: parent.verticalCenter
                }

                ThemeToggle {
                    anchors.verticalCenter: parent.verticalCenter
                }

                // Window controls live at the top-right. The group keeps the
                // macOS close / minimize / maximize order and colours; only
                // the corner changed from the original frameless-mac layout.
                TrafficLights {
                    anchors.verticalCenter: parent.verticalCenter
                    target: root
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

        // -- Login, profile or shell ---------------------------------------
        //
        // Three states, in order: the vault is locked → account forms; the
        // vault is unlocked but no engine is attached → exchange profile;
        // the control socket is Ready → dashboard shell. The PULSE_CLIENT_HOST
        // auto-connect path jumps straight to the shell for captures.
        StackLayout {
            id: content
            Layout.fillWidth: true
            Layout.fillHeight: true
            currentIndex: root.showShell ? 2 : (accounts.unlocked ? 1 : 0)

            LoginPage {
                id: login
                account: accounts
            }

            ProfilePage {
                id: profile
                account: accounts
                launcher: launcher
                engine: engine
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
                        objectName: "pagesStack"
                        anchors.fill: parent

                        DashboardPage { controller: engine }
                        PositionsPage { controller: engine }
                        OrdersPage { controller: engine }
                        SignalsPage { controller: engine }
                        MarketPage { controller: engine }
                        GoldTradersPage {
                            controller: engine
                            dataServer: dataServer
                        }
                        SettingsPage {
                            controller: engine
                            account: accounts
                            launcher: launcher
                            dataServer: dataServer
                        }
                    }
                }
            }
        }
    }

    Component.onCompleted: {
        // Restore the saved scheme before the first frame so the window does
        // not flash light before switching to dark.
        Style.dark = appearance.dark

        // Bind the data-server port and start engine polling. Deferred to
        // Component.onCompleted (rather than a constructor) so a bind failure
        // is reported through the component's own signals.
        dataServer.start()
    }
}
