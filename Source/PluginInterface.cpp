#include "PluginInterface.h"
#include "Logging.h"

#include <CoreMediaIO/CMIOHardwarePlugIn.h>
#include <CoreMediaIO/CMIOHardware.h>
#include <cstring>

// ============================================================================
// Singleton
// ============================================================================

PluginInterface& PluginInterface::instance() {
    static PluginInterface sInstance;
    return sInstance;
}

PluginInterface::PluginInterface() {
    // Build the vtable — assign each field individually since C++ doesn't
    // allow brace-init assignment on C structs in constructor bodies
    memset(&mVtable, 0, sizeof(mVtable));

    // IUnknown
    mVtable._reserved               = nullptr;
    mVtable.QueryInterface           = QueryInterface;
    mVtable.AddRef                   = AddRef;
    mVtable.Release                  = Release;

    // CMIOHardwarePlugIn lifecycle
    mVtable.Initialize               = Initialize;
    mVtable.InitializeWithObjectID   = InitializeWithObjectID;
    mVtable.Teardown                 = Teardown;

    // Object property ops
    mVtable.ObjectShow               = nullptr;
    mVtable.ObjectHasProperty        = ObjectHasProperty;
    mVtable.ObjectIsPropertySettable = ObjectIsPropertySettable;
    mVtable.ObjectGetPropertyDataSize = ObjectGetPropertyDataSize;
    mVtable.ObjectGetPropertyData    = ObjectGetPropertyData;
    mVtable.ObjectSetPropertyData    = ObjectSetPropertyData;

    // Device ops
    mVtable.DeviceSuspend            = DeviceSuspend;
    mVtable.DeviceResume             = DeviceResume;
    mVtable.DeviceStartStream        = DeviceStartStream;
    mVtable.DeviceStopStream         = DeviceStopStream;
    mVtable.DeviceProcessAVCCommand  = DeviceProcessAVCCommand;
    mVtable.DeviceProcessRS422Command = DeviceProcessRS422Command;

    // Stream ops
    mVtable.StreamCopyBufferQueue    = StreamCopyBufferQueue;
    mVtable.StreamDeckPlay           = StreamDeckPlay;
    mVtable.StreamDeckStop           = StreamDeckStop;
    mVtable.StreamDeckJog            = StreamDeckJog;
    mVtable.StreamDeckCueTo          = StreamDeckCueTo;

    mVtablePtr = &mVtable;
}

// ============================================================================
// IUnknown
// ============================================================================

HRESULT PluginInterface::QueryInterface(void* self, REFIID uuid, LPVOID* interface) {
    // We respond to IUnknown and CMIOHardwarePlugIn
    CFUUIDRef requestedUUID = CFUUIDCreateFromUUIDBytes(kCFAllocatorDefault, uuid);

    if (CFEqual(requestedUUID, kCMIOHardwarePlugInInterfaceID) ||
        CFEqual(requestedUUID, IUnknownUUID)) {
        CFRelease(requestedUUID);
        PluginInterface::instance().mRefCount++;
        *interface = PluginInterface::instance().getInterface();
        return S_OK;
    }

    CFRelease(requestedUUID);
    *interface = nullptr;
    return E_NOINTERFACE;
}

ULONG PluginInterface::AddRef(void* self) {
    return ++PluginInterface::instance().mRefCount;
}

ULONG PluginInterface::Release(void* self) {
    auto& inst = PluginInterface::instance();
    if (inst.mRefCount > 0) inst.mRefCount--;
    return inst.mRefCount;
}

// ============================================================================
// Lifecycle
// ============================================================================

OSStatus PluginInterface::Initialize(CMIOHardwarePlugInRef self) {
    ISR_LOG("Initialize called (no objectID)");
    return PluginInterface::instance().initialize();
}

OSStatus PluginInterface::InitializeWithObjectID(CMIOHardwarePlugInRef self,
                                                   CMIOObjectID objectID) {
    ISR_LOG("InitializeWithObjectID called, objectID=%u", objectID);
    auto& inst = PluginInterface::instance();
    inst.mPluginObjectID = objectID;
    return inst.initialize();
}

