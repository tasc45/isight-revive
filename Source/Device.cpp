#include "Device.h"
#include "Logging.h"

#include <CoreMediaIO/CMIOHardwarePlugIn.h>

Device::Device(CMIOObjectID objectID, CMIOHardwarePlugInRef plugin)
    : mObjectID(objectID), mPlugin(plugin) {
    mName = CFSTR("iSight");
    mModelUID = CFSTR("iSight FireWire");
    mManufacturer = CFSTR("Apple");
    // UID will be set from GUID after discovery
    mUID = CFSTR("iSightRevive-FireWire-iSight");
}

Device::~Device() {
    if (mStream) {
        mStream->stopStream();
        mCamera.stopStreaming();
        delete mStream;
    }
    mCamera.close();
}

bool Device::initialize() {
    // Discover the FireWire iSight
    if (!mCamera.discover()) {
        ISR_LOG_ERROR("No FireWire iSight camera found");
        return false;
    }

    // Open the device
    if (!mCamera.open()) {
        ISR_LOG_ERROR("Failed to open FireWire iSight");
        return false;
    }

    // Build a unique device ID from the GUID
    char uidStr[64];
    snprintf(uidStr, sizeof(uidStr), "iSightRevive-0x%016llx", mCamera.getGUID());
    mUID = CFStringCreateWithCString(kCFAllocatorDefault, uidStr, kCFStringEncodingASCII);

    // Read camera capabilities
    if (!mCamera.readCapabilities()) {
        ISR_LOG_ERROR("Failed to read iSight capabilities — proceeding anyway");
    }

    // Configure default mode: 640x480 YUV422 @ 30fps
    if (!mCamera.configure(0, 3, 4)) {
        ISR_LOG_ERROR("Failed to configure iSight — proceeding anyway");
    }

    ISR_LOG("iSight device initialized, GUID: 0x%016llx", mCamera.getGUID());
    return true;
}

bool Device::createStream(CMIOObjectID streamObjectID) {
    mStream = new Stream(streamObjectID, mObjectID);
    if (!mStream) return false;

    // Initialize frame assembler
    if (!mFrameAssembler.initialize(mCamera.getWidth(), mCamera.getHeight(),
                                     mCamera.getFrameRate())) {
        ISR_LOG_ERROR("Failed to initialize frame assembler");
        return false;
    }

    // Wire up: FrameAssembler → Stream
    mFrameAssembler.setCallback([this](CMSampleBufferRef buffer) {
        if (mStream) {
            mStream->enqueueSampleBuffer(buffer);
        }
    });

    return true;
}

// ============================================================================
// Start / Stop
// ============================================================================

OSStatus Device::startStream(CMIOStreamID streamID) {
    if (!mStream || mStream->getObjectID() != streamID) {
        return kCMIOHardwareBadStreamError;
    }

    // Start the CMIO stream (enables buffer queue)
    OSStatus status = mStream->startStream();
    if (status != noErr) return status;

    // Reconfigure camera if format changed
    auto fmt = Stream::kFormats[mStream->getFormatIndex()];
    uint32_t mode = (fmt.width == 640) ? 3 : 1;  // Mode 3 = 640x480, Mode 1 = 320x240
    if (mCamera.getWidth() != fmt.width || mCamera.getHeight() != fmt.height) {
        mCamera.configure(0, mode, 4);
        mFrameAssembler.initialize(fmt.width, fmt.height, fmt.frameRate);
        mFrameAssembler.setCallback([this](CMSampleBufferRef buffer) {
            if (mStream) mStream->enqueueSampleBuffer(buffer);
        });
    }

    // Start camera isochronous streaming
    bool ok = mCamera.startStreaming([this](const uint8_t* data, size_t length,
                                             uint32_t width, uint32_t height) {
        mFrameAssembler.deliverFrame(data, length, width, height);
    });

    if (!ok) {
        ISR_LOG_ERROR("Failed to start camera streaming");
        mStream->stopStream();
        return kCMIOHardwareNotRunningError;
    }

    ISR_LOG("Device streaming started");
    return noErr;
}

OSStatus Device::stopStream(CMIOStreamID streamID) {
    if (!mStream || mStream->getObjectID() != streamID) {
        return kCMIOHardwareBadStreamError;
    }

    mCamera.stopStreaming();
    mStream->stopStream();

    ISR_LOG("Device streaming stopped");
    return noErr;
}

void Device::suspend() {
    if (mStream && mStream->isActive()) {
        mCamera.stopStreaming();
        mStream->stopStream();
        mSuspended = true;
        ISR_LOG("Device suspended");
    }
}

void Device::resume() {
    if (mSuspended) {
        mSuspended = false;
        ISR_LOG("Device resumed — will restart on next stream start");
    }
}

// ============================================================================
// Properties
// ============================================================================

Boolean Device::hasProperty(const CMIOObjectPropertyAddress* address) {
    switch (address->mSelector) {
        case kCMIOObjectPropertyName:
        case kCMIODevicePropertyPlugIn:
        case kCMIODevicePropertyDeviceUID:
        case kCMIODevicePropertyModelUID:
        case kCMIOObjectPropertyManufacturer:
        case kCMIODevicePropertyTransportType:
        case kCMIODevicePropertyDeviceIsAlive:
        case kCMIODevicePropertyDeviceHasChanged:
        case kCMIODevicePropertyDeviceIsRunning:
        case kCMIODevicePropertyDeviceIsRunningSomewhere:
        case kCMIODevicePropertyStreams:
        case kCMIODevicePropertySuspendedByUser:
        case kCMIODevicePropertyLinkedCoreAudioDeviceUID:
            return true;
        default:
            return false;
    }
}

