#include "IIDCCamera.h"
#include "IIDCRegisters.h"
#include "Logging.h"

#include <IOKit/IOKitLib.h>
#include <IOKit/firewire/IOFireWireLib.h>
#include <IOKit/firewire/IOFireWireLibIsoch.h>
#include <IOKit/firewire/IOFireWireFamilyCommon.h>
#include <IOKit/IOCFPlugIn.h>
#include <CoreFoundation/CoreFoundation.h>
#include <mach/mach.h>
#include <unistd.h>
#include <cstring>

// ============================================================================
// Construction / Destruction
// ============================================================================

IIDCCamera::IIDCCamera() {}

IIDCCamera::~IIDCCamera() {
    if (mStreaming) stopStreaming();
    close();
}

// ============================================================================
// Discovery
// ============================================================================

bool IIDCCamera::discover() {
    // Match IOFireWireUnit with IIDC identifiers
    CFMutableDictionaryRef matching = IOServiceMatching("IOFireWireUnit");
    if (!matching) {
        ISR_LOG_ERROR("Failed to create IOFireWireUnit matching dictionary");
        return false;
    }

    // IIDC cameras identify with Unit_Spec_ID = 0x00A02D (1394 TA)
    // and Unit_SW_Version = 0x000102 (IIDC v1.02)
    int specIDVal = IIDC::kUnitSpecID_IIDC;
    int swVerVal = IIDC::kUnitSWVersion_IIDC102;
    CFNumberRef specID = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &specIDVal);
    CFNumberRef swVer = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &swVerVal);

    CFDictionarySetValue(matching, CFSTR("Unit_Spec_ID"), specID);
    CFDictionarySetValue(matching, CFSTR("Unit_SW_Version"), swVer);
    CFRelease(specID);
    CFRelease(swVer);

    // Find the first matching service
    mService = IOServiceGetMatchingService(kIOMainPortDefault, matching);
    // matching is consumed by IOServiceGetMatchingService

    if (!mService) {
        ISR_LOG_INFO("No IIDC FireWire camera found");
        return false;
    }

    ISR_LOG("Found IIDC camera service: 0x%x", mService);
    return true;
}

// ============================================================================
// Open / Close
// ============================================================================

bool IIDCCamera::open() {
    if (!mService) {
        ISR_LOG_ERROR("No service to open — call discover() first");
        return false;
    }

    // Get the IOFireWire plugin interface
    IOCFPlugInInterface** plugIn = nullptr;
    SInt32 score = 0;

    kern_return_t kr = IOCreatePlugInInterfaceForService(
        mService,
        kIOFireWireLibTypeID,
        kIOCFPlugInInterfaceID,
        &plugIn,
        &score
    );

    if (kr != kIOReturnSuccess || !plugIn) {
        ISR_LOG_ERROR("IOCreatePlugInInterfaceForService failed: 0x%x", kr);
        return false;
    }

    // Query for the FireWire device interface
    HRESULT hr = (*plugIn)->QueryInterface(
        plugIn,
        CFUUIDGetUUIDBytes(kIOFireWireDeviceInterfaceID_v9),
        (LPVOID*)&mDeviceInterface
    );

    (*plugIn)->Release(plugIn);

    if (hr != S_OK || !mDeviceInterface) {
        ISR_LOG_ERROR("QueryInterface for IOFireWireDeviceInterface failed: 0x%lx", (long)hr);
        return false;
    }

    // Open the device
    IOReturn result = (*mDeviceInterface)->Open(mDeviceInterface);
    if (result != kIOReturnSuccess) {
        ISR_LOG_ERROR("Failed to open FireWire device: 0x%x", result);
        (*mDeviceInterface)->Release(mDeviceInterface);
        mDeviceInterface = nullptr;
        return false;
    }

    // Read the GUID (bus-unique 64-bit identifier)
    // The GUID is available as a property on the device nub
    CFTypeRef guidRef = IORegistryEntryCreateCFProperty(
        mService, CFSTR("GUID"), kCFAllocatorDefault, 0
    );
    if (guidRef && CFGetTypeID(guidRef) == CFNumberGetTypeID()) {
        CFNumberGetValue((CFNumberRef)guidRef, kCFNumberSInt64Type, &mGUID);
        CFRelease(guidRef);
    }

    ISR_LOG("Opened FireWire iSight, GUID: 0x%016llx", mGUID);

    // Parse config ROM to find IIDC command register base
    if (!parseConfigROM()) {
        ISR_LOG_ERROR("Failed to parse config ROM — using default command base");
        // Default base for Apple iSight is typically at 0xFFFFF0F00000
        // The actual base is unit-dependent-directory-based
        // We'll try the standard IIDC offset
        mCommandBase = 0xFFFFF0F00000ULL;
    }

    return true;
}

