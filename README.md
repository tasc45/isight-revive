# iSight Revive

**Bring your FireWire iSight camera back to life on macOS Sonoma and Sequoia.**

A CMIO DAL plugin that bypasses Apple's crashing `IIDCVideoAssistant` and communicates directly with the FireWire iSight hardware via IOKit. No code signing or provisioning profiles required.

## The Problem

On macOS Sonoma (14.x) and Sequoia (15.x) with [OCLP](https://dortania.github.io/OpenCore-Legacy-Patcher/), the FireWire iSight is *detected* — the kext loads, IOKit sees it, `system_profiler` lists it. But video doesn't work because Apple's `IIDCVideoAssistant` crashes with `EXC_GUARD` (Mach port violation) before any frames are streamed.

## The Solution

iSight Revive is a DAL (Device Abstraction Layer) plugin that:

- Runs all FireWire communication **in-process** — no assistant daemon needed
- Uses IOKit's FireWire userspace API (`IOFireWireLib`) to talk IIDC protocol directly
- Receives isochronous video packets and assembles them into frames
- Delivers standard `CMSampleBuffer` frames through the CMIO pipeline

Once installed, the iSight appears as a normal camera in **any** application — FaceTime, Photo Booth, Zoom, Chrome, etc.

## Requirements

- Mac with **FireWire** (built-in or via Thunderbolt adapter)
- **FireWire iSight** camera (the original external one)
- macOS **Sonoma 14.x** or **Sequoia 15.x** via [OCLP](https://dortania.github.io/OpenCore-Legacy-Patcher/)
- **Xcode Command Line Tools** (for building from source)
- SIP kext signing must be disabled (standard OCLP requirement)

## Quick Install (Pre-built)

```bash
sudo ./install.sh
```

The installer copies the plugin to `/Library/CoreMediaIO/Plug-Ins/DAL/` and restarts camera services. Open any camera app and select "iSight".

## Build from Source

```bash
make
sudo make install
```

Or for a debug build with symbols:

```bash
make debug
sudo make install
```

## Uninstall

```bash
sudo ./uninstall.sh
```

Or manually:

```bash
sudo rm -rf /Library/CoreMediaIO/Plug-Ins/DAL/iSightRevive.plugin
sudo killall VDCAssistant IIDCVideoAssistant 2>/dev/null
```

## How It Works

```
FireWire iSight hardware
    | (isochronous packets over IEEE 1394)
IOFireWireFamily kext (loaded via OCLP)
    | (IOFireWireLib userspace API)
IIDCCamera class (in-process, no assistant)
    | (IIDC register control + isoch receive)
FrameAssembler (packets -> YUV422 frames)
    | (CVPixelBuffer -> CMSampleBuffer)
CMIO DAL Stream (CMSimpleQueue)
    | (standard macOS camera API)
FaceTime / Photo Booth / Zoom / etc.
```

### Key Design Decision: No Assistant Process

Apple's built-in IIDC plugin uses a Mach service (`IIDCVideoAssistant`) to do the FireWire work in a separate process. That assistant crashes on Sonoma+ due to tightened Mach port security (`EXC_GUARD`).

Our plugin omits `CMIOHardwareAssistantServiceNames` from its Info.plist, which tells CoreMediaIO to load us directly into the host process. All FireWire communication happens in-process via IOKit.

## Supported Formats

| Resolution | Pixel Format | Frame Rate |
|-----------|-------------|------------|
| 640x480   | YUV422 (2vuy) | 30 fps |
| 320x240   | YUV422 (2vuy) | 30 fps |

## Audio

The iSight's built-in microphone requires a separate CoreAudio HAL plugin (`iSightAudio.driver`). A copy from macOS High Sierra is included in the `Drivers/` directory — the install script places it in `/Library/Audio/Plug-Ins/HAL/` if not already present. See `Drivers/README-drivers.md` for details.

## Troubleshooting

### Camera doesn't appear

Check if the plugin loaded:

```bash
log show --predicate 'subsystem == "com.isightrevive.dal"' --last 5m
```

Verify the iSight is detected by IOKit:

```bash
ioreg -r -c IOFireWireUnit | grep -A5 "Unit_Spec_ID"
```

### Plugin loads but no video

Force-kill existing assistants and retry:

```bash
sudo killall VDCAssistant IIDCVideoAssistant 2>/dev/null
```

Verify FireWire communication:

```bash
log show --predicate 'subsystem == "com.isightrevive.dal"' --last 1m | grep -E "command base|ReadQuadlet|isoch"
```

### Build errors

Ensure Xcode Command Line Tools are installed:

```bash
xcode-select --install
```

## macOS Version Compatibility

| macOS Version | Status |
|--------------|--------|
| Sonoma 14.x  | Primary target (tested on 14.6.1 via OCLP) |
| Sequoia 15.x | Should work (CMIO DAL API unchanged) |
| Future (16+) | May need updates if Apple changes CMIO DAL API |

The CMIO DAL plugin API has been stable since macOS 10.7 and is not deprecated as of Sequoia 15.x. Apple's newer Camera Extensions (CMIO Extension) API is the long-term replacement, but DAL plugins continue to work.

## Contributing

PRs welcome! Areas that could use help:

- Testing on different Mac models with FireWire
- Thunderbolt-to-FireWire adapter compatibility
- Additional video formats (640x480 RGB, etc.)
- Bus reset recovery improvements
- Audio driver investigation

## License

MIT — see [LICENSE](LICENSE).

## Acknowledgments

- [OCLP](https://dortania.github.io/OpenCore-Legacy-Patcher/) for making modern macOS run on classic Macs
- The IIDC 1394-based Digital Camera Specification
- Apple's (long-removed) sample DAL plugin code
