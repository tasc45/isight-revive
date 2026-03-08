# Bundled Drivers

## iSightAudio.driver

**What:** CoreAudio HAL plugin that provides audio from the FireWire iSight's built-in microphone.

**Origin:** Extracted from macOS High Sierra (10.13). This driver was removed in later macOS versions but still functions when manually installed.

**Install location:** `/Library/Audio/Plug-Ins/HAL/iSightAudio.driver`

**Status:** The audio driver may or may not work on Sonoma+ — the same IPC/assistant issue that breaks video may affect audio too. If the video plugin works but audio doesn't, a dedicated audio HAL plugin may need to be built.

**Note:** The `install.sh` script will install this automatically if it's not already present.

## How to get iSightAudio.driver

If you have a macOS High Sierra (10.13) installation or installer:

1. Mount the installer DMG
2. Extract from: `/System/Library/Extensions/Apple_iSight.kext/Contents/PlugIns/iSightAudio.driver`
3. Or from a High Sierra system: `/Library/Audio/Plug-Ins/HAL/iSightAudio.driver`
4. Place the `.driver` bundle in this `Drivers/` directory

The install script will pick it up automatically.
