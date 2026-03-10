#!/bin/bash
# iSight Revive — One-Command Installer
# Gets your FireWire iSight working on macOS Sonoma via OCLP
#
# Usage: sudo ./install.sh
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
INSTALL_DIR="/usr/local/libexec"
DYLIB_DIR="/usr/local/lib"
DAEMON_DIR="/Library/LaunchDaemons"
AUDIO_DIR="/Library/Audio/Plug-Ins/HAL"

# Apple's original binary location
SYSTEM_IIDC="/System/Library/Frameworks/CoreMediaIO.framework/Versions/A/Resources/IIDC.plugin/Contents/Resources/IIDCVideoAssistant"
SYSTEM_IIDC_PLUGIN="/System/Library/Frameworks/CoreMediaIO.framework/Versions/A/Resources/IIDC.plugin"
SYSTEM_AUDIO="/System/Library/Extensions/IOAudioFamily.kext/Contents/PlugIns/iSightAudio.driver"

echo ""
echo "  iSight Revive — FireWire iSight for macOS Sonoma"
echo "  ================================================="
echo ""

# Check for root
if [ "$EUID" -ne 0 ]; then
    echo "This script requires root privileges."
    echo "Run: sudo ./install.sh"
    exit 1
fi

# Check for Xcode CLI tools (optional — prebuilt binaries included)
HAS_CLANG=0
if command -v clang &>/dev/null; then
    HAS_CLANG=1
fi

# Check for FireWire iSight
if ! ioreg -r -c IOFireWireUnit 2>/dev/null | grep -q "Unit_Spec_ID.*2599"; then
    echo "Warning: No FireWire iSight detected on the bus."
    echo "Make sure the camera is plugged in and the FireWire kexts are loaded (OCLP)."
    echo ""
    read -p "Continue anyway? [y/N] " -n 1 -r
    echo ""
    [[ $REPLY =~ ^[Yy]$ ]] || exit 1
fi

# ============================================================================
# Step 1: Build or use prebuilt guard fix dylib
# ============================================================================
echo "[1/7] Preparing guard fix dylib..."
if [ "$HAS_CLANG" -eq 1 ]; then
    clang -arch x86_64 -shared \
        -o "$SCRIPT_DIR/fwafix_minimal.dylib" \
        "$SCRIPT_DIR/fwafix_minimal.c" \
        -framework IOKit -framework CoreFoundation \
        -lSystem -Wno-deprecated-declarations 2>/dev/null
    codesign -s - "$SCRIPT_DIR/fwafix_minimal.dylib" 2>/dev/null
    echo "  Built from source."
elif [ -f "$SCRIPT_DIR/prebuilt/libfwafix.dylib" ]; then
    cp "$SCRIPT_DIR/prebuilt/libfwafix.dylib" "$SCRIPT_DIR/fwafix_minimal.dylib"
    echo "  Using prebuilt binary."
else
    echo "  Error: No prebuilt binary and no Xcode Command Line Tools."
    echo "  Install with: xcode-select --install"
    exit 1
fi

# ============================================================================
# Step 2: Copy and patch IIDCVideoAssistant
# ============================================================================
echo "[2/7] Preparing IIDCVideoAssistant..."
mkdir -p "$INSTALL_DIR"

if [ ! -f "$SYSTEM_IIDC" ]; then
    echo "  Error: IIDCVideoAssistant not found at $SYSTEM_IIDC"
    echo "  Is OCLP installed with FireWire support?"
    exit 1
fi

# Copy fresh from system
cp "$SYSTEM_IIDC" "$INSTALL_DIR/IIDCVideoAssistant"

# Patch sandbox_init call at fat file offset 0xE05D
# Replace: call sandbox_init (E8 xx xx xx xx) → xor eax,eax; nop; nop; nop (31 C0 90 90 90)
# Must zero eax because test eax,eax at 0xE062 checks the return value
BYTE=$(xxd -s 0xE05D -l 1 -p "$INSTALL_DIR/IIDCVideoAssistant")
if [ "$BYTE" = "e8" ]; then
    printf '\x31\xc0\x90\x90\x90' | dd of="$INSTALL_DIR/IIDCVideoAssistant" bs=1 seek=$((0xE05D)) conv=notrunc 2>/dev/null
    echo "  Sandbox call patched (xor eax,eax)."
