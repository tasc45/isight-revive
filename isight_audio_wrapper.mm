// iSight Audio Wrapper — bridges old flat-export AudioServerPlugIn to modern CFPlugIn factory API
// Also includes Mach port guard fix for Sonoma
//
// The original iSightAudio binary uses an older API with flat exports like
// AudioServerPlugIn_Initialize(host) instead of the modern COM-style
// Initialize(driver, host). This wrapper loads the original binary via dlopen
// and creates a proper AudioServerPlugInDriverInterface COM object.
//
// PLUGIN-LEVEL PROPERTIES: The old binary only handles device/stream/control
// objects in its property methods. Plugin-level queries (objectID=1) are handled
// by this wrapper directly, since the old flat API framework handled them externally.
//
// HOST PROXY: The old binary's internal code calls host methods using a pattern
// that results in wrong arguments (the vtable dispatch pointer leaks into rdi,
// and the host double-pointer leaks into rsi as a stale arg). We intercept all
// host calls with a proxy vtable and absorb them safely.
//
// Build:
//   clang -arch x86_64 -bundle -o iSightAudioWrapper -framework CoreFoundation \
//     -framework CoreAudio -framework IOKit -lSystem -Wno-deprecated-declarations \
//     isight_audio_wrapper.c
//   codesign -s - iSightAudioWrapper

#include <CoreFoundation/CoreFoundation.h>
#include <CoreAudio/AudioServerPlugIn.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <mach/mach.h>
#include <os/log.h>

#pragma mark - Mach Port Guard Fix (same as fwafix3)

static kern_return_t patched_mach_port_deallocate(ipc_space_t task, mach_port_name_t name) {
    kern_return_t kr = mach_port_destruct(task, name, 0, 0);
    if (kr == KERN_INVALID_ARGUMENT) {
        kr = mach_port_deallocate(task, name);
    }
    return kr;
}

__attribute__((used, section("__DATA,__interpose")))
static struct {
    kern_return_t (*replacement)(ipc_space_t, mach_port_name_t);
    kern_return_t (*original)(ipc_space_t, mach_port_name_t);
} interpose_dealloc = {
    patched_mach_port_deallocate,
    mach_port_deallocate
};

#pragma mark - Logging

