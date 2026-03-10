#!/bin/bash
# iSight Startup Script - run after boot to get FireWire iSight working
# The key: DON'T bus_reset on boot - it can break enumeration.
# Just wait for the bus to settle, then restart the daemon.

DIR="$(dirname "$0")"
LOG="/tmp/isight_startup.log"

echo "$(date): iSight startup beginning..." >> "$LOG"

# Wait for FireWire bus to settle and enumerate after boot
sleep 15

# Check if FireWire units are enumerated
UNITS=$(/usr/sbin/ioreg -p IOFireWire 2>/dev/null | wc -l)
echo "$(date): IOFireWire entries: $UNITS" >> "$LOG"

# If no units, wait a bit longer
if [ "$UNITS" -lt 6 ]; then
    echo "$(date): Waiting for camera enumeration..." >> "$LOG"
    sleep 10
    UNITS=$(/usr/sbin/ioreg -p IOFireWire 2>/dev/null | wc -l)
    echo "$(date): IOFireWire entries after wait: $UNITS" >> "$LOG"
fi

# Kill any stale registerassistantservice that might hold the device
killall registerassistantservice 2>/dev/null

# Restart IIDCVideoAssistant so it picks up the camera with our dylib
launchctl kickstart -k system/com.apple.cmio.IIDCVideoAssistant >> "$LOG" 2>&1
echo "$(date): daemon restarted" >> "$LOG"

echo "$(date): iSight startup complete" >> "$LOG"