else
    echo "  Warning: Expected 0xE8 at offset 0xE05D, found 0x$BYTE"
    echo "  The binary may have changed. Skipping sandbox patch."
    echo "  Video may not work without this patch."
fi

# Patch TCC checks at fat offsets 0xBA3B and 0xBD8E
# TCCAccessPreflightWithAuditToken and TCCAccessCheckAuditToken
# Must return 0 (granted) — the com.apple.private.tcc.manager entitlement requires Apple signing
for TCC_OFF in 0xBA3B 0xBD8E; do
    TCC_BYTE=$(xxd -s $TCC_OFF -l 1 -p "$INSTALL_DIR/IIDCVideoAssistant")
    if [ "$TCC_BYTE" = "e8" ]; then
        printf '\x31\xc0\x90\x90\x90' | dd of="$INSTALL_DIR/IIDCVideoAssistant" bs=1 seek=$(($TCC_OFF)) conv=notrunc 2>/dev/null
        echo "  TCC check patched at $TCC_OFF (xor eax,eax)."
    fi
done

# Re-sign with DYLD entitlements
codesign -f -s - --entitlements "$SCRIPT_DIR/iidc_entitlements.plist" "$INSTALL_DIR/IIDCVideoAssistant" 2>/dev/null
echo "  Signed with DYLD entitlements."

# ============================================================================
# Step 3: Symlink IIDC.plugin next to IIDCVideoAssistant
# ============================================================================
echo "[3/7] Symlinking IIDC.plugin..."
rm -f "$INSTALL_DIR/IIDC.plugin"
ln -s "$SYSTEM_IIDC_PLUGIN" "$INSTALL_DIR/IIDC.plugin"
echo "  Done."

# ============================================================================
# Step 4: Install dylib
# ============================================================================
echo "[4/7] Installing guard fix dylib..."
mkdir -p "$DYLIB_DIR"
cp "$SCRIPT_DIR/fwafix_minimal.dylib" "$DYLIB_DIR/libfwafix.dylib"
echo "  Done."

# ============================================================================
# Step 5: Install LaunchDaemon for IIDCVideoAssistant
# ============================================================================
echo "[5/7] Installing LaunchDaemon..."

# System volume is sealed/read-only (APFS snapshot), so we can't modify the
# system plist. Instead: disable the system daemon, install our own with a
# unique label but same MachService name + correct user identity.
# Clean up old plists from previous attempts.
rm -f "$DAEMON_DIR/com.apple.cmio.IIDCVideoAssistant.plist" 2>/dev/null

# Disable the system daemon so it doesn't compete
launchctl disable system/com.apple.cmio.IIDCVideoAssistant 2>/dev/null || true
launchctl bootout system/com.apple.cmio.IIDCVideoAssistant 2>/dev/null || true

# Unload old custom daemon if present
launchctl bootout system/com.isight-revive.iidc 2>/dev/null || true

cat > "$DAEMON_DIR/com.isight-revive.iidc.plist" << 'PLIST'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
	<key>Label</key>
	<string>com.isight-revive.iidc</string>
	<key>MachServices</key>
	<dict>
		<key>com.apple.cmio.IIDCVideoAssistant</key>
		<true/>
	</dict>
	<key>ProgramArguments</key>
	<array>
		<string>/usr/local/libexec/IIDCVideoAssistant</string>
	</array>
	<key>EnvironmentVariables</key>
	<dict>
		<key>DYLD_INSERT_LIBRARIES</key>
		<string>/usr/local/lib/libfwafix.dylib</string>
	</dict>
	<key>StandardErrorPath</key>
	<string>/tmp/iidc_stderr.log</string>
	<key>StandardOutPath</key>
	<string>/tmp/iidc_stdout.log</string>
	<key>RunAtLoad</key>
	<true/>
	<key>ThrottleInterval</key>
	<integer>3</integer>
	<key>EnableTransactions</key>
	<true/>
	<key>POSIXSpawnType</key>
	<string>Interactive</string>