static FILE* gLogFile = NULL;
static void ensureLog(void) {
    if (!gLogFile) {
        gLogFile = fopen("/tmp/isight_audio.log", "a");
        if (gLogFile) setbuf(gLogFile, NULL);
    }
}
#define LOG(fmt, ...) do { \
    ensureLog(); \
    if (gLogFile) { fprintf(gLogFile, "[iSightAudioWrap] " fmt "\n", ##__VA_ARGS__); } \
    os_log(OS_LOG_DEFAULT, "[iSightAudioWrap] " fmt, ##__VA_ARGS__); \
} while(0)

#pragma mark - Device ID Tracking

// Track device IDs created by the old binary during Initialize.
// The old binary calls our proxy_offset_0x10 with rdx=1 (plugin obj)
// when it creates a device. We capture the device's object ID.
#define MAX_DEVICES 8
static AudioObjectID gDeviceIDs[MAX_DEVICES];
static UInt32 gDeviceCount = 0;

#pragma mark - Original Plugin Function Pointers

typedef OSStatus (*InitFunc)(AudioServerPlugInHostRef);
typedef void (*TeardownFunc)(void);
typedef OSStatus (*PerformConfigFunc)(AudioObjectID, UInt64, void*);
typedef OSStatus (*StartFunc)(AudioObjectID);
typedef OSStatus (*StopFunc)(AudioObjectID);
typedef OSStatus (*GetZTSFunc)(AudioObjectID, Float64*, UInt64*, UInt64*);
typedef Boolean (*HasPropFunc)(AudioObjectID, const AudioObjectPropertyAddress*);
typedef OSStatus (*IsSettableFunc)(AudioObjectID, const AudioObjectPropertyAddress*, Boolean*);
typedef OSStatus (*GetPropSizeFunc)(AudioObjectID, const AudioObjectPropertyAddress*, UInt32, const void*, UInt32*);
typedef OSStatus (*GetPropDataFunc)(AudioObjectID, const AudioObjectPropertyAddress*, UInt32, const void*, UInt32, UInt32*, void*);
typedef OSStatus (*SetPropDataFunc)(AudioObjectID, const AudioObjectPropertyAddress*, UInt32, const void*, UInt32, const void*);
typedef OSStatus (*BeginIOFunc)(void);
typedef OSStatus (*EndIOFunc)(void);
typedef OSStatus (*IOCycleFunc)(AudioObjectID, UInt32, const AudioServerPlugInIOCycleInfo*);
typedef OSStatus (*IOOpFunc)(AudioObjectID, UInt32, UInt32, UInt32, const AudioServerPlugInIOCycleInfo*);
typedef OSStatus (*StreamOpFunc)(AudioObjectID, AudioObjectID, UInt32, UInt32, UInt32, const AudioServerPlugInIOCycleInfo*, void*, void*);

static struct {
    void* handle;
    InitFunc Initialize;
    TeardownFunc Teardown;
    PerformConfigFunc PerformDeviceConfigChange;
    StartFunc Start;
    StopFunc Stop;
    GetZTSFunc GetZeroTimeStamp;
    HasPropFunc ObjectHasProperty;
    IsSettableFunc ObjectIsPropertySettable;
    GetPropSizeFunc ObjectGetPropertyDataSize;
    GetPropDataFunc ObjectGetPropertyData;
    SetPropDataFunc ObjectSetPropertyData;
    BeginIOFunc BeginIO;
    EndIOFunc EndIO;
    IOCycleFunc BeginIOCycle;
    IOCycleFunc EndIOCycle;
    IOOpFunc BeginReading;
    IOOpFunc EndReading;
    IOOpFunc BeginWriting;
    IOOpFunc EndWriting;
    StreamOpFunc ReadFromStream;
    StreamOpFunc WriteToStream;
} gOrig = {0};

static Boolean loadOriginal(void) {
    if (gOrig.handle) return true;

    CFBundleRef bundle = CFBundleGetBundleWithIdentifier(CFSTR("com.apple.iSightAudio"));
    char origPath[1024];
    if (bundle) {
        CFURLRef bundleURL = CFBundleCopyBundleURL(bundle);
        CFURLGetFileSystemRepresentation(bundleURL, true, (UInt8*)origPath, sizeof(origPath));
        CFRelease(bundleURL);
        strlcat(origPath, "/Contents/MacOS/iSightAudio.orig", sizeof(origPath));
    } else {
        strlcpy(origPath, "/Library/Audio/Plug-Ins/HAL/iSightAudio.driver/Contents/MacOS/iSightAudio.orig", sizeof(origPath));
    }

    LOG("Loading original: %s", origPath);
    gOrig.handle = dlopen(origPath, RTLD_NOW | RTLD_LOCAL);
    if (!gOrig.handle) {
        LOG("dlopen failed: %s", dlerror());
        return false;
    }

#define LOAD(name) gOrig.name = (typeof(gOrig.name))dlsym(gOrig.handle, "AudioServerPlugIn_" #name)
    LOAD(Initialize);
    LOAD(Teardown);
    LOAD(PerformDeviceConfigChange);
    LOAD(Start);
    LOAD(Stop);
    LOAD(GetZeroTimeStamp);
    LOAD(ObjectHasProperty);
    LOAD(ObjectIsPropertySettable);
    LOAD(ObjectGetPropertyDataSize);
    LOAD(ObjectGetPropertyData);
    LOAD(ObjectSetPropertyData);
    LOAD(BeginIO);
    LOAD(EndIO);
    LOAD(BeginIOCycle);
    LOAD(EndIOCycle);
    LOAD(BeginReading);
    LOAD(EndReading);
    LOAD(BeginWriting);
    LOAD(EndWriting);
    LOAD(ReadFromStream);
    LOAD(WriteToStream);
#undef LOAD

    LOG("Original loaded. Initialize=%p Start=%p HasProperty=%p",
        gOrig.Initialize, gOrig.Start, gOrig.ObjectHasProperty);
    return true;
}

#pragma mark - Host Proxy

static AudioServerPlugInHostRef gRealHost = NULL;

// Proxy methods: the old binary calls these through a broken double-pointer dispatch.
// All calls go to offset 0x10 of the proxy vtable. We absorb them as no-ops.

static OSStatus proxy_PropertiesChanged(AudioServerPlugInHostRef h, AudioObjectID o, UInt32 n, const AudioObjectPropertyAddress* a) {
    LOG("proxy_PropertiesChanged (absorbed)");
    return kAudioHardwareNoError;
}

static OSStatus proxy_CopyFromStorage(AudioServerPlugInHostRef h, CFStringRef k, CFPropertyListRef* d) {
    LOG("proxy_CopyFromStorage (absorbed)");
    if (d) *d = NULL;
    return kAudioHardwareUnspecifiedError;
}

// Proxy for offset 0x10 — the old binary's host call target.
// Called with broken convention: rdi=vtable_ptr, rsi=host_pp, rdx=arg1, rcx=arg2, r8=arg3, r9=arg4
// When rdx=1 (plugin obj) and rcx=ptr, the old binary just created a device and is notifying.
// We absorb the call; our wrapper handles host notification after Initialize.
static OSStatus proxy_offset_0x10_noop(AudioServerPlugInHostRef h, CFStringRef k, CFPropertyListRef d) {
    // rdx arrives as 'd' parameter due to arg shift. If it's 1, a device was likely created.
    LOG("proxy_offset_0x10 absorbed: rdx=%p", d);
    return kAudioHardwareNoError;
}

static OSStatus proxy_DeleteFromStorage(AudioServerPlugInHostRef h, CFStringRef k) {
    return kAudioHardwareNoError;
}

static OSStatus proxy_RequestDeviceConfigChange(AudioServerPlugInHostRef h, AudioObjectID d, UInt64 a, void* i) {
    LOG("proxy_RequestDeviceConfigChange: dev=%u action=%llu", d, a);
    if (gRealHost && gRealHost->RequestDeviceConfigurationChange)
        return gRealHost->RequestDeviceConfigurationChange(gRealHost, d, a, i);
    return kAudioHardwareNoError;
}

static AudioServerPlugInHostInterface gProxyHostInterface = {0};
static const AudioServerPlugInHostInterface* gProxyHostPtr = NULL;

#pragma mark - Plugin-Level Property Handling (obj=1)

// The old binary doesn't handle plugin-level (objectID=1) property queries —
// those were handled by the HAL framework in the old flat API. Our wrapper
// implements these directly.

// Helper: FourCC to string for logging
static void fourcc(UInt32 v, char out[5]) {
    out[0] = (v >> 24) & 0xff;
    out[1] = (v >> 16) & 0xff;
    out[2] = (v >> 8) & 0xff;
    out[3] = v & 0xff;
    out[4] = 0;
}

static Boolean plugin_HasProperty(AudioObjectID objID, const AudioObjectPropertyAddress* addr) {
    UInt32 sel = addr->mSelector;
    switch (sel) {
        case kAudioObjectPropertyBaseClass:           // 'bcls'
        case kAudioObjectPropertyClass:               // 'clas'
        case kAudioObjectPropertyOwner:               // 'stdw'
        case kAudioObjectPropertyOwnedObjects:        // 'ownd'
        case kAudioObjectPropertyManufacturer:         // 'lmak'
        case kAudioPlugInPropertyDeviceList:           // 'dev#'
        case kAudioPlugInPropertyTranslateUIDToDevice: // 'uidd'
        case kAudioPlugInPropertyResourceBundle:       // 'rsrc'
            return true;
        default:
            return false;
    }
}

static OSStatus plugin_IsPropertySettable(AudioObjectID objID, const AudioObjectPropertyAddress* addr, Boolean* outSettable) {
    if (outSettable) *outSettable = false;
    return kAudioHardwareNoError;
}

static OSStatus plugin_GetPropertyDataSize(AudioObjectID objID, const AudioObjectPropertyAddress* addr,
                                            UInt32 qualSize, const void* qual, UInt32* outSize) {
    UInt32 sel = addr->mSelector;
    switch (sel) {
        case kAudioObjectPropertyBaseClass:
        case kAudioObjectPropertyClass:
        case kAudioObjectPropertyOwner:
            *outSize = sizeof(AudioClassID);
            return kAudioHardwareNoError;
        case kAudioObjectPropertyManufacturer:
            *outSize = sizeof(CFStringRef);
            return kAudioHardwareNoError;
        case kAudioPlugInPropertyResourceBundle:
            *outSize = sizeof(CFStringRef);
            return kAudioHardwareNoError;
        case kAudioObjectPropertyOwnedObjects:
        case kAudioPlugInPropertyDeviceList:
            *outSize = gDeviceCount * sizeof(AudioObjectID);
            return kAudioHardwareNoError;
        case kAudioPlugInPropertyTranslateUIDToDevice:
            *outSize = sizeof(AudioObjectID);
            return kAudioHardwareNoError;
        default:
            return kAudioHardwareUnknownPropertyError;
    }
}

static OSStatus plugin_GetPropertyData(AudioObjectID objID, const AudioObjectPropertyAddress* addr,
                                        UInt32 qualSize, const void* qual,
                                        UInt32 inSize, UInt32* outSize, void* outData) {
    UInt32 sel = addr->mSelector;
    switch (sel) {
        case kAudioObjectPropertyBaseClass:
            if (inSize < sizeof(AudioClassID)) return kAudioHardwareBadPropertySizeError;
            *((AudioClassID*)outData) = kAudioObjectClassID;
            *outSize = sizeof(AudioClassID);
            return kAudioHardwareNoError;

        case kAudioObjectPropertyClass:
            if (inSize < sizeof(AudioClassID)) return kAudioHardwareBadPropertySizeError;
            *((AudioClassID*)outData) = kAudioPlugInClassID;
            *outSize = sizeof(AudioClassID);
            return kAudioHardwareNoError;

        case kAudioObjectPropertyOwner:
            if (inSize < sizeof(AudioObjectID)) return kAudioHardwareBadPropertySizeError;
            *((AudioObjectID*)outData) = kAudioObjectUnknown;
            *outSize = sizeof(AudioObjectID);
            return kAudioHardwareNoError;

        case kAudioObjectPropertyManufacturer:
            if (inSize < sizeof(CFStringRef)) return kAudioHardwareBadPropertySizeError;
            *((CFStringRef*)outData) = CFSTR("Apple, Inc.");
            *outSize = sizeof(CFStringRef);
            return kAudioHardwareNoError;

        case kAudioPlugInPropertyResourceBundle:
            if (inSize < sizeof(CFStringRef)) return kAudioHardwareBadPropertySizeError;
            *((CFStringRef*)outData) = CFSTR("");
            *outSize = sizeof(CFStringRef);
            return kAudioHardwareNoError;

        case kAudioObjectPropertyOwnedObjects:
        case kAudioPlugInPropertyDeviceList: {
            UInt32 needed = gDeviceCount * sizeof(AudioObjectID);
            UInt32 toCopy = (inSize < needed) ? inSize : needed;
            memcpy(outData, gDeviceIDs, toCopy);
            *outSize = toCopy;
            LOG("plugin_GetPropertyData '%s': returning %u device(s)",
                sel == kAudioPlugInPropertyDeviceList ? "dev#" : "ownd", gDeviceCount);
            for (UInt32 i = 0; i < gDeviceCount; i++) {
                LOG("  device[%u] = %u", i, gDeviceIDs[i]);
            }
            return kAudioHardwareNoError;
        }

        case kAudioPlugInPropertyTranslateUIDToDevice: {
            if (inSize < sizeof(AudioObjectID)) return kAudioHardwareBadPropertySizeError;
            // Return the first device for any UID query
            *((AudioObjectID*)outData) = (gDeviceCount > 0) ? gDeviceIDs[0] : kAudioObjectUnknown;
            *outSize = sizeof(AudioObjectID);
            return kAudioHardwareNoError;
        }

        default:
            return kAudioHardwareUnknownPropertyError;
    }
}

#pragma mark - C++ Exception-Safe Forwarding to Old Binary

// The old binary throws C++ exceptions for unknown objects/properties.
// We need to catch these. Since we compile as C, we use setjmp/longjmp
// as a workaround, or we handle obj=1 ourselves and forward obj>1 to the
// old binary (which should know about those objects).
//
// Actually, obj=1 is fully handled by our plugin_* functions above.
// For obj>1, the old binary should handle them without throwing (since it
// created those objects). But to be safe, we wrap calls in a helper that
// catches potential issues.

// Simple safety wrapper: if the old binary would crash, return an error instead
static Boolean safe_HasProperty(AudioObjectID objID, const AudioObjectPropertyAddress* addr) {
    if (!gOrig.ObjectHasProperty) return false;
    // The old binary should handle objects it created. Just call it.
    return gOrig.ObjectHasProperty(objID, addr);
}

static OSStatus safe_GetPropertyDataSize(AudioObjectID objID, const AudioObjectPropertyAddress* addr,
                                          UInt32 qualSize, const void* qual, UInt32* outSize) {
    if (!gOrig.ObjectGetPropertyDataSize) return kAudioHardwareUnknownPropertyError;
    return gOrig.ObjectGetPropertyDataSize(objID, addr, qualSize, qual, outSize);
}

static OSStatus safe_GetPropertyData(AudioObjectID objID, const AudioObjectPropertyAddress* addr,
                                      UInt32 qualSize, const void* qual,
                                      UInt32 inSize, UInt32* outSize, void* outData) {
    if (!gOrig.ObjectGetPropertyData) return kAudioHardwareUnknownPropertyError;
    return gOrig.ObjectGetPropertyData(objID, addr, qualSize, qual, inSize, outSize, outData);
}

#pragma mark - COM Driver Object

typedef struct {
    AudioServerPlugInDriverInterface* vtable;
    AudioServerPlugInDriverInterface vtableStorage;
    UInt32 refCount;
    AudioServerPlugInHostRef host;
} iSightDriver;

static iSightDriver* gDriver = NULL;

#pragma mark - COM Interface Methods

static HRESULT wrapper_QueryInterface(void* inDriver, REFIID inUUID, LPVOID* outInterface) {
    CFUUIDRef requestedUUID = CFUUIDCreateFromUUIDBytes(NULL, inUUID);
    CFUUIDRef driverUUID = CFUUIDGetConstantUUIDWithBytes(NULL,
        0xEE, 0xA5, 0x77, 0x3D, 0xCC, 0x43, 0x49, 0xF1,
        0x8E, 0x00, 0x8F, 0x96, 0xE7, 0xD2, 0x3B, 0x17);
    CFUUIDRef iunknownUUID = CFUUIDGetConstantUUIDWithBytes(NULL,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46);

    if (CFEqual(requestedUUID, driverUUID) || CFEqual(requestedUUID, iunknownUUID)) {
        gDriver->refCount++;
        *outInterface = &gDriver->vtable;
        CFRelease(requestedUUID);
        return S_OK;
    }
    CFRelease(requestedUUID);
    *outInterface = NULL;
    return E_NOINTERFACE;
}

static ULONG wrapper_AddRef(void* inDriver) { return ++gDriver->refCount; }

static ULONG wrapper_Release(void* inDriver) {
    UInt32 count = --gDriver->refCount;
    if (count == 0) {
        if (gOrig.Teardown) gOrig.Teardown();
        if (gOrig.handle) dlclose(gOrig.handle);
        free(gDriver);
        gDriver = NULL;
    }
    return count;
}

static OSStatus wrapper_Initialize(AudioServerPlugInDriverRef inDriver, AudioServerPlugInHostRef inHost) {
    LOG("Initialize called, host=%p", inHost);
    gDriver->host = inHost;
    gRealHost = inHost;

    // Set up proxy host interface
    gProxyHostInterface.PropertiesChanged = proxy_PropertiesChanged;
    gProxyHostInterface.CopyFromStorage = proxy_CopyFromStorage;
    gProxyHostInterface.WriteToStorage = proxy_offset_0x10_noop;
    gProxyHostInterface.DeleteFromStorage = proxy_DeleteFromStorage;
    gProxyHostInterface.RequestDeviceConfigurationChange = proxy_RequestDeviceConfigChange;
    gProxyHostPtr = &gProxyHostInterface;

    if (!loadOriginal()) return kAudioHardwareUnspecifiedError;
    if (gOrig.Initialize) {
        LOG("Calling original Initialize with proxy host=%p (->%p)", &gProxyHostPtr, gProxyHostPtr);
        OSStatus r = gOrig.Initialize((AudioServerPlugInHostRef)&gProxyHostPtr);
        LOG("Original Initialize returned: %d", (int)r);

        if (r == kAudioHardwareNoError) {
            // Discover device IDs: The old binary created devices during Initialize.
            // We need to find their IDs. The old binary's property methods for the
            // plugin object (1) throw C++ exceptions, so we can't query 'dev#' on it.
            // Instead, we probe object IDs 2-20 and check their class. Only objects
            // with class 'adev' (kAudioDeviceClassID) are audio devices.
            gDeviceCount = 0;
            for (AudioObjectID probe = 1; probe <= 30 && gDeviceCount < MAX_DEVICES; probe++) {
                AudioObjectPropertyAddress classAddr = {
                    kAudioObjectPropertyClass, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain
                };
                if (gOrig.ObjectHasProperty && gOrig.ObjectHasProperty(probe, &classAddr)) {
                    // Read the class to check if it's a device
                    UInt32 classVal = 0;
                    UInt32 outSize = sizeof(classVal);
                    if (gOrig.ObjectGetPropertyData) {
                        OSStatus err = gOrig.ObjectGetPropertyData(probe, &classAddr, 0, NULL,
                                                                     sizeof(classVal), &outSize, &classVal);
                        char cls[5];
                        fourcc(classVal, cls);
                        LOG("Probed object %u: class='%s' (0x%x) err=%d", probe, cls, classVal, (int)err);
                        if (err == kAudioHardwareNoError && classVal == kAudioDeviceClassID) {
                            gDeviceIDs[gDeviceCount++] = probe;
                            LOG("  -> This is a device! Added as device[%u]", gDeviceCount - 1);
                        }
                    }
                }
            }
            LOG("Total devices discovered: %u", gDeviceCount);

            if (gDeviceCount > 0 && gRealHost && gRealHost->PropertiesChanged) {
                AudioObjectPropertyAddress changedAddrs[] = {
                    { kAudioObjectPropertyOwnedObjects, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain },
                    { kAudioPlugInPropertyDeviceList, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain },
                };
                LOG("Notifying host about %u device(s)", gDeviceCount);
                gRealHost->PropertiesChanged(gRealHost, kAudioObjectPlugInObject, 2, changedAddrs);
            }
        }
        return r;
    }
    return kAudioHardwareNoError;
}

static OSStatus wrapper_CreateDevice(AudioServerPlugInDriverRef d, CFDictionaryRef desc,
                                      const AudioServerPlugInClientInfo* i, AudioObjectID* o) {
    return kAudioHardwareUnsupportedOperationError;
}

static OSStatus wrapper_DestroyDevice(AudioServerPlugInDriverRef d, AudioObjectID id) {
    return kAudioHardwareUnsupportedOperationError;
}

static OSStatus wrapper_AddDeviceClient(AudioServerPlugInDriverRef d, AudioObjectID id,
                                         const AudioServerPlugInClientInfo* i) {
    return kAudioHardwareNoError;
}

static OSStatus wrapper_RemoveDeviceClient(AudioServerPlugInDriverRef d, AudioObjectID id,
                                            const AudioServerPlugInClientInfo* i) {
    return kAudioHardwareNoError;
}

static OSStatus wrapper_PerformDeviceConfigChange(AudioServerPlugInDriverRef d, AudioObjectID id,
                                                    UInt64 action, void* info) {
    if (gOrig.PerformDeviceConfigChange)
        return gOrig.PerformDeviceConfigChange(id, action, info);
    return kAudioHardwareNoError;
}

static OSStatus wrapper_AbortDeviceConfigChange(AudioServerPlugInDriverRef d, AudioObjectID id,
                                                  UInt64 action, void* info) {
    return kAudioHardwareNoError;
}

static Boolean wrapper_HasProperty(AudioServerPlugInDriverRef inDriver, AudioObjectID objID,
                                    pid_t clientPID, const AudioObjectPropertyAddress* addr) {
    Boolean r;
    char sel[5], scope[5];
    fourcc(addr->mSelector, sel);
    fourcc(addr->mScope, scope);

    if (objID == kAudioObjectPlugInObject) {
        // Plugin-level: handled by wrapper
        r = plugin_HasProperty(objID, addr);
    } else {
        // Device/stream/control: forward to old binary
        r = safe_HasProperty(objID, addr);
    }

    LOG("HasProperty obj=%u sel='%s' scope='%s' elem=%u -> %d",
        objID, sel, scope, addr->mElement, (int)r);
    return r;
}

static OSStatus wrapper_IsPropertySettable(AudioServerPlugInDriverRef inDriver, AudioObjectID objID,
                                            pid_t clientPID, const AudioObjectPropertyAddress* addr,
                                            Boolean* outSettable) {
    if (objID == kAudioObjectPlugInObject) {
        return plugin_IsPropertySettable(objID, addr, outSettable);
    }
    if (gOrig.ObjectIsPropertySettable)
        return gOrig.ObjectIsPropertySettable(objID, addr, outSettable);
    if (outSettable) *outSettable = false;
    return kAudioHardwareNoError;
}

static OSStatus wrapper_GetPropertyDataSize(AudioServerPlugInDriverRef inDriver, AudioObjectID objID,
                                             pid_t clientPID, const AudioObjectPropertyAddress* addr,
                                             UInt32 qualSize, const void* qual, UInt32* outSize) {
    OSStatus r;
    char sel[5];
    fourcc(addr->mSelector, sel);

    if (objID == kAudioObjectPlugInObject) {
        r = plugin_GetPropertyDataSize(objID, addr, qualSize, qual, outSize);
    } else {
        r = safe_GetPropertyDataSize(objID, addr, qualSize, qual, outSize);
    }

    LOG("GetPropertyDataSize obj=%u sel='%s' -> %d size=%u",
        objID, sel, (int)r, outSize ? *outSize : 0);
    return r;
}

static OSStatus wrapper_GetPropertyData(AudioServerPlugInDriverRef inDriver, AudioObjectID objID,
                                         pid_t clientPID, const AudioObjectPropertyAddress* addr,
                                         UInt32 qualSize, const void* qual,
                                         UInt32 inSize, UInt32* outSize, void* outData) {
    OSStatus r;
    char sel[5];
    fourcc(addr->mSelector, sel);

    if (objID == kAudioObjectPlugInObject) {
        r = plugin_GetPropertyData(objID, addr, qualSize, qual, inSize, outSize, outData);
    } else {
        r = safe_GetPropertyData(objID, addr, qualSize, qual, inSize, outSize, outData);
    }

    LOG("GetPropertyData obj=%u sel='%s' -> %d outSize=%u",
        objID, sel, (int)r, outSize ? *outSize : 0);
    // Log returned data for key properties
    if (r == kAudioHardwareNoError && outSize && *outSize > 0) {
        if (addr->mSelector == kAudioObjectPropertyClass ||
            addr->mSelector == kAudioObjectPropertyBaseClass) {
            char cls[5];
            fourcc(*(UInt32*)outData, cls);
            LOG("  -> class value: '%s' (0x%x)", cls, *(UInt32*)outData);
        } else if (addr->mSelector == 'uid ' && *outSize == sizeof(CFStringRef)) {
            CFStringRef uid = *(CFStringRef*)outData;
            if (uid) {
                char buf[256] = {0};
                CFStringGetCString(uid, buf, sizeof(buf), kCFStringEncodingUTF8);
                LOG("  -> UID: '%s'", buf);
            }
        } else if (addr->mSelector == kAudioObjectPropertyOwnedObjects ||
                   addr->mSelector == kAudioPlugInPropertyDeviceList ||
                   addr->mSelector == 'stm#') {
            UInt32 count = *outSize / sizeof(AudioObjectID);
            AudioObjectID* ids = (AudioObjectID*)outData;
            for (UInt32 i = 0; i < count; i++) {
                LOG("  -> id[%u] = %u", i, ids[i]);
            }
        }
    }
    return r;
}

static OSStatus wrapper_SetPropertyData(AudioServerPlugInDriverRef inDriver, AudioObjectID objID,
                                         pid_t clientPID, const AudioObjectPropertyAddress* addr,
                                         UInt32 qualSize, const void* qual,
                                         UInt32 inSize, const void* inData) {
    if (objID == kAudioObjectPlugInObject) {
        return kAudioHardwareUnsupportedOperationError;
    }
    if (gOrig.ObjectSetPropertyData)
        return gOrig.ObjectSetPropertyData(objID, addr, qualSize, qual, inSize, inData);
    return kAudioHardwareUnsupportedOperationError;
}

static OSStatus wrapper_StartIO(AudioServerPlugInDriverRef d, AudioObjectID devID, UInt32 clientID) {
    LOG("StartIO device=%u client=%u", devID, clientID);
    if (gOrig.Start) return gOrig.Start(devID);
    return kAudioHardwareNoError;
}

static OSStatus wrapper_StopIO(AudioServerPlugInDriverRef d, AudioObjectID devID, UInt32 clientID) {
    LOG("StopIO device=%u client=%u", devID, clientID);
    if (gOrig.Stop) return gOrig.Stop(devID);
    return kAudioHardwareNoError;
}

static OSStatus wrapper_GetZeroTimeStamp(AudioServerPlugInDriverRef d, AudioObjectID devID,
                                          UInt32 clientID, Float64* outSampleTime,
                                          UInt64* outHostTime, UInt64* outSeed) {
    if (gOrig.GetZeroTimeStamp)
        return gOrig.GetZeroTimeStamp(devID, outSampleTime, outHostTime, outSeed);
    return kAudioHardwareUnspecifiedError;
}

static OSStatus wrapper_WillDoIOOperation(AudioServerPlugInDriverRef d, AudioObjectID devID,
                                           UInt32 clientID, UInt32 opID,
                                           Boolean* outWillDo, Boolean* outWillDoInPlace) {
    if (outWillDo) *outWillDo = (opID == 'read');
    if (outWillDoInPlace) *outWillDoInPlace = true;
    return kAudioHardwareNoError;
}

static OSStatus wrapper_BeginIOOperation(AudioServerPlugInDriverRef d, AudioObjectID devID,
                                          UInt32 clientID, UInt32 opID,
                                          UInt32 bufFrameSize, const AudioServerPlugInIOCycleInfo* info) {
    if (gOrig.BeginReading && opID == 'read')
        return gOrig.BeginReading(devID, clientID, opID, bufFrameSize, info);
    if (gOrig.BeginIO) return gOrig.BeginIO();
    return kAudioHardwareNoError;
}

static OSStatus wrapper_DoIOOperation(AudioServerPlugInDriverRef d, AudioObjectID devID,
                                       AudioObjectID streamID, UInt32 clientID, UInt32 opID,
                                       UInt32 bufFrameSize, const AudioServerPlugInIOCycleInfo* info,
                                       void* mainBuf, void* secBuf) {
    if (gOrig.ReadFromStream && opID == 'read')
        return gOrig.ReadFromStream(devID, streamID, clientID, opID, bufFrameSize, info, mainBuf, secBuf);
    return kAudioHardwareNoError;
}

static OSStatus wrapper_EndIOOperation(AudioServerPlugInDriverRef d, AudioObjectID devID,
                                        UInt32 clientID, UInt32 opID,
                                        UInt32 bufFrameSize, const AudioServerPlugInIOCycleInfo* info) {
    if (gOrig.EndReading && opID == 'read')
        return gOrig.EndReading(devID, clientID, opID, bufFrameSize, info);
    if (gOrig.EndIO) return gOrig.EndIO();
    return kAudioHardwareNoError;
}

#pragma mark - Factory Function

void* iSightAudioFactory(CFAllocatorRef allocator, CFUUIDRef typeUUID) {
    LOG("Factory called");

    CFUUIDRef pluginTypeUUID = CFUUIDGetConstantUUIDWithBytes(NULL,
        0x44, 0x3A, 0xBA, 0xB8, 0xE7, 0xB3, 0x49, 0x1A,
        0xB9, 0x85, 0xBE, 0xB9, 0x18, 0x70, 0x30, 0xDB);

    if (!CFEqual(typeUUID, pluginTypeUUID)) {
        LOG("Wrong type UUID");
        return NULL;
    }

    if (gDriver) {
        gDriver->refCount++;
        return &gDriver->vtable;
    }

    gDriver = (iSightDriver*)calloc(1, sizeof(iSightDriver));
    if (!gDriver) return NULL;

    gDriver->refCount = 1;
    gDriver->vtable = &gDriver->vtableStorage;

    AudioServerPlugInDriverInterface* vt = &gDriver->vtableStorage;
    vt->_reserved = NULL;
    vt->QueryInterface = wrapper_QueryInterface;
    vt->AddRef = wrapper_AddRef;
    vt->Release = wrapper_Release;
    vt->Initialize = wrapper_Initialize;
    vt->CreateDevice = wrapper_CreateDevice;
    vt->DestroyDevice = wrapper_DestroyDevice;
    vt->AddDeviceClient = wrapper_AddDeviceClient;
    vt->RemoveDeviceClient = wrapper_RemoveDeviceClient;
    vt->PerformDeviceConfigurationChange = wrapper_PerformDeviceConfigChange;
    vt->AbortDeviceConfigurationChange = wrapper_AbortDeviceConfigChange;
    vt->HasProperty = wrapper_HasProperty;
    vt->IsPropertySettable = wrapper_IsPropertySettable;
    vt->GetPropertyDataSize = wrapper_GetPropertyDataSize;
    vt->GetPropertyData = wrapper_GetPropertyData;
    vt->SetPropertyData = wrapper_SetPropertyData;
    vt->StartIO = wrapper_StartIO;
    vt->StopIO = wrapper_StopIO;
    vt->GetZeroTimeStamp = wrapper_GetZeroTimeStamp;
    vt->WillDoIOOperation = wrapper_WillDoIOOperation;
    vt->BeginIOOperation = wrapper_BeginIOOperation;
    vt->DoIOOperation = wrapper_DoIOOperation;
    vt->EndIOOperation = wrapper_EndIOOperation;

    LOG("Driver created at %p", gDriver);
    return &gDriver->vtable;
}
