#!/bin/sh
# Install the desktop entry and icon for a user-level (no sudo) launch.
#
# GNOME on Wayland does not read QGuiApplication::setWindowIcon for the dock
# icon: it resolves the icon from the .desktop file whose basename matches the
# window's app_id / desktop file name. The application therefore sets
# QGuiApplication::setDesktopFileName("pulsetrader"), and this script installs
# pulsetrader.desktop plus a hicolor icon so the two agree.
#
# Usage: packaging/linux/install-desktop.sh [path-to-binary]
#   Default binary: ../../build_qt/pulseTraderQt relative to this script.

set -eu

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
CLIENT_DIR=$(cd "$SCRIPT_DIR/../.." && pwd)

BINARY=${1:-"$CLIENT_DIR/../../build_qt/pulseTraderQt"}

if [ ! -x "$BINARY" ]; then
    echo "error: binary not found or not executable: $BINARY" >&2
    echo "build first:  cmake --build build_qt" >&2
    exit 1
fi

BINARY=$(cd "$(dirname "$BINARY")" && pwd)/$(basename "$BINARY")

APPS_DIR="$HOME/.local/share/applications"
ICONS_DIR="$HOME/.local/share/icons/hicolor/scalable/apps"
mkdir -p "$APPS_DIR" "$ICONS_DIR"

sed "s|@EXEC@|$BINARY|" "$SCRIPT_DIR/pulsetrader.desktop.in" \
    > "$APPS_DIR/pulsetrader.desktop"
chmod 644 "$APPS_DIR/pulsetrader.desktop"

cp "$CLIENT_DIR/resources/icons/app.svg" "$ICONS_DIR/pulsetrader.svg"
chmod 644 "$ICONS_DIR/pulsetrader.svg"

# Refresh the desktop database. The icon cache is deliberately NOT built here:
# ~/.local/share/icons/hicolor has no index.theme, and gtk-update-icon-cache
# then writes an empty cache that hides the directory from some lookups.
if command -v update-desktop-database >/dev/null 2>&1; then
    update-desktop-database "$APPS_DIR" >/dev/null 2>&1 || true
fi
rm -f "$HOME/.local/share/icons/hicolor/icon-theme.cache"

echo "installed:"
echo "  $APPS_DIR/pulsetrader.desktop"
echo "  $ICONS_DIR/pulsetrader.svg"
echo "launch from the app grid, or run: $BINARY"