</dict>
</plist>
PLIST

chmod 644 "$DAEMON_DIR/com.isight-revive.iidc.plist"
chown root:wheel "$DAEMON_DIR/com.isight-revive.iidc.plist"

# Load our daemon
launchctl bootstrap system "$DAEMON_DIR/com.isight-revive.iidc.plist" 2>/dev/null || \
    launchctl load "$DAEMON_DIR/com.isight-revive.iidc.plist" 2>/dev/null
echo "  Done."

# ============================================================================
# Step 6: Install audio wrapper (optional)
# ============================================================================
echo "[6/7] Setting up audio..."

if [ -f "$SCRIPT_DIR/isight_audio_wrapper.mm" ]; then
    # Check if audio driver exists on system or in our repo
    AUDIO_ORIG=""
    if [ -d "$AUDIO_DIR/iSightAudio.driver" ] && [ -f "$AUDIO_DIR/iSightAudio.driver/Contents/MacOS/iSightAudio" ]; then
        AUDIO_ORIG="$AUDIO_DIR/iSightAudio.driver/Contents/MacOS/iSightAudio"
    elif [ -d "$AUDIO_DIR/iSightAudio.driver.disabled" ] && [ -f "$AUDIO_DIR/iSightAudio.driver.disabled/Contents/MacOS/iSightAudio.orig" ]; then
        # Already set up from a previous install
        echo "  Audio wrapper already installed."
        AUDIO_ORIG=""
    fi

    if [ -n "$AUDIO_ORIG" ]; then
        AUDIO_BUILT=0
        if [ "$HAS_CLANG" -eq 1 ]; then
            echo "  Building audio wrapper from source..."
            clang -arch x86_64 -c -o /tmp/audio_wrapper.o -Wno-deprecated-declarations \
                "$SCRIPT_DIR/isight_audio_wrapper.mm" 2>/dev/null && \
            clang++ -arch x86_64 -c -o /tmp/safe_call.o "$SCRIPT_DIR/safe_call.cpp" 2>/dev/null && \
            clang++ -arch x86_64 -bundle -o "$SCRIPT_DIR/iSightAudioWrapper" \
                /tmp/audio_wrapper.o /tmp/safe_call.o \
                -framework CoreFoundation -framework CoreAudio -framework IOKit 2>/dev/null && \
            codesign -s - "$SCRIPT_DIR/iSightAudioWrapper" 2>/dev/null && \
            AUDIO_BUILT=1
            rm -f /tmp/audio_wrapper.o /tmp/safe_call.o
        fi
        if [ "$AUDIO_BUILT" -eq 0 ] && [ -f "$SCRIPT_DIR/prebuilt/iSightAudioWrapper" ]; then
            cp "$SCRIPT_DIR/prebuilt/iSightAudioWrapper" "$SCRIPT_DIR/iSightAudioWrapper"
            echo "  Using prebuilt audio wrapper."
            AUDIO_BUILT=1
        fi
        if [ "$AUDIO_BUILT" -eq 0 ]; then
            echo "  Warning: Could not build audio wrapper. Audio will not work."
            echo "  Install Xcode Command Line Tools: xcode-select --install"
        else
            # Rename original, install wrapper
            mv "$AUDIO_ORIG" "${AUDIO_ORIG}.orig" 2>/dev/null || true
            cp "$SCRIPT_DIR/iSightAudioWrapper" "$AUDIO_ORIG"
            echo "  Audio wrapper installed."
        fi
    else
        echo "  No iSightAudio.driver found on system — skipping audio."
    fi
else
    echo "  Audio wrapper source not found — skipping."
fi

# ============================================================================
# Step 7: Install boot persistence (restart avconferenced on login)
# ============================================================================
echo "[7/7] Setting up boot persistence..."

# Create startup script
cat > "$INSTALL_DIR/isight-startup.sh" << 'STARTUP'
#!/bin/bash
# iSight Revive — Boot startup script
# Waits for FireWire bus to settle, then restarts avconferenced
# so FaceTime discovers the camera.
LOG="/tmp/isight_startup.log"

