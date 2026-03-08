#ifndef ISIGHTREVIVE_DEVICE_H
#define ISIGHTREVIVE_DEVICE_H

#include <CoreMediaIO/CMIOHardwarePlugIn.h>
#include <CoreFoundation/CoreFoundation.h>
#include "Stream.h"
#include "IIDCCamera.h"
#include "FrameAssembler.h"

class Device {
public:
    Device(CMIOObjectID objectID, CMIOHardwarePlugInRef plugin);
    ~Device();

    CMIOObjectID getObjectID() const { return mObjectID; }

    // Initialize hardware (discover + open camera)
    bool initialize();

    // Create and publish the stream
    bool createStream(CMIOObjectID streamObjectID);

    Stream* getStream() const { return mStream; }

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

    // Start / stop streaming (delegates to camera + stream)
    OSStatus startStream(CMIOStreamID streamID);
    OSStatus stopStream(CMIOStreamID streamID);

    // Suspend / Resume (for sleep/wake)
    void suspend();
    void resume();

    // Device info
    CFStringRef getName() const { return mName; }
    CFStringRef getUID() const { return mUID; }
    CFStringRef getModelUID() const { return mModelUID; }

private:
    CMIOObjectID mObjectID;
    CMIOHardwarePlugInRef mPlugin;

    // Device metadata
    CFStringRef mName = nullptr;
    CFStringRef mUID = nullptr;
    CFStringRef mModelUID = nullptr;
    CFStringRef mManufacturer = nullptr;

    // Sub-objects
    Stream* mStream = nullptr;
    IIDCCamera mCamera;
    FrameAssembler mFrameAssembler;

    bool mSuspended = false;
};

#endif // ISIGHTREVIVE_DEVICE_H