OSStatus PluginInterface::initialize() {
    ISR_LOG("iSightRevive plugin initializing...");

    // Create device object ID
    OSStatus status = CMIOObjectCreate(
        getInterface(),
        kCMIOObjectSystemObject,
        kCMIODeviceClassID,
        &mDeviceObjectID
    );
    if (status != noErr) {
        ISR_LOG_ERROR("CMIOObjectCreate device failed: %d", (int)status);
        return status;
    }

    // Create stream object ID
    status = CMIOObjectCreate(
        getInterface(),
        mDeviceObjectID,
        kCMIOStreamClassID,
        &mStreamObjectID
    );
    if (status != noErr) {
        ISR_LOG_ERROR("CMIOObjectCreate stream failed: %d", (int)status);
        return status;
    }

    // Create and initialize our device
    mDevice = new Device(mDeviceObjectID, (CMIOHardwarePlugInRef)getInterface());
    if (!mDevice->initialize()) {
        ISR_LOG_ERROR("Device initialization failed — camera may not be connected");
        // Don't return error — we still publish the device so it shows up
        // and can reconnect later
    }

    // Create the stream on our device
    mDevice->createStream(mStreamObjectID);

    // Publish the device + stream to the system
    // First publish stream (child), then device (parent)
    CMIOObjectID streamID = mStreamObjectID;
    status = CMIOObjectsPublishedAndDied(
        getInterface(),
        mDeviceObjectID,
        1, &streamID,   // published
        0, nullptr       // died
    );
    if (status != noErr) {
        ISR_LOG_ERROR("Failed to publish stream: %d", (int)status);
    }

    CMIOObjectID deviceID = mDeviceObjectID;
    status = CMIOObjectsPublishedAndDied(
        getInterface(),
        kCMIOObjectSystemObject,
        1, &deviceID,
        0, nullptr
    );
    if (status != noErr) {
        ISR_LOG_ERROR("Failed to publish device: %d", (int)status);
    }

    ISR_LOG("iSightRevive plugin initialized — device=%u stream=%u",
            mDeviceObjectID, mStreamObjectID);
    return noErr;
}

OSStatus PluginInterface::Teardown(CMIOHardwarePlugInRef self) {
    return PluginInterface::instance().teardown();
}

OSStatus PluginInterface::teardown() {
    ISR_LOG("iSightRevive plugin tearing down...");

    if (mDevice) {
        // Unpublish
        CMIOObjectID streamID = mStreamObjectID;
        CMIOObjectsPublishedAndDied(
            getInterface(), mDeviceObjectID,
            0, nullptr, 1, &streamID
        );
        CMIOObjectID deviceID = mDeviceObjectID;
        CMIOObjectsPublishedAndDied(
            getInterface(), kCMIOObjectSystemObject,
            0, nullptr, 1, &deviceID
        );

        delete mDevice;
        mDevice = nullptr;
    }

    return noErr;
}

// ============================================================================
// Object lookup helpers
// ============================================================================

Stream* PluginInterface::getStream(CMIOStreamID streamID) {
    if (mDevice && mDevice->getStream() &&
        mDevice->getStream()->getObjectID() == streamID) {
        return mDevice->getStream();
    }
    return nullptr;
}

// ============================================================================
// Property Operations
// ============================================================================

Boolean PluginInterface::ObjectHasProperty(CMIOHardwarePlugInRef self,
                                            CMIOObjectID objectID,
                                            const CMIOObjectPropertyAddress* address) {
    auto& inst = PluginInterface::instance();

    if (objectID == inst.mDeviceObjectID && inst.mDevice) {
        return inst.mDevice->hasProperty(address);
    }
    if (objectID == inst.mStreamObjectID && inst.mDevice && inst.mDevice->getStream()) {
        return inst.mDevice->getStream()->hasProperty(address);
    }

    // Plugin object properties
    if (objectID == inst.mPluginObjectID) {
        return (address->mSelector == kCMIOObjectPropertyName);
    }

    return false;
}

