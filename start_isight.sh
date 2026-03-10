#!/bin/bash
set -e

echo "=== iSight Deploy ==="

# 1. Kill any running IIDCVideoAssistant
sudo killall -9 IIDCVideoAssistant 2>/dev/null || true
sleep 1

# 2. Disable + bootout the SYSTEM service so it doesn't respawn
sudo launchctl disable system/com.apple.cmio.IIDCVideoAssistant 2>/dev/null || true
sudo launchctl bootout system/com.apple.cmio.IIDCVideoAssistant 2>/dev/null || true
sleep 1

# Verify it's dead
if pgrep -x IIDCVideoAssistant >/dev/null 2>&1; then
    echo "Still running, force kill..."
    sudo kill -9 $(pgrep -x IIDCVideoAssistant) 2>/dev/null || true
    sleep 1
fi
echo "✓ Stock daemon killed"

# 3. Bus reset to reclaim IRM bandwidth
echo "=== Bus reset ==="
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
"$SCRIPT_DIR/bus_reset" 2>/dev/null && echo "✓ Bus reset" || echo "⚠ Bus reset failed (build bus_reset first)"

# 4. Clear old log
sudo rm -f /tmp/fwafix.log

# 5. Launch patched binary as _cmiodalassistants (root gets kIOReturnNotPermitted on FireWire)
#    TCC patches in binary handle permission checks for this user
echo "=== Launching patched IIDCVideoAssistant as _cmiodalassistants ==="
sudo -u _cmiodalassistants DYLD_INSERT_LIBRARIES=/usr/local/lib/libfwafix.dylib /usr/local/libexec/IIDCVideoAssistant &
DAEMON_PID=$!
echo "Launched PID=$DAEMON_PID"

sleep 3

# 6. Check log
echo "=== fwafix log ==="
cat /tmp/fwafix.log 2>/dev/null || echo "(no log)"

# 7. Open Photo Booth to trigger camera
echo "=== Opening Photo Booth ==="
open -a "Photo Booth"
sleep 5

# 8. Check for DMA success
echo "=== DMA check ==="
if grep -qE "DMA Start SUCCEEDED|ScalarMethod\(sel=0x20031\) -> 0x0" /tmp/fwafix.log 2>/dev/null; then
    echo "★ DMA SUCCEEDED! ★"
else
    echo "Checking full log..."
    cat /tmp/fwafix.log 2>/dev/null | tail -20
fi

# 9. Restart avconferenced for FaceTime
echo "=== Restart avconferenced ==="
sudo kill $(pgrep avconferenced) 2>/dev/null || true

echo ""
echo "=== Done. Check Photo Booth for video. ==="
echo "Log: tail -f /tmp/fwafix.log"
