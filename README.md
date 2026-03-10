# iSight Revive

**Get your FireWire iSight camera working on macOS Sonoma — video AND audio.**

One command. No Recovery Mode. No SIP changes. No virtual cameras. Uses Apple's own drivers with two tiny compatibility patches.

## Install

```bash
git clone https://github.com/tasc45/isight-revive.git
cd isight-revive
sudo ./install.sh
```

That's it. Open FaceTime or Photo Booth — your iSight is back. Video and audio.

## What It Does

macOS Sonoma broke two things in Apple's FireWire iSight pipeline:

1. **Mach port guard crash** — Sonoma added security guards to Mach reply ports. Apple's `IIDCVideoAssistant` calls `mach_port_deallocate` on a guarded port, which kills the process instantly. **Fix:** A 40-line shim redirects to `mach_port_destruct` (the guard-aware equivalent).

2. **Sandbox blocks FireWire** — The assistant's sandbox profile denies IOKit access to FireWire devices. **Fix:** One byte patched in the binary (a `call` instruction NOPed out).

Everything else is Apple's original code — untouched. The installer copies Apple's `IIDCVideoAssistant` from your system, applies the two patches, and sets up LaunchDaemons so it starts automatically on every boot.

### Crash Protection

If `IIDCVideoAssistant` crashes, FireWire isochronous bandwidth leaks on the bus. If it crash-loops (launchd restarts it repeatedly), the leaked resources accumulate and can cause a **kernel panic** in `AppleFWOHCI_ReceiveDCL::link()`. The guard fix shim includes a crash handler that catches fatal signals (SIGSEGV, SIGABRT, SIGBUS) and fires a FireWire bus reset to reclaim leaked IRM bandwidth before the process exits. This prevents leaked resources from stacking up across restarts.

### Audio

The iSight's built-in microphone also works. Apple's original `iSightAudio.driver` uses an old CoreAudio API (flat exported functions like `AudioServerPlugIn_Initialize`) that modern macOS won't load. The installer wraps it with a translation layer that bridges the old API to the modern CFPlugIn factory pattern. All calls to the original driver are protected by three safety layers: C++ exception handlers, SIGSEGV/SIGBUS signal guards (the old binary has broken pointers that cause segfaults), and blocked dangerous code paths. Your iSight shows up as both a camera AND a microphone in FaceTime, Zoom, and every other app.

### No Xcode Required

Prebuilt binaries are included. The installer uses them automatically. If you have Xcode Command Line Tools installed, it builds from source instead.

## Requirements

