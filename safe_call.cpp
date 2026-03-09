// C++ exception-safe wrappers for calling the old binary's functions
#include <CoreAudio/AudioServerPlugIn.h>
#include <stdio.h>
#include <typeinfo>
#include <exception>

static FILE* safeLog = nullptr;
static void ensureSafeLog() {
    if (!safeLog) {
        safeLog = fopen("/tmp/isight_audio.log", "a");
        if (safeLog) setbuf(safeLog, NULL);
    }
}

extern "C" {

typedef Boolean (*HasPropFunc)(AudioObjectID, const AudioObjectPropertyAddress*);
typedef OSStatus (*GetPropSizeFunc)(AudioObjectID, const AudioObjectPropertyAddress*, UInt32, const void*, UInt32*);
typedef OSStatus (*GetPropDataFunc)(AudioObjectID, const AudioObjectPropertyAddress*, UInt32, const void*, UInt32, UInt32*, void*);
typedef OSStatus (*SetPropDataFunc)(AudioObjectID, const AudioObjectPropertyAddress*, UInt32, const void*, UInt32, const void*);
typedef OSStatus (*IsSettableFunc)(AudioObjectID, const AudioObjectPropertyAddress*, Boolean*);

Boolean safe_call_HasProperty(HasPropFunc fn, AudioObjectID objID, const AudioObjectPropertyAddress* addr) {
    try {
        return fn(objID, addr);
    } catch (const std::exception& e) {
        ensureSafeLog();
        if (safeLog) fprintf(safeLog, "[iSightAudioWrap] C++ exception in HasProperty obj=%u sel=0x%x: %s\n",
                             objID, addr->mSelector, e.what());
        return false;
    } catch (...) {
        ensureSafeLog();
        if (safeLog) fprintf(safeLog, "[iSightAudioWrap] C++ exception in HasProperty obj=%u sel=0x%x: unknown\n",
                             objID, addr->mSelector);
        return false;
    }
}

OSStatus safe_call_IsPropertySettable(IsSettableFunc fn, AudioObjectID objID,
                                       const AudioObjectPropertyAddress* addr, Boolean* outSettable) {
    try {
        return fn(objID, addr, outSettable);
    } catch (...) {
        if (outSettable) *outSettable = false;
        return kAudioHardwareUnknownPropertyError;
    }
}

OSStatus safe_call_GetPropertyDataSize(GetPropSizeFunc fn, AudioObjectID objID,
                                        const AudioObjectPropertyAddress* addr,
                                        UInt32 qualSize, const void* qual, UInt32* outSize) {
    try {
        return fn(objID, addr, qualSize, qual, outSize);
    } catch (...) {
        if (outSize) *outSize = 0;
        return kAudioHardwareUnknownPropertyError;
    }
}

OSStatus safe_call_GetPropertyData(GetPropDataFunc fn, AudioObjectID objID,
                                    const AudioObjectPropertyAddress* addr,
                                    UInt32 qualSize, const void* qual,
                                    UInt32 inSize, UInt32* outSize, void* outData) {
    try {
        return fn(objID, addr, qualSize, qual, inSize, outSize, outData);
    } catch (const std::exception& e) {
        ensureSafeLog();
        if (safeLog) fprintf(safeLog, "[iSightAudioWrap] C++ exception in GetPropertyData obj=%u sel=0x%x: %s\n",
                             objID, addr->mSelector, e.what());
        return kAudioHardwareUnknownPropertyError;
    } catch (...) {
        ensureSafeLog();
        if (safeLog) fprintf(safeLog, "[iSightAudioWrap] C++ exception in GetPropertyData obj=%u sel=0x%x: unknown\n",
                             objID, addr->mSelector);
        return kAudioHardwareUnknownPropertyError;
    }
}

OSStatus safe_call_SetPropertyData(SetPropDataFunc fn, AudioObjectID objID,
                                    const AudioObjectPropertyAddress* addr,
                                    UInt32 qualSize, const void* qual,
                                    UInt32 inSize, const void* inData) {
    try {
        return fn(objID, addr, qualSize, qual, inSize, inData);
    } catch (...) {
        return kAudioHardwareUnsupportedOperationError;
    }
}

} // extern "C"
