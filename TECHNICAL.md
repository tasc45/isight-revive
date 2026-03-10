# Technical Details — How iSight Revive Works

This document covers the full technical journey of getting the FireWire iSight working on macOS Sonoma, including every bug we hit, every fix we applied, and the critical discovery that made FaceTime work.

## The Problem

macOS Sonoma (14.x) on OCLP-patched Macs has the FireWire kernel extensions loaded, so `ioreg` sees the iSight hardware. But Apple's userspace camera pipeline — `IIDCVideoAssistant` — crashes immediately on launch. No video in any app.

## Apple's Camera Pipeline

```
FireWire iSight (IEEE 1394 device)
    ↓ isochronous data (YUV 4:1:1, 640×480, S200)
IOFireWireFamily.kext + AppleFWOHCI.kext (kernel, loaded by OCLP)
    ↓ IOKit userspace API
IIDCVideoAssistant (Apple's daemon, Mach service: com.apple.cmio.IIDCVideoAssistant)
    ↓ Mach IPC
IIDC.plugin (Apple's CoreMediaIO DAL plugin, in CoreMediaIO.framework)
    ↓ CoreMediaIO device/stream APIs
avconferenced (Apple's camera bridge — serves cameras to FaceTime, Photo Booth, etc.)
    ↓ AVFoundation
FaceTime / Photo Booth / Zoom / any camera app
```

Key insight: **everything goes through Apple's own IIDCVideoAssistant**. There's no way around it — FaceTime on Sonoma only discovers cameras through CMIO assistants (the old DAL plugin factory pattern is dead). We don't replace any part of this pipeline. We just fix two bugs in it.

## Bug 1: Mach Port Guard Crash (EXC_GUARD)

### Symptom
`IIDCVideoAssistant` dies with `EXC_GUARD` within seconds of launch:
```
Exception Type:  EXC_GUARD
Exception Codes: 0x4000000000000002
```

### Root Cause
macOS Sonoma added security guards to Mach reply ports. Apple's `IIDCVideoAssistant` (last updated ~2012) calls `mach_port_deallocate()` on a guarded reply port. On Sonoma, this triggers an `EXC_GUARD` exception and the process is killed by the kernel.

### Fix
A 40-line DYLD interposition shim (`fwafix_minimal.c`) that redirects `mach_port_deallocate` → `mach_port_destruct`:

```c
kern_return_t safe_mach_port_deallocate(ipc_space_t task, mach_port_name_t name) {
    mach_port_destruct(task, name, 0, 0);  // guard-aware equivalent
    return KERN_SUCCESS;
}
```

`mach_port_destruct` is the guard-aware version of `mach_port_deallocate`. Passing `srdelta=0` and `guard=0` makes it work on both guarded and unguarded ports.

The shim is injected via `DYLD_INSERT_LIBRARIES` in the LaunchDaemon plist. The binary must be signed with `com.apple.security.cs.allow-dyld-environment-variables` and `com.apple.security.cs.disable-library-validation` entitlements.

### Critical: Minimal interpositions only
We tried adding more interpositions (sandbox_init, IOConnectCallMethod tracing) — **this breaks FaceTime**. The CMIO pipeline is sensitive to function interpositions. Only the guard fix is needed; the sandbox is handled separately via binary patch.

## Bug 2: Sandbox Blocks FireWire

### Symptom
With the guard fix alone, `IIDCVideoAssistant` no longer crashes but can't access the FireWire bus:
```
sandbox violation: deny iokit-open IOFireWireUserClient
```

### Root Cause
`IIDCVideoAssistant` calls `sandbox_init()` early in startup with a profile that denies IOKit access to FireWire device classes. In older macOS versions, this profile was permissive enough; Sonoma tightened it.

### Fix
Binary patch: NOP out the `call sandbox_init` instruction in IIDCVideoAssistant.

The call is at **fat file offset 0xE05D** in the Sonoma 14.6.1 binary. It's a 5-byte x86_64 `CALL rel32` instruction (`E8 xx xx xx xx`). We replace all 5 bytes with `NOP` (`90 90 90 90 90`):