OSStatus PluginInterface::ObjectIsPropertySettable(CMIOHardwarePlugInRef self,
                                                    CMIOObjectID objectID,
                                                    const CMIOObjectPropertyAddress* address,
                                                    Boolean* isSettable) {
    *isSettable = false;

    // Format description is settable on stream
    if (address->mSelector == kCMIOStreamPropertyFormatDescription) {
        *isSettable = true;
    }

    return noErr;
}

OSStatus PluginInterface::ObjectGetPropertyDataSize(CMIOHardwarePlugInRef self,
                                                     CMIOObjectID objectID,
                                                     const CMIOObjectPropertyAddress* address,
                                                     UInt32 qualifierDataSize,
                                                     const void* qualifierData,
                                                     UInt32* dataSize) {
    auto& inst = PluginInterface::instance();

    if (objectID == inst.mDeviceObjectID && inst.mDevice) {
        return inst.mDevice->getPropertyDataSize(address, qualifierDataSize,
                                                  qualifierData, dataSize);
    }
    if (objectID == inst.mStreamObjectID && inst.mDevice && inst.mDevice->getStream()) {
        return inst.mDevice->getStream()->getPropertyDataSize(address, qualifierDataSize,
                                                               qualifierData, dataSize);
    }
    if (objectID == inst.mPluginObjectID) {
        if (address->mSelector == kCMIOObjectPropertyName) {
            *dataSize = sizeof(CFStringRef);
            return noErr;
        }
    }

    return kCMIOHardwareUnknownPropertyError;
}

OSStatus PluginInterface::ObjectGetPropertyData(CMIOHardwarePlugInRef self,
                                                 CMIOObjectID objectID,
                                                 const CMIOObjectPropertyAddress* address,
                                                 UInt32 qualifierDataSize,
                                                 const void* qualifierData,
                                                 UInt32 dataSize,
                                                 UInt32* dataUsed,
                                                 void* data) {
    auto& inst = PluginInterface::instance();

    if (objectID == inst.mDeviceObjectID && inst.mDevice) {
        return inst.mDevice->getPropertyData(address, qualifierDataSize,
                                              qualifierData, dataSize, dataUsed, data);
    }
    if (objectID == inst.mStreamObjectID && inst.mDevice && inst.mDevice->getStream()) {
        return inst.mDevice->getStream()->getPropertyData(address, qualifierDataSize,
                                                           qualifierData, dataSize,
                                                           dataUsed, data);
    }
    if (objectID == inst.mPluginObjectID) {
        if (address->mSelector == kCMIOObjectPropertyName) {
            if (dataSize < sizeof(CFStringRef)) return kCMIOHardwareBadPropertySizeError;
            *(CFStringRef*)data = CFSTR("iSightRevive");
            *dataUsed = sizeof(CFStringRef);
            return noErr;
        }
    }

    return kCMIOHardwareUnknownPropertyError;
}

OSStatus PluginInterface::ObjectSetPropertyData(CMIOHardwarePlugInRef self,
                                                 CMIOObjectID objectID,
                                                 const CMIOObjectPropertyAddress* address,
                                                 UInt32 qualifierDataSize,
                                                 const void* qualifierData,
                                                 UInt32 dataSize,
                                                 const void* data) {
    auto& inst = PluginInterface::instance();

    if (objectID == inst.mStreamObjectID && inst.mDevice && inst.mDevice->getStream()) {
        return inst.mDevice->getStream()->setPropertyData(address, qualifierDataSize,
                                                           qualifierData, dataSize, data);
    }

    return kCMIOHardwareUnknownPropertyError;
}

// ============================================================================
// Device Operations
// ============================================================================

