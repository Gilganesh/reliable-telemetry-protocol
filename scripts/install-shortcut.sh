#!/usr/bin/env bash
set -eu
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DESKTOP_DIR="$(xdg-user-dir DESKTOP 2>/dev/null || true)"
[ -z "$DESKTOP_DIR" ] && DESKTOP_DIR="$HOME/Desktop"
APPS_DIR="$HOME/.local/share/applications"
FILE_NAME="telemetry-dashboard.desktop"

mkdir -p "$DESKTOP_DIR" "$APPS_DIR"
chmod +x "$ROOT/run.sh"

cat > "$DESKTOP_DIR/$FILE_NAME" <<DESKTOP
[Desktop Entry]
Type=Application
Version=1.0
Name=Telemetry Dashboard
Comment=Start the broker, gateway and web dashboard
Path=$ROOT
Exec=bash -c "./run.sh --open; echo; read -r -p 'Stopped. Press Enter to close. ' _"
Icon=$ROOT/scripts/icon.svg
Terminal=true
Categories=Utility;
DESKTOP

chmod +x "$DESKTOP_DIR/$FILE_NAME"
gio set "$DESKTOP_DIR/$FILE_NAME" metadata::trusted true 2>/dev/null || true
cp "$DESKTOP_DIR/$FILE_NAME" "$APPS_DIR/$FILE_NAME"

echo "Shortcut created: $DESKTOP_DIR/$FILE_NAME"
echo "Double-click it to start everything. If the desktop asks what to do with the file, choose Execute."
