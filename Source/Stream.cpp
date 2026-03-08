#include "Stream.h"
#include "Logging.h"

#include <CoreMediaIO/CMIOHardwarePlugIn.h>
#include <mach/mach_time.h>

// Supported formats: 640x480 and 320x240, both YUV422 (2vuy) at 30fps
const Stream::FormatInfo Stream::kFormats[Stream::kNumFormats] = {
    { 640, 480, 30.0, kCVPixelFormatType_422YpCbCr8 },  // Primary
    { 320, 240, 30.0, kCVPixelFormatType_422YpCbCr8 },  // Secondary
};

Stream::Stream(CMIOObjectID objectID, CMIODeviceID deviceID)
    : mObjectID(objectID), mDeviceID(deviceID) {
    createFormatDescriptions();

    // Create a clock for this stream
    OSStatus status = CMIOStreamClockCreate(
        kCFAllocatorDefault,
        CFSTR("iSightRevive Stream Clock"),
        this,       // sourceIdentifier
        CMTimeMake(1, 30),  // minInterval
        100,        // numberOfEventsForRateSmoothing
        10,         // numberOfAveragesForRateSmoothing
        &mClock
    );
    if (status != noErr) {
        ISR_LOG_ERROR("Failed to create stream clock: %d", (int)status);
    }
}

Stream::~Stream() {
    if (mClock) {
        CFRelease(mClock);
    }
    for (int i = 0; i < kNumFormats; i++) {
        if (mFormatDescs[i]) CFRelease(mFormatDescs[i]);
    }
}

void Stream::createFormatDescriptions() {
    for (int i = 0; i < kNumFormats; i++) {
        OSStatus status = CMVideoFormatDescriptionCreate(
            kCFAllocatorDefault,
            kFormats[i].pixelFormat,
            (int32_t)kFormats[i].width,
            (int32_t)kFormats[i].height,
            nullptr,
            &mFormatDescs[i]
        );
        if (status != noErr) {
            ISR_LOG_ERROR("Failed to create format description %d: %d", i, (int)status);
        }
    }
    mFormatDesc = mFormatDescs[0];
}

void Stream::setFormatIndex(uint32_t index) {
    if (index < kNumFormats) {
        mCurrentFormatIndex = index;
        mFormatDesc = mFormatDescs[index];
    }
}

// ============================================================================
// Properties
// ============================================================================

Boolean Stream::hasProperty(const CMIOObjectPropertyAddress* address) {
    switch (address->mSelector) {
        case kCMIOStreamPropertyDirection:
        case kCMIOStreamPropertyTerminalType:
        case kCMIOStreamPropertyStartingChannel:
        case kCMIOStreamPropertyFormatDescription:
        case kCMIOStreamPropertyFormatDescriptions:
        case kCMIOStreamPropertyFrameRate:
        case kCMIOStreamPropertyFrameRates:
        case kCMIOStreamPropertyClock:
            return true;
        default:
            return false;
    }
}

OSStatus Stream::getPropertyDataSize(const CMIOObjectPropertyAddress* address,
                                      UInt32 qualifierDataSize, const void* qualifierData,
                                      UInt32* dataSize) {
    switch (address->mSelector) {
        case kCMIOStreamPropertyDirection:
            *dataSize = sizeof(UInt32);
            return noErr;
        case kCMIOStreamPropertyTerminalType:
            *dataSize = sizeof(UInt32);
            return noErr;
        case kCMIOStreamPropertyStartingChannel:
            *dataSize = sizeof(UInt32);
            return noErr;
        case kCMIOStreamPropertyFormatDescription:
            *dataSize = sizeof(CMFormatDescriptionRef);
            return noErr;
        case kCMIOStreamPropertyFormatDescriptions:
            *dataSize = sizeof(CMFormatDescriptionRef) * kNumFormats;
            return noErr;
        case kCMIOStreamPropertyFrameRate:
            *dataSize = sizeof(Float64);
            return noErr;
        case kCMIOStreamPropertyFrameRates:
            *dataSize = sizeof(Float64);  // One rate per format
            return noErr;
        case kCMIOStreamPropertyClock:
            *dataSize = sizeof(CFTypeRef);
            return noErr;
        default:
            return kCMIOHardwareUnknownPropertyError;
    }
}