void IIDCCamera::close() {
    if (mDeviceInterface) {
        (*mDeviceInterface)->Close(mDeviceInterface);
        (*mDeviceInterface)->Release(mDeviceInterface);
        mDeviceInterface = nullptr;
    }
    if (mService) {
        IOObjectRelease(mService);
        mService = 0;
    }
    if (mFrameBuffer) {
        free(mFrameBuffer);
        mFrameBuffer = nullptr;
    }
}

// ============================================================================
// Config ROM Parsing
// ============================================================================

bool IIDCCamera::parseConfigROM() {
    if (!mDeviceInterface) return false;

    // Read the unit directory from the config ROM to find the
    // command_regs_base address. The IIDC spec says:
    //   Unit Directory contains a "Unit_Dependent_Info" entry (key 0xD4)
    //   whose value is the offset to the command registers.
    //
    // For the Apple iSight, the command base is typically discoverable
    // by reading the config ROM at standard offsets. However, the
    // IOFireWire framework gives us the unit directory location.

    // Read the bus info block to get the base address
    // The unit dependent directory pointer gives us the IIDC CSR base
    CFTypeRef configROMRef = IORegistryEntryCreateCFProperty(
        mService, CFSTR("ConfigROM"), kCFAllocatorDefault, 0
    );

    if (!configROMRef) {
        ISR_LOG_ERROR("No ConfigROM property available");
        return false;
    }

    if (CFGetTypeID(configROMRef) != CFDataGetTypeID()) {
        CFRelease(configROMRef);
        return false;
    }

    CFDataRef romData = (CFDataRef)configROMRef;
    const UInt8* rom = CFDataGetBytePtr(romData);
    CFIndex romLen = CFDataGetLength(romData);

    ISR_LOG_DEBUG("Config ROM length: %ld bytes", (long)romLen);

    // Parse the config ROM to find unit dependent directory
    // The ROM structure is: Bus Info Block (20 bytes), then Root Directory
    // We need to walk the directory structure to find our unit directory
    // and its command_regs_base offset.

    // For IIDC cameras, the command registers are typically at an offset
    // specified in the unit dependent directory. The offset is relative
    // to the initial register space (0xFFFFF0000000).

    // Walk ROM quadlets looking for unit dependent info
    bool found = false;
    for (CFIndex i = 20; i + 4 <= romLen; i += 4) {
        uint32_t quad = ((uint32_t)rom[i] << 24) | ((uint32_t)rom[i+1] << 16) |
                        ((uint32_t)rom[i+2] << 8) | (uint32_t)rom[i+3];

        uint8_t key = (quad >> 24) & 0xFF;

        // Key 0xD4 = Unit_Dependent_Info (command base offset)
        if (key == 0xD4) {
            uint32_t offset = (quad & 0x00FFFFFF) * 4;  // Convert quadlet offset to bytes
            mCommandBase = 0xFFFFF0000000ULL + (uint64_t)(i + offset);
            ISR_LOG("Found IIDC command base: 0x%012llx (ROM offset 0x%lx, value 0x%08x)",
                    mCommandBase, (long)i, quad);
            found = true;
            break;
        }

        // Key 0xD1 = Indirect offset (follow pointer to unit dependent directory)
        if (key == 0xD1) {
            uint32_t offset = (quad & 0x00FFFFFF) * 4;
            uint64_t dirAddr = 0xFFFFF0000000ULL + (uint64_t)(i + offset);
            ISR_LOG_DEBUG("Found unit dependent directory at 0x%012llx", dirAddr);
            // The command base is often the first entry in this directory
            mCommandBase = dirAddr;
            found = true;
            // Don't break — keep looking for a more specific 0xD4 entry
        }
    }

    CFRelease(configROMRef);

    if (!found) {
        ISR_LOG_ERROR("Could not find IIDC command registers in config ROM");
        return false;
    }

    return true;
}

