#ifndef ISIGHTREVIVE_PLUGININTERFACE_H
#define ISIGHTREVIVE_PLUGININTERFACE_H

#include <CoreMediaIO/CMIOHardwarePlugIn.h>
#include "Device.h"

// The global plugin interface singleton
class PluginInterface {
public:
    static PluginInterface& instance();

    // Get the CMIOHardwarePlugInInterface pointer (for factory return)
    CMIOHardwarePlugInInterface** getInterface() { return &mVtablePtr; }

    // Lifecycle
    OSStatus initialize();
    OSStatus teardown();

    // Object lookup
    Device* getDevice() { return mDevice; }
    Stream* getStream(CMIOStreamID streamID);

    // CMIO vtable methods (static, delegate to instance)
    static HRESULT QueryInterface(void* self, REFIID uuid, LPVOID* interface);
    static ULONG AddRef(void* self);
    static ULONG Release(void* self);

    static OSStatus Initialize(CMIOHardwarePlugInRef self);
    static OSStatus InitializeWithObjectID(CMIOHardwarePlugInRef self,
                                            CMIOObjectID objectID);
    static OSStatus Teardown(CMIOHardwarePlugInRef self);

    // Object properties
    static Boolean ObjectHasProperty(CMIOHardwarePlugInRef self,
                                      CMIOObjectID objectID,
                                      const CMIOObjectPropertyAddress* address);
    static OSStatus ObjectIsPropertySettable(CMIOHardwarePlugInRef self,
                                              CMIOObjectID objectID,
                                              const CMIOObjectPropertyAddress* address,
                                              Boolean* isSettable);
    static OSStatus ObjectGetPropertyDataSize(CMIOHardwarePlugInRef self,
                                               CMIOObjectID objectID,
                                               const CMIOObjectPropertyAddress* address,
                                               UInt32 qualifierDataSize,
                                               const void* qualifierData,
                                               UInt32* dataSize);
    static OSStatus ObjectGetPropertyData(CMIOHardwarePlugInRef self,
                                           CMIOObjectID objectID,
                                           const CMIOObjectPropertyAddress* address,
                                           UInt32 qualifierDataSize,
                                           const void* qualifierData,
                                           UInt32 dataSize,
                                           UInt32* dataUsed,
                                           void* data);
    static OSStatus ObjectSetPropertyData(CMIOHardwarePlugInRef self,
                                           CMIOObjectID objectID,
                                           const CMIOObjectPropertyAddress* address,
                                           UInt32 qualifierDataSize,
                                           const void* qualifierData,
                                           UInt32 dataSize,
                                           const void* data);

    // Device operations
    static OSStatus DeviceSuspend(CMIOHardwarePlugInRef self, CMIODeviceID deviceID);
    static OSStatus DeviceResume(CMIOHardwarePlugInRef self, CMIODeviceID deviceID);
    static OSStatus DeviceStartStream(CMIOHardwarePlugInRef self,
                                       CMIODeviceID deviceID,
                                       CMIOStreamID streamID);
    static OSStatus DeviceStopStream(CMIOHardwarePlugInRef self,
                                      CMIODeviceID deviceID,
                                      CMIOStreamID streamID);
    static OSStatus DeviceProcessAVCCommand(CMIOHardwarePlugInRef self,
                                             CMIODeviceID deviceID,
                                             CMIODeviceAVCCommand* command);
    static OSStatus DeviceProcessRS422Command(CMIOHardwarePlugInRef self,
                                               CMIODeviceID deviceID,
                                               CMIODeviceRS422Command* command);

    // Stream operations
    static OSStatus StreamCopyBufferQueue(CMIOHardwarePlugInRef self,
                                           CMIOStreamID streamID,
                                           CMIODeviceStreamQueueAlteredProc queueAlteredProc,
                                           void* queueAlteredRefCon,
                                           CMSimpleQueueRef* queue);
    static OSStatus StreamDeckPlay(CMIOHardwarePlugInRef self, CMIOStreamID streamID);
    static OSStatus StreamDeckStop(CMIOHardwarePlugInRef self, CMIOStreamID streamID);
    static OSStatus StreamDeckJog(CMIOHardwarePlugInRef self, CMIOStreamID streamID,
                                   SInt32 speed);
    static OSStatus StreamDeckCueTo(CMIOHardwarePlugInRef self, CMIOStreamID streamID,
                                     Float64 frameNumber, Boolean playOnCue);

private:
    PluginInterface();

    // The vtable itself
    CMIOHardwarePlugInInterface mVtable;
    // Pointer to vtable (what we hand out)
    CMIOHardwarePlugInInterface* mVtablePtr;

    ULONG mRefCount = 1;
    CMIOObjectID mPluginObjectID = 0;

    // Our single device
    Device* mDevice = nullptr;
    CMIOObjectID mDeviceObjectID = 0;
    CMIOObjectID mStreamObjectID = 0;
};

#endif // ISIGHTREVIVE_PLUGININTERFACE_H
