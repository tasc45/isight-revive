#!/bin/bash
# iSight Revive — Uninstaller
# Removes all installed components and restores system defaults
#
# Usage: sudo ./uninstall.sh
set -e

echo ""
echo "  iSight Revive — Uninstaller"
echo "  ============================"
echo ""

if [ "$EUID" -ne 0 ]; then
    echo "This script requires root privileges."
    echo "Run: sudo ./uninstall.sh"
    exit 1
fi

# Stop daemons
echo "Stopping daemons..."
launchctl unload /Library/LaunchDaemons/com.apple.cmio.IIDCVideoAssistant.plist 2>/dev/null || true
launchctl unload /Library/LaunchDaemons/com.isight-revive.startup.plist 2>/dev/null || true
killall IIDCVideoAssistant 2>/dev/null || true

# Remove installed files
echo "Removing installed files..."
rm -f /usr/local/lib/libfwafix.dylib
rm -f /usr/local/libexec/IIDCVideoAssistant
rm -f /usr/local/libexec/IIDC.plugin
rm -f /usr/local/libexec/isight-startup.sh
rm -f /Library/LaunchDaemons/com.apple.cmio.IIDCVideoAssistant.plist
rm -f /Library/LaunchDaemons/com.isight-revive.startup.plist

# Restore audio driver if we wrapped it
AUDIO_DIR="/Library/Audio/Plug-Ins/HAL/iSightAudio.driver/Contents/MacOS"
if [ -f "$AUDIO_DIR/iSightAudio.orig" ]; then
    echo "Restoring original audio driver..."
    mv "$AUDIO_DIR/iSightAudio.orig" "$AUDIO_DIR/iSightAudio"
fi

# Re-enable disabled DAL plugins
for plugin in /Library/CoreMediaIO/Plug-Ins/DAL/*.disabled; do
    if [ -d "$plugin" ]; then
        newname="${plugin%.disabled}"
        mv "$plugin" "$newname" 2>/dev/null || true
    fi
done

# Restart audio
killall coreaudiod 2>/dev/null || true

echo ""
echo "  Uninstall complete."
echo "  The FireWire iSight will no longer work until you re-install."
echo ""