// ============================================================================
// Register Access
// ============================================================================

bool IIDCCamera::readQuadlet(uint64_t address, uint32_t* value) {
    if (!mDeviceInterface) return false;

    FWAddress fwAddr;
    fwAddr.nodeID = 0;  // Local device
    fwAddr.addressHi = (UInt16)(address >> 32);
    fwAddr.addressLo = (UInt32)(address & 0xFFFFFFFF);

    UInt32 readValue = 0;

    IOReturn result = (*mDeviceInterface)->ReadQuadlet(mDeviceInterface, mService,
                                                       &fwAddr, &readValue, false, 0);
    if (result != kIOReturnSuccess) {
        ISR_LOG_ERROR("ReadQuadlet at 0x%012llx failed: 0x%x", address, result);
        return false;
    }

    *value = readValue;
    return true;
}

bool IIDCCamera::writeQuadlet(uint64_t address, uint32_t value) {
    if (!mDeviceInterface) return false;

    FWAddress fwAddr;
    fwAddr.nodeID = 0;
    fwAddr.addressHi = (UInt16)(address >> 32);
    fwAddr.addressLo = (UInt32)(address & 0xFFFFFFFF);

    IOReturn result = (*mDeviceInterface)->WriteQuadlet(mDeviceInterface, mService,
                                                        &fwAddr, value, false, 0);
    if (result != kIOReturnSuccess) {
        ISR_LOG_ERROR("WriteQuadlet at 0x%012llx failed: 0x%x", address, result);
        return false;
    }

    return true;
}

// ============================================================================
// Capabilities
// ============================================================================