```bash
# Verify the byte is E8 (call instruction) before patching
BYTE=$(xxd -s 0xE05D -l 1 -p IIDCVideoAssistant)
if [ "$BYTE" = "e8" ]; then
    printf '\x90\x90\x90\x90\x90' | dd of=IIDCVideoAssistant bs=1 seek=$((0xE05D)) conv=notrunc
fi
```

After patching, the binary must be re-signed with entitlements:
```bash
codesign -f -s - --entitlements iidc_entitlements.plist IIDCVideoAssistant
```

## The FaceTime Discovery

### Problem
After fixing both bugs, **Photo Booth showed video but FaceTime didn't**.

### Root Cause
`avconferenced` — Apple's camera bridge process that serves cameras to FaceTime — **caches device state at boot**. If `IIDCVideoAssistant` wasn't running when `avconferenced` started (which it wasn't, because it was crashing), `avconferenced` cached "no iSight camera" and never re-checked.

Photo Booth queries the CMIO system directly and doesn't rely on `avconferenced`, which is why it worked.

### Fix
**Restart avconferenced** after IIDCVideoAssistant is running:
```bash
sudo kill $(pgrep avconferenced)
```
It auto-respawns via launchd, discovers the now-running IIDCVideoAssistant, and FaceTime sees the camera.

This is the single most important operational detail. The boot startup script handles this automatically.

## Audio Fix

### Problem
The iSight's built-in microphone uses Apple's `iSightAudio.driver`, which exports the old flat-function AudioServerPlugIn API. Modern macOS (Catalina+) requires the CFPlugIn factory pattern and won't load drivers that don't implement it.

### Fix
A wrapper binary (`isight_audio_wrapper.mm` + `safe_call.cpp`) replaces the original `iSightAudio` binary. It:
1. Implements the CFPlugIn factory interface (`iSightAudioFactory`) that macOS expects
2. Uses `dlopen` to load the original binary (renamed `.orig`) and resolves all `AudioServerPlugIn_*` flat exports via `dlsym`
3. Bridges all AudioServerPlugIn calls through to the original driver (stripping the COM `driver` first argument)
4. Wraps all property forwarding calls in C++ exception handlers (`safe_call.cpp`) to protect `coreaudiod` from crashes — the old driver throws C++ exceptions for unknown object IDs
5. Provides a proxy host interface that absorbs the old driver's broken double-pointer dispatch convention (the old code does `rdi=vtable_ptr, rsi=host_pp` which shifts all arguments by one register)
6. Discovers devices by querying the old driver's internal plugin object (ID 2) for `kAudioPlugInPropertyDeviceList` — the old driver assigns device IDs starting at 256, not the expected low range
7. Handles plugin-level properties (object ID 1) directly, since the old flat API framework handled these externally
8. Includes the same Mach port guard fix as the video side (`mach_port_deallocate` → `mach_port_destruct` with fallback)

The iSight has separate FireWire units for video (SW=258) and audio (SW=16), so video and audio work simultaneously without conflicts. The audio driver loads into `coreaudiod` (not a separate process), so crash protection is critical.

## Boot Persistence

### Problem
On reboot, the camera must work automatically without manual intervention.

### Solution
Two LaunchDaemons:

1. **`com.apple.cmio.IIDCVideoAssistant`** — Runs the patched IIDCVideoAssistant with `DYLD_INSERT_LIBRARIES` pointing to our guard fix dylib. Uses `MachServices` and `EnableTransactions` to match Apple's original on-demand pattern.

2. **`com.isight-revive.startup`** — Runs once at boot:
   - Waits 15 seconds for FireWire bus to enumerate
   - Checks for iSight via `ioreg` (Unit_Spec_ID = 0xA27 / 2599)
   - Kicks IIDCVideoAssistant to pick up the camera
   - **Restarts avconferenced** so FaceTime discovers the camera

### Camera Replug
If the camera is unplugged and replugged:
1. The FireWire bus may need a bandwidth reset: `./bus_reset`
2. Restart the assistant: `sudo launchctl kickstart -k system/com.apple.cmio.IIDCVideoAssistant`
3. Restart avconferenced: `sudo kill $(pgrep avconferenced)`

The IRM (Isochronous Resource Manager) can leak bandwidth allocations when `IIDCVideoAssistant` is killed unexpectedly. `bus_reset` reclaims this.

## What Didn't Work (Abandoned Approaches)

### Custom DAL Plugin
We built a full CoreMediaIO DAL plugin (`iSightDAL.plugin`) that talked directly to the FireWire bus. FaceTime on Sonoma **memory-maps DAL plugin bundles but never calls their factory functions**. The constructor never ran. Dead end.

### CMIOExtension (System Extension)
Swift-based `CMIOExtension` registered as a system extension. Required proper signing (not ad-hoc) and system extension approval flow. Too complex and fragile for a community project.

### Extra DYLD Interpositions
Interposing `sandbox_init`, `IOConnectCallMethod`, and `IOConnectCallScalarMethod` in addition to the guard fix caused FaceTime to break (Photo Booth still worked). The CMIO pipeline is sensitive to function interpositions beyond the minimum. The working solution uses exactly ONE interposition (guard fix) plus ONE binary patch (sandbox NOP).

## Files

| File | Purpose |
|------|---------|
| `install.sh` | One-command installer — builds/copies everything, patches binary, sets up daemons |
| `uninstall.sh` | Clean removal of all installed components |
| `fwafix_minimal.c` | Guard fix + DMA retry + crash handler DYLD shim |
| `isight_audio_wrapper.mm` | Audio driver API bridge (CFPlugIn factory wrapper) |
| `safe_call.cpp` | C++ exception-safe wrappers for old audio driver calls |
| `iidc_entitlements.plist` | Entitlements for DYLD injection into patched binary |
| `bus_reset.cpp` | FireWire bus reset tool (reclaims leaked IRM bandwidth) |
| `prebuilt/libfwafix.dylib` | Pre-compiled guard fix (no Xcode needed) |
| `prebuilt/iSightAudioWrapper` | Pre-compiled audio wrapper (no Xcode needed) |

## Installed Components

| Path | What |
|------|------|
| `/usr/local/lib/libfwafix.dylib` | Guard fix dylib |
| `/usr/local/libexec/IIDCVideoAssistant` | Patched Apple binary |
| `/usr/local/libexec/IIDC.plugin` | Symlink to system IIDC.plugin |
| `/usr/local/libexec/isight-startup.sh` | Boot startup script |
| `/Library/LaunchDaemons/com.apple.cmio.IIDCVideoAssistant.plist` | Assistant daemon |
| `/Library/LaunchDaemons/com.isight-revive.startup.plist` | Boot startup daemon |
| `/Library/Audio/Plug-Ins/HAL/iSightAudio.driver/Contents/MacOS/iSightAudio` | Audio wrapper (original renamed .orig) |

## Log Files

| Log | Contents |
|-----|----------|
| `/tmp/fwafix.log` | Guard fix dylib — shows interposition loading |
| `/tmp/isight_startup.log` | Boot script — FireWire detection and service restarts |
| `/tmp/isight_audio.log` | Audio wrapper — driver loading and property queries |
| `/tmp/iidc_stderr.log` | IIDCVideoAssistant stderr output |

## Verifying the Camera

```bash
# Check FireWire iSight is on the bus
ioreg -r -c IOFireWireUnit | grep "Unit_Spec_ID"
# Should show: Unit_Spec_ID = 2599 (0xA27)

# Check IIDCVideoAssistant is running
pgrep -l IIDCVideoAssistant

# Check avconferenced sees the camera
log show --last 1m --predicate 'process == "avconferenced"' | grep -i isight
```

## Environment

- **Tested on:** Mac Pro 5,1 (2010), macOS Sonoma 14.6.1, OCLP 2.2.0
- **Camera:** Apple FireWire iSight (original external model, 2003)
- **FireWire kexts:** IOFireWireFamily 4.8.3, AppleFWOHCI 5.7.5, Apple_iSight 4.0.1
- **Video format:** YUV 4:1:1, 640×480, isochronous channel 0, S200