- Mac with **FireWire** (built-in or Thunderbolt adapter)
- **FireWire iSight** camera (the original external one)
- macOS **Sonoma 14.x** with [OpenCore Legacy Patcher](https://dortania.github.io/OpenCore-Legacy-Patcher/) (OCLP injects the FireWire kexts that Apple removed)

> **Why OCLP?** Apple dropped FireWire kernel drivers starting in macOS Catalina. OCLP re-injects them (IOFireWireFamily, AppleFWOHCI, Apple_iSight). Without OCLP, macOS won't see the FireWire bus at all. iSight Revive then fixes two Sonoma-specific bugs in Apple's userspace camera pipeline.

### Thunderbolt-to-FireWire Adapter Users

If your Mac has Thunderbolt but no built-in FireWire (2013+), you can use Apple's [Thunderbolt to FireWire Adapter](https://www.apple.com/shop/product/MD464LL/A/). You still need OCLP — Apple removed the FireWire kernel extensions from macOS regardless of how the hardware is connected. OCLP works on both legacy and Apple-supported Macs.

## How It Works

```
FireWire iSight
    ↓ IEEE 1394 isochronous
IOFireWireFamily.kext (OCLP)
    ↓ IOKit userspace API
IIDCVideoAssistant (Apple's binary, patched)     ← guard fix + sandbox NOP
    ↓ Mach IPC
IIDC.plugin (Apple's, untouched)
    ↓ CoreMediaIO
avconferenced → AVFoundation
    ↓
FaceTime / Photo Booth / Zoom / etc.
```

No custom camera drivers. No virtual cameras. No DAL plugins. Just Apple's own pipeline with two compatibility fixes.

## Survives Reboot

The installer sets up two LaunchDaemons:

1. **Camera daemon** — Runs the patched `IIDCVideoAssistant` with the guard fix loaded. Starts on demand when apps request the camera.
2. **Startup script** — Runs once at boot. Waits for the FireWire bus to enumerate, restarts the camera daemon, then **restarts `avconferenced`** (FaceTime's camera bridge) so it discovers the camera fresh.

The `avconferenced` restart is critical. This process caches camera state at boot — if the iSight wasn't ready yet, FaceTime will show no camera until `avconferenced` is restarted. The startup script handles this automatically.

## Uninstall

```bash
sudo ./uninstall.sh
```

Removes everything and restores system defaults.

## Troubleshooting

### Camera not showing in FaceTime

FaceTime's camera bridge (`avconferenced`) caches device state. This is the #1 issue. Restart it:

```bash
sudo kill $(pgrep avconferenced)
```

It respawns automatically and rediscovers the camera. This is handled automatically on reboot by the startup script.

### No video in FaceTime but Photo Booth works

Same issue — `avconferenced` needs to be restarted. Photo Booth queries the camera system directly and doesn't depend on `avconferenced`.

```bash
# Restart the camera assistant first, then avconferenced
sudo launchctl kickstart -k system/com.apple.cmio.IIDCVideoAssistant
sleep 3
sudo kill $(pgrep avconferenced)
```

### No video after replugging camera

The FireWire bus may need a reset to reclaim isochronous bandwidth that leaked when the camera was unplugged:

```bash
# Build the bus reset tool (one time)
clang++ -std=c++17 -arch x86_64 -o bus_reset bus_reset.cpp -framework IOKit -framework CoreFoundation
```

Then reset and restart:

```bash
./bus_reset
sudo launchctl kickstart -k system/com.apple.cmio.IIDCVideoAssistant
sleep 3
sudo kill $(pgrep avconferenced)
```

### No video after waking from sleep

Same as replug — the FireWire bus may need a nudge:

```bash
sudo launchctl kickstart -k system/com.apple.cmio.IIDCVideoAssistant
sleep 3
sudo kill $(pgrep avconferenced)
```

### Camera locks up (green LED on but no video)

The IRM (Isochronous Resource Manager) may have leaked bandwidth. Full reset:

```bash
# Bus reset to reclaim bandwidth
./bus_reset

# Kill and restart the camera daemon
sudo launchctl kickstart -k system/com.apple.cmio.IIDCVideoAssistant
sleep 3
sudo kill $(pgrep avconferenced)
```

### Check if the camera is detected

```bash
ioreg -r -c IOFireWireUnit | grep "Unit_Spec_ID"
```

You should see entries with `Unit_Spec_ID = 2599` (0xA27) — that's your iSight.

### Check if the assistant is running

```bash
pgrep -l IIDCVideoAssistant
```

If nothing shows, start it:
```bash
sudo launchctl kickstart system/com.apple.cmio.IIDCVideoAssistant
```

### Logs

```
/tmp/fwafix.log           — guard fix dylib log
/tmp/isight_startup.log   — boot startup log
/tmp/isight_audio.log     — audio wrapper log
/tmp/iidc_stderr.log      — IIDCVideoAssistant errors
```

## What's in the Box

| File | Purpose |
|------|---------|
| `install.sh` | One-command installer |
| `uninstall.sh` | Clean removal |
| `fwafix_minimal.c` | Guard fix + crash cleanup (bus reset on crash) |
| `isight_audio_wrapper.mm` | Audio API bridge |
| `safe_call.cpp` | C++ exception-safe wrappers for audio |
| `iidc_entitlements.plist` | DYLD injection entitlements |
| `bus_reset.cpp` | FireWire bus reset tool |
| `prebuilt/` | Pre-compiled binaries (no Xcode needed) |
| `TECHNICAL.md` | Deep dive — every bug, fix, and abandoned approach |

## Tested On

- Mac Pro 5,1 (2010) + macOS Sonoma 14.6.1 + OCLP 2.2.0
- FireWire iSight (original external model, 2003)
- Video AND audio confirmed in FaceTime, Photo Booth

## Contributing

PRs welcome! Things that would help:

- Testing on other Mac models with FireWire
- Thunderbolt-to-FireWire adapter compatibility reports
- macOS Sequoia testing
- Multiple iSight cameras simultaneously
- Wake-from-sleep behavior reports

## License

MIT — see [LICENSE](LICENSE).

## Acknowledgments

- [OpenCore Legacy Patcher](https://dortania.github.io/OpenCore-Legacy-Patcher/) for keeping classic Macs alive
- Apple, for building hardware that still works 20 years later