bool IIDCCamera::readCapabilities() {
    if (!mDeviceInterface || mCommandBase == 0) return false;

    // Read V_FORMAT_INQ — which formats are supported
    uint32_t formatInq = 0;
    if (!readQuadlet(mCommandBase + IIDC::kREG_V_FORMAT_INQ, &formatInq)) {
        ISR_LOG_ERROR("Failed to read V_FORMAT_INQ");
        return false;
    }
    ISR_LOG("V_FORMAT_INQ: 0x%08x", formatInq);

    // Check Format 0 (VGA) is supported
    bool hasFormat0 = (formatInq & (1u << 31)) != 0;
    if (!hasFormat0) {
        ISR_LOG_ERROR("Camera does not support Format 0 (VGA)");
        return false;
    }

    // Read V_MODE_INQ for Format 0
    uint32_t modeInq = 0;
    if (!readQuadlet(mCommandBase + IIDC::kREG_V_MODE_INQ_BASE, &modeInq)) {
        ISR_LOG_ERROR("Failed to read V_MODE_INQ for Format 0");
        return false;
    }
    ISR_LOG("V_MODE_INQ[Format 0]: 0x%08x", modeInq);

    // Check 640x480 YUV422 (Mode 3) support
    bool hasMode3 = (modeInq & (1u << (31 - IIDC::kMODE0_640x480_YUV422))) != 0;
    // Check 320x240 YUV422 (Mode 1) support
    bool hasMode1 = (modeInq & (1u << (31 - IIDC::kMODE0_320x240_YUV422))) != 0;

    ISR_LOG("640x480 YUV422: %s, 320x240 YUV422: %s",
            hasMode3 ? "YES" : "NO", hasMode1 ? "YES" : "NO");

    if (!hasMode3 && !hasMode1) {
        ISR_LOG_ERROR("Camera supports neither 640x480 nor 320x240 YUV422");
        return false;
    }

    // Read V_RATE_INQ for our preferred mode
    uint32_t rateInq = 0;
    uint32_t rateInqOffset = IIDC::kREG_V_RATE_INQ_BASE +
        (IIDC::kFORMAT_VGA_NONCOMPRESSED * 8 + IIDC::kMODE0_640x480_YUV422) * 4;
    if (!readQuadlet(mCommandBase + rateInqOffset, &rateInq)) {
        ISR_LOG_ERROR("Failed to read V_RATE_INQ");
        return false;
    }
    ISR_LOG("V_RATE_INQ[Format 0, Mode 3]: 0x%08x", rateInq);

    bool has30fps = (rateInq & (1u << (31 - IIDC::kRATE_30))) != 0;
    bool has15fps = (rateInq & (1u << (31 - IIDC::kRATE_15))) != 0;
    ISR_LOG("30fps: %s, 15fps: %s", has30fps ? "YES" : "NO", has15fps ? "YES" : "NO");

    return true;
}

// ============================================================================
// Configuration
// ============================================================================

bool IIDCCamera::configure(uint32_t format, uint32_t mode, uint32_t rate) {
    if (!mDeviceInterface || mCommandBase == 0) return false;

    // Initialize camera
    if (!writeQuadlet(mCommandBase + IIDC::kREG_INITIALIZE, IIDC::kINITIALIZE_VALUE)) {
        ISR_LOG_ERROR("Failed to initialize camera");
        // Non-fatal, continue
    }
    usleep(100000);  // 100ms for camera to reset

    // Set format
    if (!writeQuadlet(mCommandBase + IIDC::kREG_CURRENT_V_FORMAT, format << 29)) {
        ISR_LOG_ERROR("Failed to set video format %u", format);
        return false;
    }

    // Set mode
    if (!writeQuadlet(mCommandBase + IIDC::kREG_CURRENT_V_MODE, mode << 29)) {
        ISR_LOG_ERROR("Failed to set video mode %u", mode);
        return false;
    }

    // Set frame rate
    if (!writeQuadlet(mCommandBase + IIDC::kREG_CURRENT_V_RATE, rate << 29)) {
        ISR_LOG_ERROR("Failed to set frame rate %u", rate);
        return false;
    }

    mFormat = format;
    mMode = mode;
    mRate = rate;

    // Determine dimensions based on format/mode
    if (format == IIDC::kFORMAT_VGA_NONCOMPRESSED) {
        switch (mode) {
            case IIDC::kMODE0_640x480_YUV422:
                mWidth = 640; mHeight = 480;
                break;
            case IIDC::kMODE0_320x240_YUV422:
                mWidth = 320; mHeight = 240;
                break;
            case IIDC::kMODE0_640x480_YUV411:
                mWidth = 640; mHeight = 480;
                break;
            default:
                mWidth = 640; mHeight = 480;
                break;
        }
    }

    mFrameRateHz = (rate < 6) ? IIDC::kFrameRate[rate] : 30.0;

    // Allocate frame assembly buffer
    mFrameBufferSize = mWidth * mHeight * 2;  // YUV422 = 2 bytes per pixel
    if (mFrameBuffer) free(mFrameBuffer);
    mFrameBuffer = (uint8_t*)calloc(1, mFrameBufferSize);
    mFrameBufferOffset = 0;
    mFrameStarted = false;

    ISR_LOG("Configured: %ux%u @ %.1ffps (format=%u mode=%u rate=%u)",
            mWidth, mHeight, mFrameRateHz, format, mode, rate);

    return true;
}

