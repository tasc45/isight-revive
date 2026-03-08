#!/bin/bash
# iSightRevive — Uninstall Script
set -e

PLUGIN_NAME="iSightRevive.plugin"
DAL_DIR="/Library/CoreMediaIO/Plug-Ins/DAL"

echo "iSightRevive Uninstaller"
echo "========================"
echo ""

if [ "$EUID" -ne 0 ]; then
    echo "This script requires root privileges."
    echo "Run: sudo ./uninstall.sh"
    exit 1
fi

if [ -d "$DAL_DIR/$PLUGIN_NAME" ]; then
    echo "Removing $DAL_DIR/$PLUGIN_NAME"
    rm -rf "$DAL_DIR/$PLUGIN_NAME"
    echo "  Done."
else
    echo "$PLUGIN_NAME is not installed."
fi

# Restart camera services
echo ""
echo "Restarting camera services..."
killall VDCAssistant 2>/dev/null || true
killall IIDCVideoAssistant 2>/dev/null || true
echo "  Done."

echo ""
echo "Uninstall complete. The iSightAudio.driver was left in place."
echo "To also remove audio: sudo rm -rf /Library/Audio/Plug-Ins/HAL/iSightAudio.driver"
