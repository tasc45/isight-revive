#ifndef ISIGHTREVIVE_IIDC_REGISTERS_H
#define ISIGHTREVIVE_IIDC_REGISTERS_H

// IIDC Digital Camera Specification v1.31 — CSR Register Map
// All offsets are relative to the unit-dependent directory base address
// found by parsing the FireWire config ROM.

#include <cstdint>

namespace IIDC {

// ============================================================================
// Config ROM Unit Directory Keys
// ============================================================================

static constexpr uint32_t kUnitSpecID_IIDC       = 0x00A02D;  // 1394 TA: 41005
static constexpr uint32_t kUnitSWVersion_IIDC102  = 0x000102;  // IIDC v1.02 (258)

// ============================================================================
// Command Registers (offsets from command_regs_base)
// ============================================================================

// Initialize register — write 1 to reset camera to factory defaults
static constexpr uint32_t kREG_INITIALIZE         = 0x000;

// Video format/mode/rate inquiry registers
static constexpr uint32_t kREG_V_FORMAT_INQ       = 0x100;
static constexpr uint32_t kREG_V_MODE_INQ_BASE    = 0x180;  // +0x04 per format
static constexpr uint32_t kREG_V_RATE_INQ_BASE    = 0x200;  // +0x04 per format*mode

// Current format/mode/frame rate
static constexpr uint32_t kREG_CURRENT_V_FORMAT   = 0x608;
static constexpr uint32_t kREG_CURRENT_V_MODE     = 0x60C;
static constexpr uint32_t kREG_CURRENT_V_RATE     = 0x610;

// ISO channel and speed
static constexpr uint32_t kREG_ISO_CHANNEL        = 0x614;
static constexpr uint32_t kREG_ISO_EN             = 0x614;  // Same register, bit 0 = enable

// Power control
static constexpr uint32_t kREG_CAMERA_POWER       = 0x610;  // On some cameras

// Basic function inquiry
static constexpr uint32_t kREG_BASIC_FUNC_INQ     = 0x400;

// Feature control registers (brightness, exposure, etc.)
static constexpr uint32_t kREG_FEATURE_HI_INQ     = 0x404;
static constexpr uint32_t kREG_FEATURE_LO_INQ     = 0x408;

// Feature element base — each feature is at offset 0x800 + (feature# * 0x04) for inquiry,
// and 0xC00 + (feature# * 0x04) for control
static constexpr uint32_t kREG_FEATURE_INQ_BASE   = 0x500;
static constexpr uint32_t kREG_FEATURE_CSR_BASE   = 0x800;

// ============================================================================
// Video Formats (IIDC v1.31 §4.1)
// ============================================================================

static constexpr uint32_t kFORMAT_VGA_NONCOMPRESSED = 0;  // Format 0: 160x120 to 640x480
static constexpr uint32_t kFORMAT_SVGA_NONCOMPRESSED_1 = 1;  // Format 1: 800x600 to 1024x768
static constexpr uint32_t kFORMAT_SVGA_NONCOMPRESSED_2 = 2;  // Format 2: 1280x960 to 1600x1200
static constexpr uint32_t kFORMAT_STILL_IMAGE     = 6;
static constexpr uint32_t kFORMAT_SCALABLE_IMAGE  = 7;  // Format 7

// ============================================================================
// Video Modes for Format 0 (VGA)
// ============================================================================

static constexpr uint32_t kMODE0_160x120_YUV444   = 0;
static constexpr uint32_t kMODE0_320x240_YUV422   = 1;
static constexpr uint32_t kMODE0_640x480_YUV411   = 2;
static constexpr uint32_t kMODE0_640x480_YUV422   = 3;
static constexpr uint32_t kMODE0_640x480_RGB8     = 4;
static constexpr uint32_t kMODE0_640x480_MONO8    = 5;
static constexpr uint32_t kMODE0_640x480_MONO16   = 6;

// ============================================================================
// Frame Rates
// ============================================================================

static constexpr uint32_t kRATE_1_875             = 0;
static constexpr uint32_t kRATE_3_75              = 1;
static constexpr uint32_t kRATE_7_5               = 2;
static constexpr uint32_t kRATE_15                = 3;
static constexpr uint32_t kRATE_30                = 4;
static constexpr uint32_t kRATE_60                = 5;

// Rate values for timing calculations
static constexpr double kFrameRate[] = { 1.875, 3.75, 7.5, 15.0, 30.0, 60.0 };

// ============================================================================
// ISO Channel / Enable Register (0x614)
// ============================================================================

// Bits 28-31: channel number (0-63)
// Bit 15: ISO_EN (1 = enable transmission)
static constexpr uint32_t kISO_EN_BIT             = (1u << 15);
static constexpr uint32_t kISO_CHANNEL_SHIFT      = 28;
static constexpr uint32_t kISO_CHANNEL_MASK       = 0xF0000000;

// ============================================================================
// Packet Sizes (bytes per isochronous packet, excluding header)
// For Format 0 modes at various rates
// ============================================================================

// 640x480 YUV422 = 614400 bytes/frame
// At 30fps over 8000 packets/sec (S400): 614400 * 30 / 8000 ≈ 2304 bytes/packet
// But actual packet size depends on bus speed; iSight typically uses 4096 or less
static constexpr uint32_t kMaxPacketSize          = 4096;

// Frame sizes in bytes
static constexpr uint32_t kFrameSize_640x480_YUV422 = 640 * 480 * 2;  // 614400
static constexpr uint32_t kFrameSize_320x240_YUV422 = 320 * 240 * 2;  // 153600
static constexpr uint32_t kFrameSize_640x480_YUV411 = 640 * 480 * 3 / 2;  // 460800

// ============================================================================
// Feature IDs
// ============================================================================

static constexpr uint32_t kFEATURE_BRIGHTNESS     = 0;
static constexpr uint32_t kFEATURE_AUTO_EXPOSURE  = 1;
static constexpr uint32_t kFEATURE_SHARPNESS      = 2;
static constexpr uint32_t kFEATURE_WHITE_BALANCE  = 3;
static constexpr uint32_t kFEATURE_HUE            = 4;
static constexpr uint32_t kFEATURE_SATURATION     = 5;
static constexpr uint32_t kFEATURE_GAMMA          = 6;
static constexpr uint32_t kFEATURE_SHUTTER        = 7;
static constexpr uint32_t kFEATURE_GAIN           = 8;
static constexpr uint32_t kFEATURE_IRIS           = 9;
static constexpr uint32_t kFEATURE_FOCUS          = 10;

// Initialize value
static constexpr uint32_t kINITIALIZE_VALUE       = 0x80000000;

} // namespace IIDC

#endif // ISIGHTREVIVE_IIDC_REGISTERS_H