// ============================================================================
// Isochronous Streaming
// ============================================================================

bool IIDCCamera::startStreaming(FrameCallback callback) {
    if (mStreaming) return true;
    if (!mDeviceInterface) return false;

    mFrameCallback = callback;

    if (!setupIsoch()) {
        ISR_LOG_ERROR("Failed to set up isochronous receive");
        return false;
    }

    // Set ISO channel on camera and enable
    uint32_t isoReg = (mISOChannel << IIDC::kISO_CHANNEL_SHIFT) | IIDC::kISO_EN_BIT;
    if (!writeQuadlet(mCommandBase + IIDC::kREG_ISO_EN, isoReg)) {
        ISR_LOG_ERROR("Failed to enable ISO transmission on camera");
        teardownIsoch();
        return false;
    }

    mStreaming = true;
    ISR_LOG("Streaming started on ISO channel %u", mISOChannel);
    return true;
}

void IIDCCamera::stopStreaming() {
    if (!mStreaming) return;

    // Disable ISO on camera
    writeQuadlet(mCommandBase + IIDC::kREG_ISO_EN, 0);

    teardownIsoch();
    mStreaming = false;
    mFrameCallback = nullptr;

    ISR_LOG("Streaming stopped");
}

// ============================================================================
// Isochronous Setup
// ============================================================================