OSStatus Device::getPropertyDataSize(const CMIOObjectPropertyAddress* address,
                                      UInt32 qualifierDataSize, const void* qualifierData,
                                      UInt32* dataSize) {
    switch (address->mSelector) {
        case kCMIOObjectPropertyName:
        case kCMIODevicePropertyDeviceUID:
        case kCMIODevicePropertyModelUID:
        case kCMIOObjectPropertyManufacturer:
        case kCMIODevicePropertyLinkedCoreAudioDeviceUID:
            *dataSize = sizeof(CFStringRef);
            return noErr;
        case kCMIODevicePropertyPlugIn:
            *dataSize = sizeof(CMIOObjectID);
            return noErr;
        case kCMIODevicePropertyTransportType:
        case kCMIODevicePropertyDeviceIsAlive:
        case kCMIODevicePropertyDeviceHasChanged:
        case kCMIODevicePropertyDeviceIsRunning:
        case kCMIODevicePropertyDeviceIsRunningSomewhere:
        case kCMIODevicePropertySuspendedByUser:
            *dataSize = sizeof(UInt32);
            return noErr;
        case kCMIODevicePropertyStreams:
            *dataSize = mStream ? sizeof(CMIOObjectID) : 0;
            return noErr;
        default:
            return kCMIOHardwareUnknownPropertyError;
    }
}

OSStatus Device::getPropertyData(const CMIOObjectPropertyAddress* address,
                                  UInt32 qualifierDataSize, const void* qualifierData,
                                  UInt32 dataSize, UInt32* dataUsed, void* data) {
    switch (address->mSelector) {
        case kCMIOObjectPropertyName:
            if (dataSize < sizeof(CFStringRef)) return kCMIOHardwareBadPropertySizeError;
            *(CFStringRef*)data = mName;
            *dataUsed = sizeof(CFStringRef);
            return noErr;

        case kCMIODevicePropertyPlugIn:
            if (dataSize < sizeof(CMIOObjectID)) return kCMIOHardwareBadPropertySizeError;
            *(CMIOObjectID*)data = (CMIOObjectID)(uintptr_t)mPlugin;
            *dataUsed = sizeof(CMIOObjectID);
            return noErr;

        case kCMIODevicePropertyDeviceUID:
            if (dataSize < sizeof(CFStringRef)) return kCMIOHardwareBadPropertySizeError;
            *(CFStringRef*)data = mUID;
            *dataUsed = sizeof(CFStringRef);
            return noErr;

        case kCMIODevicePropertyModelUID:
            if (dataSize < sizeof(CFStringRef)) return kCMIOHardwareBadPropertySizeError;
            *(CFStringRef*)data = mModelUID;
            *dataUsed = sizeof(CFStringRef);
            return noErr;

        case kCMIOObjectPropertyManufacturer:
            if (dataSize < sizeof(CFStringRef)) return kCMIOHardwareBadPropertySizeError;
            *(CFStringRef*)data = mManufacturer;
            *dataUsed = sizeof(CFStringRef);
            return noErr;

        case kCMIODevicePropertyTransportType:
            if (dataSize < sizeof(UInt32)) return kCMIOHardwareBadPropertySizeError;
            *(UInt32*)data = '1394';  // kAudioDeviceTransportTypeFireWire
            *dataUsed = sizeof(UInt32);
            return noErr;

        case kCMIODevicePropertyDeviceIsAlive:
            if (dataSize < sizeof(UInt32)) return kCMIOHardwareBadPropertySizeError;
            *(UInt32*)data = 1;  // Always alive while loaded
            *dataUsed = sizeof(UInt32);
            return noErr;

        case kCMIODevicePropertyDeviceHasChanged:
            if (dataSize < sizeof(UInt32)) return kCMIOHardwareBadPropertySizeError;
            *(UInt32*)data = 0;
            *dataUsed = sizeof(UInt32);
            return noErr;

        case kCMIODevicePropertyDeviceIsRunning:
        case kCMIODevicePropertyDeviceIsRunningSomewhere:
            if (dataSize < sizeof(UInt32)) return kCMIOHardwareBadPropertySizeError;
            *(UInt32*)data = (mStream && mStream->isActive()) ? 1 : 0;
            *dataUsed = sizeof(UInt32);
            return noErr;

        case kCMIODevicePropertyStreams:
            if (mStream && dataSize >= sizeof(CMIOObjectID)) {
                *(CMIOObjectID*)data = mStream->getObjectID();
                *dataUsed = sizeof(CMIOObjectID);
            } else {
                *dataUsed = 0;
            }
            return noErr;

        case kCMIODevicePropertySuspendedByUser:
            if (dataSize < sizeof(UInt32)) return kCMIOHardwareBadPropertySizeError;
            *(UInt32*)data = mSuspended ? 1 : 0;
            *dataUsed = sizeof(UInt32);
            return noErr;

        case kCMIODevicePropertyLinkedCoreAudioDeviceUID:
            // Link to iSight audio device if the HAL plugin is installed
            if (dataSize < sizeof(CFStringRef)) return kCMIOHardwareBadPropertySizeError;
            *(CFStringRef*)data = CFSTR("iSightAudioDevice");
            *dataUsed = sizeof(CFStringRef);
            return noErr;

        default:
            return kCMIOHardwareUnknownPropertyError;
    }
}

OSStatus Device::setPropertyData(const CMIOObjectPropertyAddress* address,
                                  UInt32 qualifierDataSize, const void* qualifierData,
                                  UInt32 dataSize, const void* data) {
    // Most device properties are read-only
    return kCMIOHardwareUnknownPropertyError;
}