OSStatus Stream::getPropertyData(const CMIOObjectPropertyAddress* address,
                                  UInt32 qualifierDataSize, const void* qualifierData,
                                  UInt32 dataSize, UInt32* dataUsed, void* data) {
    switch (address->mSelector) {
        case kCMIOStreamPropertyDirection:
            if (dataSize < sizeof(UInt32)) return kCMIOHardwareBadPropertySizeError;
            *(UInt32*)data = 1;  // 1 = input (camera), 0 = output
            *dataUsed = sizeof(UInt32);
            return noErr;

        case kCMIOStreamPropertyTerminalType:
            if (dataSize < sizeof(UInt32)) return kCMIOHardwareBadPropertySizeError;
            *(UInt32*)data = 'camr';  // Camera terminal type
            *dataUsed = sizeof(UInt32);
            return noErr;

        case kCMIOStreamPropertyStartingChannel:
            if (dataSize < sizeof(UInt32)) return kCMIOHardwareBadPropertySizeError;
            *(UInt32*)data = 1;
            *dataUsed = sizeof(UInt32);
            return noErr;

        case kCMIOStreamPropertyFormatDescription:
            if (dataSize < sizeof(CMFormatDescriptionRef)) return kCMIOHardwareBadPropertySizeError;
            *(CMFormatDescriptionRef*)data = mFormatDesc;
            *dataUsed = sizeof(CMFormatDescriptionRef);
            return noErr;

        case kCMIOStreamPropertyFormatDescriptions: {
            UInt32 needed = sizeof(CMFormatDescriptionRef) * kNumFormats;
            if (dataSize < needed) return kCMIOHardwareBadPropertySizeError;
            CMFormatDescriptionRef* descs = (CMFormatDescriptionRef*)data;
            for (int i = 0; i < kNumFormats; i++) {
                descs[i] = mFormatDescs[i];
            }
            *dataUsed = needed;
            return noErr;
        }

        case kCMIOStreamPropertyFrameRate:
            if (dataSize < sizeof(Float64)) return kCMIOHardwareBadPropertySizeError;
            *(Float64*)data = kFormats[mCurrentFormatIndex].frameRate;
            *dataUsed = sizeof(Float64);
            return noErr;

        case kCMIOStreamPropertyFrameRates:
            if (dataSize < sizeof(Float64)) return kCMIOHardwareBadPropertySizeError;
            *(Float64*)data = 30.0;
            *dataUsed = sizeof(Float64);
            return noErr;

        case kCMIOStreamPropertyClock:
            if (dataSize < sizeof(CFTypeRef)) return kCMIOHardwareBadPropertySizeError;
            *(CFTypeRef*)data = mClock;
            *dataUsed = sizeof(CFTypeRef);
            return noErr;

        default:
            return kCMIOHardwareUnknownPropertyError;
    }
}

OSStatus Stream::setPropertyData(const CMIOObjectPropertyAddress* address,
                                  UInt32 qualifierDataSize, const void* qualifierData,
                                  UInt32 dataSize, const void* data) {
    switch (address->mSelector) {
        case kCMIOStreamPropertyFormatDescription: {
            if (dataSize < sizeof(CMFormatDescriptionRef)) return kCMIOHardwareBadPropertySizeError;
            CMFormatDescriptionRef requestedDesc = *(CMFormatDescriptionRef*)data;
            // Find matching format
            CMVideoDimensions dims = CMVideoFormatDescriptionGetDimensions(requestedDesc);
            for (int i = 0; i < kNumFormats; i++) {
                if (dims.width == (int32_t)kFormats[i].width &&
                    dims.height == (int32_t)kFormats[i].height) {
                    setFormatIndex(i);
                    return noErr;
                }
            }
            return kCMIODeviceUnsupportedFormatError;
        }
        default:
            return kCMIOHardwareUnknownPropertyError;
    }
}

// ============================================================================
// Start / Stop
// ============================================================================

OSStatus Stream::startStream() {
    ISR_LOG("Stream start requested");
    mActive = true;
    mSequenceNumber = 0;
    return noErr;
}

OSStatus Stream::stopStream() {
    ISR_LOG("Stream stop requested");
    mActive = false;
    return noErr;
}

// ============================================================================
// Frame Delivery
// ============================================================================

void Stream::enqueueSampleBuffer(CMSampleBufferRef buffer) {
    if (!mActive || !mQueue) return;

    // Post timing event to the clock
    if (mClock) {
        CMTime pts = CMSampleBufferGetPresentationTimeStamp(buffer);
        UInt64 hostTime = mach_absolute_time();
        OSStatus status = CMIOStreamClockPostTimingEvent(
            pts, hostTime, true, mClock
        );
        if (status != noErr) {
            ISR_LOG_DEBUG("Clock timing event failed: %d", (int)status);
        }
    }

    // Enqueue the buffer
    OSStatus status = CMSimpleQueueEnqueue(mQueue, CFRetain(buffer));
    if (status != noErr) {
        ISR_LOG_DEBUG("Failed to enqueue sample buffer: %d", (int)status);
        CFRelease(buffer);
        return;
    }

    // Notify CMIO that the queue has new data
    if (mQueueAlteredProc) {
        mQueueAlteredProc(mObjectID, nullptr, mQueueAlteredRefcon);
    }
}