OSStatus PluginInterface::DeviceSuspend(CMIOHardwarePlugInRef self, CMIODeviceID deviceID) {
    auto& inst = PluginInterface::instance();
    if (inst.mDevice && inst.mDevice->getObjectID() == deviceID) {
        inst.mDevice->suspend();
        return noErr;
    }
    return kCMIOHardwareBadDeviceError;
}

OSStatus PluginInterface::DeviceResume(CMIOHardwarePlugInRef self, CMIODeviceID deviceID) {
    auto& inst = PluginInterface::instance();
    if (inst.mDevice && inst.mDevice->getObjectID() == deviceID) {
        inst.mDevice->resume();
        return noErr;
    }
    return kCMIOHardwareBadDeviceError;
}

OSStatus PluginInterface::DeviceStartStream(CMIOHardwarePlugInRef self,
                                             CMIODeviceID deviceID,
                                             CMIOStreamID streamID) {
    auto& inst = PluginInterface::instance();
    if (inst.mDevice && inst.mDevice->getObjectID() == deviceID) {
        return inst.mDevice->startStream(streamID);
    }
    return kCMIOHardwareBadDeviceError;
}

OSStatus PluginInterface::DeviceStopStream(CMIOHardwarePlugInRef self,
                                            CMIODeviceID deviceID,
                                            CMIOStreamID streamID) {
    auto& inst = PluginInterface::instance();
    if (inst.mDevice && inst.mDevice->getObjectID() == deviceID) {
        return inst.mDevice->stopStream(streamID);
    }
    return kCMIOHardwareBadDeviceError;
}

OSStatus PluginInterface::DeviceProcessAVCCommand(CMIOHardwarePlugInRef self,
                                                    CMIODeviceID deviceID,
                                                    CMIODeviceAVCCommand* command) {
    return kCMIOHardwareIllegalOperationError;
}

OSStatus PluginInterface::DeviceProcessRS422Command(CMIOHardwarePlugInRef self,
                                                      CMIODeviceID deviceID,
                                                      CMIODeviceRS422Command* command) {
    return kCMIOHardwareIllegalOperationError;
}

// ============================================================================
// Stream Operations
// ============================================================================

OSStatus PluginInterface::StreamCopyBufferQueue(CMIOHardwarePlugInRef self,
                                                 CMIOStreamID streamID,
                                                 CMIODeviceStreamQueueAlteredProc queueAlteredProc,
                                                 void* queueAlteredRefCon,
                                                 CMSimpleQueueRef* queue) {
    auto& inst = PluginInterface::instance();
    Stream* stream = inst.getStream(streamID);
    if (!stream) return kCMIOHardwareBadStreamError;

    // Create the simple queue — CMIO owns this after we return it
    CMSimpleQueueRef newQueue = nullptr;
    OSStatus status = CMSimpleQueueCreate(kCFAllocatorDefault,
                                           30,  // capacity (frames)
                                           &newQueue);
    if (status != noErr) {
        ISR_LOG_ERROR("Failed to create simple queue: %d", (int)status);
        return status;
    }

    stream->setQueue(newQueue);
    stream->setQueueAlteredProc(queueAlteredProc, queueAlteredRefCon);
    *queue = newQueue;

    ISR_LOG("Buffer queue created for stream %u", streamID);
    return noErr;
}

OSStatus PluginInterface::StreamDeckPlay(CMIOHardwarePlugInRef self, CMIOStreamID streamID) {
    return kCMIOHardwareIllegalOperationError;
}

OSStatus PluginInterface::StreamDeckStop(CMIOHardwarePlugInRef self, CMIOStreamID streamID) {
    return kCMIOHardwareIllegalOperationError;
}

OSStatus PluginInterface::StreamDeckJog(CMIOHardwarePlugInRef self, CMIOStreamID streamID,
                                         SInt32 speed) {
    return kCMIOHardwareIllegalOperationError;
}

OSStatus PluginInterface::StreamDeckCueTo(CMIOHardwarePlugInRef self, CMIOStreamID streamID,
                                           Float64 frameNumber, Boolean playOnCue) {
    return kCMIOHardwareIllegalOperationError;
}