bool IIDCCamera::setupIsoch() {
    // Allocate DMA buffers for NuDCL receive
    mDCLBuffers = (uint8_t*)calloc(kNumDCLBuffers, kDCLBufferSize);
    if (!mDCLBuffers) {
        ISR_LOG_ERROR("Failed to allocate DCL buffers");
        return false;
    }

    // Create NuDCL pool
    mDCLPool = (*mDeviceInterface)->CreateNuDCLPool(
        mDeviceInterface, kNumDCLBuffers,
        CFUUIDGetUUIDBytes(kIOFireWireNuDCLPoolInterfaceID)
    );
    if (!mDCLPool) {
        ISR_LOG_ERROR("Failed to create NuDCL pool");
        return false;
    }

    // Create receive DCLs in a circular chain
    for (int i = 0; i < kNumDCLBuffers; i++) {
        IOVirtualRange range;
        range.address = (IOVirtualAddress)(mDCLBuffers + (i * kDCLBufferSize));
        range.length = kDCLBufferSize;

        // headerBytes=4 gives us the isoch header (data_length, tag, channel, tcode, sy)
        mDCLs[i] = (*mDCLPool)->AllocateReceivePacket(mDCLPool, nullptr, 4, 1, &range);
        if (!mDCLs[i]) {
            ISR_LOG_ERROR("Failed to allocate receive DCL %d", i);
            return false;
        }

        // Set callback on each DCL — NuDCL pool methods take NuDCLRef directly
        (*mDCLPool)->SetDCLCallback(mDCLs[i], &IIDCCamera::isochCallback);
        (*mDCLPool)->SetDCLRefcon(mDCLs[i], this);
    }

    // Chain DCLs: each branches to the next, last wraps to first
    for (int i = 0; i < kNumDCLBuffers; i++) {
        (*mDCLPool)->SetDCLBranch(mDCLs[i], mDCLs[(i + 1) % kNumDCLBuffers]);
    }

    // Get the DCL program (as DCLCommand* linked list) for CreateLocalIsochPort
    DCLCommand* dclProgram = (*mDCLPool)->GetProgram(mDCLPool);
    if (!dclProgram) {
        ISR_LOG_ERROR("Failed to get DCL program from pool");
        return false;
    }

    // Buffer ranges for optimization hint
    IOVirtualRange bufRange;
    bufRange.address = (IOVirtualAddress)mDCLBuffers;
    bufRange.length = (IOByteCount)(kNumDCLBuffers * kDCLBufferSize);

    // Create local isoch port (receiver)
    mLocalPort = (*mDeviceInterface)->CreateLocalIsochPortWithOptions(
        mDeviceInterface,
        false,           // receiver (not talker)
        dclProgram,
        0,               // startEvent
        0,               // startState
        0,               // startMask
        nullptr,         // dclProgramRanges
        0,               // dclProgramRangeCount
        &bufRange,       // bufferRanges
        1,               // bufferRangeCount
        kFWIsochPortDefaultOptions,
        CFUUIDGetUUIDBytes(kIOFireWireLocalIsochPortInterfaceID)
    );

    if (!mLocalPort) {
        ISR_LOG_ERROR("Failed to create local isoch port");
        return false;
    }

    // Create remote isoch port (represents the camera/talker)
    mRemotePort = (*mDeviceInterface)->CreateRemoteIsochPort(
        mDeviceInterface,
        true,            // talker
        CFUUIDGetUUIDBytes(kIOFireWireRemoteIsochPortInterfaceID)
    );

    if (!mRemotePort) {
        ISR_LOG_ERROR("Failed to create remote isoch port");
        return false;
    }

    // Create isoch channel
    mChannel = (*mDeviceInterface)->CreateIsochChannel(
        mDeviceInterface,
        true,            // doIRM (use Isochronous Resource Manager)
        kDCLBufferSize,  // packet size
        kFWSpeed400MBit, // bus speed
        CFUUIDGetUUIDBytes(kIOFireWireIsochChannelInterfaceID)
    );

    if (!mChannel) {
        ISR_LOG_ERROR("Failed to create isoch channel");
        return false;
    }

    // Add ports to channel
    IOReturn result = (*mChannel)->AddListener(mChannel,
        (IOFireWireLibIsochPortRef)mLocalPort);
    if (result != kIOReturnSuccess) {
        ISR_LOG_ERROR("Failed to add local port as listener: 0x%x", result);
        return false;
    }

    result = (*mChannel)->SetTalker(mChannel,
        (IOFireWireLibIsochPortRef)mRemotePort);
    if (result != kIOReturnSuccess) {
        ISR_LOG_ERROR("Failed to set remote port as talker: 0x%x", result);
        return false;
    }

    // Allocate channel (gets an ISO channel number from IRM)
    result = (*mChannel)->AllocateChannel(mChannel);
    if (result != kIOReturnSuccess) {
        ISR_LOG_ERROR("Failed to allocate isoch channel: 0x%x", result);
        return false;
    }

    // Start the isoch receive thread
    mIsochThreadRunning = true;
    if (pthread_create(&mIsochThread, nullptr, &IIDCCamera::isochThreadEntry, this) != 0) {
        ISR_LOG_ERROR("Failed to create isoch thread");
        mIsochThreadRunning = false;
        return false;
    }

    // Wait briefly for the run loop to start
    usleep(50000);

    // Start the channel
    result = (*mChannel)->Start(mChannel);
    if (result != kIOReturnSuccess) {
        ISR_LOG_ERROR("Failed to start isoch channel: 0x%x", result);
        return false;
    }

    return true;
}

void IIDCCamera::teardownIsoch() {
    if (mChannel) {
        (*mChannel)->Stop(mChannel);
        (*mChannel)->ReleaseChannel(mChannel);
        (*mChannel)->Release(mChannel);
        mChannel = nullptr;
    }

    // Stop the isoch thread
    if (mIsochThreadRunning) {
        mIsochThreadRunning = false;
        if (mIsochRunLoop) {
            CFRunLoopStop(mIsochRunLoop);
        }
        pthread_join(mIsochThread, nullptr);
        mIsochThread = nullptr;
        mIsochRunLoop = nullptr;
    }

    if (mRemotePort) {
        (*mRemotePort)->Release(mRemotePort);
        mRemotePort = nullptr;
    }
    if (mLocalPort) {
        (*mLocalPort)->Release(mLocalPort);
        mLocalPort = nullptr;
    }
    if (mDCLPool) {
        (*mDCLPool)->Release(mDCLPool);
        mDCLPool = nullptr;
    }
    if (mDCLBuffers) {
        free(mDCLBuffers);
        mDCLBuffers = nullptr;
    }
}