echo "$(date): iSight startup beginning..." >> "$LOG"

# Wait for FireWire bus to enumerate
sleep 15

# Check if FireWire iSight is present
UNITS=$(ioreg -r -c IOFireWireUnit 2>/dev/null | grep -c "Unit_Spec_ID.*2599")
echo "$(date): Found $UNITS iSight units" >> "$LOG"

if [ "$UNITS" -lt 1 ]; then
    echo "$(date): No iSight detected, waiting longer..." >> "$LOG"
    sleep 15
fi

# Disable system daemon, start our patched one
launchctl disable system/com.apple.cmio.IIDCVideoAssistant 2>/dev/null
launchctl bootout system/com.apple.cmio.IIDCVideoAssistant 2>/dev/null
sleep 1
launchctl kickstart -k system/com.isight-revive.iidc >> "$LOG" 2>&1
echo "$(date): IIDCVideoAssistant restarted (patched)" >> "$LOG"

# Wait for it to initialize
sleep 5

# Restart avconferenced so FaceTime discovers the camera
# This is critical — avconferenced caches camera state at boot
killall avconferenced 2>/dev/null
echo "$(date): avconferenced restarted (will auto-respawn)" >> "$LOG"

echo "$(date): iSight startup complete" >> "$LOG"
STARTUP
chmod 755 "$INSTALL_DIR/isight-startup.sh"

# Install as LaunchDaemon (runs at boot as root)
cat > "$DAEMON_DIR/com.isight-revive.startup.plist" << 'PLIST'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
	<key>Label</key>
	<string>com.isight-revive.startup</string>
	<key>ProgramArguments</key>
	<array>
		<string>/bin/bash</string>
		<string>/usr/local/libexec/isight-startup.sh</string>
	</array>
	<key>RunAtLoad</key>
	<true/>
	<key>LaunchOnlyOnce</key>
	<true/>
</dict>
</plist>
PLIST
chmod 644 "$DAEMON_DIR/com.isight-revive.startup.plist"
chown root:wheel "$DAEMON_DIR/com.isight-revive.startup.plist"
launchctl load "$DAEMON_DIR/com.isight-revive.startup.plist" 2>/dev/null || true
echo "  Done."

# ============================================================================
# Disable any conflicting custom DAL plugins
# ============================================================================
for plugin in iSightDAL.plugin iSightRevive.plugin; do
    if [ -d "/Library/CoreMediaIO/Plug-Ins/DAL/$plugin" ]; then
        mv "/Library/CoreMediaIO/Plug-Ins/DAL/$plugin" "/Library/CoreMediaIO/Plug-Ins/DAL/${plugin}.disabled" 2>/dev/null || true
    fi
done

# ============================================================================
# Start everything
# ============================================================================
echo ""
echo "Starting camera..."
launchctl bootout system/com.apple.cmio.IIDCVideoAssistant 2>/dev/null || true
sleep 1
launchctl kickstart -k system/com.isight-revive.iidc 2>/dev/null || true
sleep 3
killall avconferenced 2>/dev/null || true
sleep 2

echo ""
echo "  Installation complete!"
echo ""
echo "  Installed components:"
echo "    Guard fix:    $DYLIB_DIR/libfwafix.dylib"
echo "    Assistant:    $INSTALL_DIR/IIDCVideoAssistant"
echo "    IIDC.plugin:  $INSTALL_DIR/IIDC.plugin (symlink)"
echo "    Daemon:       $DAEMON_DIR/com.isight-revive.iidc.plist"
echo "    Startup:      $DAEMON_DIR/com.isight-revive.startup.plist"
echo ""
echo "  Open Photo Booth or FaceTime — you should see your iSight camera."
echo "  The camera will work automatically on reboot."
echo ""
echo "  Troubleshooting:"
echo "    Logs:         /tmp/fwafix.log, /tmp/isight_startup.log"
echo "    Manual start: sudo launchctl kickstart -k system/com.isight-revive.iidc"
echo "    Restart FT:   sudo kill \$(pgrep avconferenced)"
echo ""
