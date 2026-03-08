#ifndef ISIGHTREVIVE_STREAM_H
#define ISIGHTREVIVE_STREAM_H

#include <CoreMediaIO/CMIOHardwarePlugIn.h>
#include <CoreMedia/CoreMedia.h>
#include <dispatch/dispatch.h>
#include "FrameAssembler.h"

class Stream {
public:
    Stream(CMIOObjectID objectID, CMIODeviceID deviceID);
    ~Stream();

    CMIOObjectID getObjectID() const { return mObjectID; }

    // CMIO property access
    Boolean hasProperty(const CMIOObjectPropertyAddress* address);
    OSStatus getPropertyDataSize(const CMIOObjectPropertyAddress* address,
                                  UInt32 qualifierDataSize, const void* qualifierData,
                                  UInt32* dataSize);
    OSStatus getPropertyData(const CMIOObjectPropertyAddress* address,
                              UInt32 qualifierDataSize, const void* qualifierData,
                              UInt32 dataSize, UInt32* dataUsed, void* data);
    OSStatus setPropertyData(const CMIOObjectPropertyAddress* address,
                              UInt32 qualifierDataSize, const void* qualifierData,
                              UInt32 dataSize, const void* data);

    // Start/stop
    OSStatus startStream();
    OSStatus stopStream();

    // Frame delivery (called by FrameAssembler → IIDCCamera pipeline)
    void enqueueSampleBuffer(CMSampleBufferRef buffer);

    // CMIO queue management
    void setQueue(CMSimpleQueueRef queue) { mQueue = queue; }
    void setQueueAlteredProc(CMIODeviceStreamQueueAlteredProc proc, void* refcon) {
        mQueueAlteredProc = proc;
        mQueueAlteredRefcon = refcon;
    }

    bool isActive() const { return mActive; }

    // Format management
    CMVideoFormatDescriptionRef getFormatDescription() const { return mFormatDesc; }

    // Current format selection (index into supported formats)
    void setFormatIndex(uint32_t index);
    uint32_t getFormatIndex() const { return mCurrentFormatIndex; }

    // Get supported formats
    struct FormatInfo {
        uint32_t width;
        uint32_t height;
        double frameRate;
        FourCharCode pixelFormat;
    };
    static constexpr int kNumFormats = 2;
    static const FormatInfo kFormats[kNumFormats];

private:
    void createFormatDescriptions();

    CMIOObjectID mObjectID;
    CMIODeviceID mDeviceID;
    bool mActive = false;

    // Format
    uint32_t mCurrentFormatIndex = 0;  // Default: 640x480 @ 30fps
    CMVideoFormatDescriptionRef mFormatDesc = nullptr;
    CMVideoFormatDescriptionRef mFormatDescs[kNumFormats] = {};

    // Buffer queue
    CMSimpleQueueRef mQueue = nullptr;
    CMIODeviceStreamQueueAlteredProc mQueueAlteredProc = nullptr;
    void* mQueueAlteredRefcon = nullptr;

    // Clock
    CFTypeRef mClock = nullptr;
    uint64_t mSequenceNumber = 0;
};

#endif // ISIGHTREVIVE_STREAM_H