// ============================================================================
// Isoch Thread
// ============================================================================

void* IIDCCamera::isochThreadEntry(void* arg) {
    pthread_setname_np("iSightRevive-isoch");
    static_cast<IIDCCamera*>(arg)->isochThreadRun();
    return nullptr;
}

void IIDCCamera::isochThreadRun() {
    // The IOFireWire isoch callbacks are delivered on a CFRunLoop
    mIsochRunLoop = CFRunLoopGetCurrent();

    // Add the device's notification port to this run loop
    (*mDeviceInterface)->AddIsochCallbackDispatcherToRunLoop(
        mDeviceInterface, mIsochRunLoop
    );

    ISR_LOG("Isoch thread started");

    // Run until told to stop
    while (mIsochThreadRunning) {
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.1, false);
    }

    (*mDeviceInterface)->RemoveIsochCallbackDispatcherFromRunLoop(mDeviceInterface);

    ISR_LOG("Isoch thread exiting");
}

// ============================================================================
// Isoch Callback
// ============================================================================

void IIDCCamera::isochCallback(void* refcon, NuDCLRef dcl) {
    IIDCCamera* self = static_cast<IIDCCamera*>(refcon);
    if (!self || !self->mStreaming) return;

    // Find which DCL buffer this is
    int index = -1;
    for (int i = 0; i < kNumDCLBuffers; i++) {
        if (self->mDCLs[i] == dcl) {
            index = i;
            break;
        }
    }
    if (index < 0) return;

    const uint8_t* buffer = self->mDCLBuffers + (index * kDCLBufferSize);

    // Isochronous packet header is 4 bytes:
    //   [0-1]: data_length (payload bytes)
    //   [2]: tag (2 bits) + channel (6 bits)
    //   [3]: tcode (4 bits) + sy (4 bits)
    uint16_t dataLength = ((uint16_t)buffer[0] << 8) | buffer[1];
    uint8_t sy = buffer[3] & 0x0F;

    if (dataLength > 0 && dataLength <= (kDCLBufferSize - 4)) {
        self->handleIsochPacket(buffer + 4, dataLength, sy);
    }
}

// ============================================================================
// Frame Assembly
// ============================================================================

void IIDCCamera::handleIsochPacket(const uint8_t* payload, size_t length, uint32_t sy) {
    // IIDC uses sy=1 to mark the start of a new frame
    if (sy == 1) {
        // If we have a complete frame from previous accumulation, deliver it
        if (mFrameStarted && mFrameBufferOffset == mFrameBufferSize) {
            if (mFrameCallback) {
                mFrameCallback(mFrameBuffer, mFrameBufferSize, mWidth, mHeight);
            }
        } else if (mFrameStarted && mFrameBufferOffset > 0) {
            ISR_LOG_DEBUG("Incomplete frame: %zu / %zu bytes",
                          mFrameBufferOffset, mFrameBufferSize);
        }

        // Start new frame
        mFrameBufferOffset = 0;
        mFrameStarted = true;
    }

    if (!mFrameStarted) return;

    // Append payload to frame buffer
    size_t copyLen = length;
    if (mFrameBufferOffset + copyLen > mFrameBufferSize) {
        copyLen = mFrameBufferSize - mFrameBufferOffset;
    }

    if (copyLen > 0) {
        memcpy(mFrameBuffer + mFrameBufferOffset, payload, copyLen);
        mFrameBufferOffset += copyLen;
    }
}
