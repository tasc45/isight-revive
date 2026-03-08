#!/bin/bash
# iSightRevive — Install Script
set -e

PLUGIN_NAME="iSightRevive.plugin"
DAL_DIR="/Library/CoreMediaIO/Plug-Ins/DAL"
AUDIO_PLUGIN="iSightAudio.driver"
AUDIO_DIR="/Library/Audio/Plug-Ins/HAL"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

echo "iSightRevive Installer"
echo "======================"
echo ""

# Check for root
if [ "$EUID" -ne 0 ]; then
    echo "This script requires root privileges."
    echo "Run: sudo ./install.sh"
    exit 1
fi

# Determine plugin source
if [ -d "$SCRIPT_DIR/build/$PLUGIN_NAME" ]; then
    PLUGIN_SRC="$SCRIPT_DIR/build/$PLUGIN_NAME"
    echo "Installing from build directory..."
elif [ -d "$SCRIPT_DIR/Prebuilt/$PLUGIN_NAME" ]; then
    PLUGIN_SRC="$SCRIPT_DIR/Prebuilt/$PLUGIN_NAME"
    echo "Installing pre-built binary..."
else
    echo "Error: No plugin found. Run 'make' first, or place a pre-built binary in Prebuilt/"
    exit 1
fi

# Install DAL plugin
echo "Installing $PLUGIN_NAME to $DAL_DIR/"
mkdir -p "$DAL_DIR"
rm -rf "$DAL_DIR/$PLUGIN_NAME"
cp -R "$PLUGIN_SRC" "$DAL_DIR/$PLUGIN_NAME"
chmod -R 755 "$DAL_DIR/$PLUGIN_NAME"
echo "  Done."

# Install audio driver if present and not already installed
if [ -d "$SCRIPT_DIR/Drivers/$AUDIO_PLUGIN" ] && [ ! -d "$AUDIO_DIR/$AUDIO_PLUGIN" ]; then
    echo ""
    echo "Installing $AUDIO_PLUGIN to $AUDIO_DIR/"
    mkdir -p "$AUDIO_DIR"
    cp -R "$SCRIPT_DIR/Drivers/$AUDIO_PLUGIN" "$AUDIO_DIR/$AUDIO_PLUGIN"
    chmod -R 755 "$AUDIO_DIR/$AUDIO_PLUGIN"
    echo "  Done."
elif [ -d "$AUDIO_DIR/$AUDIO_PLUGIN" ]; then
    echo ""
    echo "$AUDIO_PLUGIN already installed — skipping."
fi

# Kill existing camera assistants to force reload
echo ""
echo "Restarting camera services..."
killall VDCAssistant 2>/dev/null || true
killall IIDCVideoAssistant 2>/dev/null || true
echo "  Done."

echo ""
echo "Installation complete!"
echo ""
echo "Open FaceTime, Photo Booth, or any camera app — you should see 'iSight' in the camera list."
echo "To check logs: log show --predicate 'subsystem == \"com.isightrevive.dal\"' --last 1m"
