// C++ exception-safe wrappers for calling the old binary's functions.
//
// IMPORTANT: try/catch only catches C++ exceptions, NOT segfaults (SIGSEGV).
// The old binary has broken pointers in some code paths (e.g. SetPropertyData
// dereferences address 0x11). To protect coreaudiod from crashes:
//   1. Wrapper blocks known-dangerous calls (SetPropertyData) entirely
//   2. This file uses setjmp/longjmp + SIGSEGV handler as last resort
//   3. try/catch still handles C++ exceptions from the old binary

#include <CoreAudio/AudioServerPlugIn.h>
#include <stdio.h>
#include <signal.h>
#include <setjmp.h>
#include <pthread.h>
#include <typeinfo>
#include <exception>

static FILE* safeLog = nullptr;
static void ensureSafeLog() {
    if (!safeLog) {
        safeLog = fopen("/tmp/isight_audio.log", "a");
        if (safeLog) setbuf(safeLog, NULL);
    }
}

// Per-thread jmp_buf for SIGSEGV recovery
static __thread sigjmp_buf sSigJmpBuf;
static __thread volatile sig_atomic_t sSigGuardActive = 0;

static struct sigaction sOldSIGSEGV;
static struct sigaction sOldSIGBUS;
static bool sHandlersInstalled = false;

static void crash_handler(int sig, siginfo_t* info, void* ctx) {
    if (sSigGuardActive) {
        // We're inside a safe_call — recover via longjmp
        ensureSafeLog();
        if (safeLog) fprintf(safeLog, "[iSightAudioWrap] SIGNAL %d caught at addr %p — recovering\n",
                             sig, info ? info->si_addr : NULL);
        sSigGuardActive = 0;
        siglongjmp(sSigJmpBuf, 1);
    }
    // Not inside a safe_call — chain to previous handler
    struct sigaction* old = (sig == SIGSEGV) ? &sOldSIGSEGV : &sOldSIGBUS;
    if (old->sa_flags & SA_SIGINFO) {
        old->sa_sigaction(sig, info, ctx);
    } else if (old->sa_handler != SIG_DFL && old->sa_handler != SIG_IGN) {
        old->sa_handler(sig);
    } else {
        // Re-raise with default handler
        signal(sig, SIG_DFL);
        raise(sig);
    }
}

static void install_crash_handlers() {
    if (sHandlersInstalled) return;
    struct sigaction sa;
    sa.sa_sigaction = crash_handler;
    sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, &sOldSIGSEGV);
    sigaction(SIGBUS, &sa, &sOldSIGBUS);
    sHandlersInstalled = true;
    ensureSafeLog();
    if (safeLog) fprintf(safeLog, "[iSightAudioWrap] SIGSEGV/SIGBUS crash handlers installed\n");
}

extern "C" {

typedef Boolean (*HasPropFunc)(AudioObjectID, const AudioObjectPropertyAddress*);
typedef OSStatus (*GetPropSizeFunc)(AudioObjectID, const AudioObjectPropertyAddress*, UInt32, const void*, UInt32*);
typedef OSStatus (*GetPropDataFunc)(AudioObjectID, const AudioObjectPropertyAddress*, UInt32, const void*, UInt32, UInt32*, void*);
typedef OSStatus (*SetPropDataFunc)(AudioObjectID, const AudioObjectPropertyAddress*, UInt32, const void*, UInt32, const void*);
typedef OSStatus (*IsSettableFunc)(AudioObjectID, const AudioObjectPropertyAddress*, Boolean*);

Boolean safe_call_HasProperty(HasPropFunc fn, AudioObjectID objID, const AudioObjectPropertyAddress* addr) {
    install_crash_handlers();
    if (sigsetjmp(sSigJmpBuf, 1) != 0) {
        ensureSafeLog();
        if (safeLog) fprintf(safeLog, "[iSightAudioWrap] CRASH recovered in HasProperty obj=%u sel=0x%x\n",
                             objID, addr->mSelector);
        return false;
    }
    sSigGuardActive = 1;
    Boolean result = false;
    try {
        result = fn(objID, addr);
    } catch (const std::exception& e) {
        ensureSafeLog();
        if (safeLog) fprintf(safeLog, "[iSightAudioWrap] C++ exception in HasProperty obj=%u sel=0x%x: %s\n",
                             objID, addr->mSelector, e.what());
        result = false;
    } catch (...) {
        ensureSafeLog();
        if (safeLog) fprintf(safeLog, "[iSightAudioWrap] C++ exception in HasProperty obj=%u sel=0x%x: unknown\n",
                             objID, addr->mSelector);
        result = false;
    }
    sSigGuardActive = 0;
    return result;
}

OSStatus safe_call_IsPropertySettable(IsSettableFunc fn, AudioObjectID objID,
                                       const AudioObjectPropertyAddress* addr, Boolean* outSettable) {
    install_crash_handlers();
    if (sigsetjmp(sSigJmpBuf, 1) != 0) {
        if (outSettable) *outSettable = false;
        return kAudioHardwareUnknownPropertyError;
    }
    sSigGuardActive = 1;
    OSStatus r;
    try {
        r = fn(objID, addr, outSettable);
    } catch (...) {
        if (outSettable) *outSettable = false;
        r = kAudioHardwareUnknownPropertyError;
    }
    sSigGuardActive = 0;
    return r;
}

OSStatus safe_call_GetPropertyDataSize(GetPropSizeFunc fn, AudioObjectID objID,
                                        const AudioObjectPropertyAddress* addr,
                                        UInt32 qualSize, const void* qual, UInt32* outSize) {
    install_crash_handlers();
    if (sigsetjmp(sSigJmpBuf, 1) != 0) {
        if (outSize) *outSize = 0;
        return kAudioHardwareUnknownPropertyError;
    }
    sSigGuardActive = 1;
    OSStatus r;
    try {
        r = fn(objID, addr, qualSize, qual, outSize);
    } catch (...) {
        if (outSize) *outSize = 0;
        r = kAudioHardwareUnknownPropertyError;
    }
    sSigGuardActive = 0;
    return r;
}

OSStatus safe_call_GetPropertyData(GetPropDataFunc fn, AudioObjectID objID,
                                    const AudioObjectPropertyAddress* addr,
                                    UInt32 qualSize, const void* qual,
                                    UInt32 inSize, UInt32* outSize, void* outData) {
    install_crash_handlers();
    if (sigsetjmp(sSigJmpBuf, 1) != 0) {
        ensureSafeLog();
        if (safeLog) fprintf(safeLog, "[iSightAudioWrap] CRASH recovered in GetPropertyData obj=%u sel=0x%x\n",
                             objID, addr->mSelector);
        return kAudioHardwareUnknownPropertyError;
    }
    sSigGuardActive = 1;
    OSStatus r;
    try {
        r = fn(objID, addr, qualSize, qual, inSize, outSize, outData);
    } catch (const std::exception& e) {
        ensureSafeLog();
        if (safeLog) fprintf(safeLog, "[iSightAudioWrap] C++ exception in GetPropertyData obj=%u sel=0x%x: %s\n",
                             objID, addr->mSelector, e.what());
        r = kAudioHardwareUnknownPropertyError;
    } catch (...) {
        ensureSafeLog();
        if (safeLog) fprintf(safeLog, "[iSightAudioWrap] C++ exception in GetPropertyData obj=%u sel=0x%x: unknown\n",
                             objID, addr->mSelector);
        r = kAudioHardwareUnknownPropertyError;
    }
    sSigGuardActive = 0;
    return r;
}

OSStatus safe_call_SetPropertyData(SetPropDataFunc fn, AudioObjectID objID,
                                    const AudioObjectPropertyAddress* addr,
                                    UInt32 qualSize, const void* qual,
                                    UInt32 inSize, const void* inData) {
    // NOTE: wrapper_SetPropertyData already blocks all SetPropertyData calls
    // to the old binary. This function should never be reached, but if it is,
    // the crash guard will catch the SIGSEGV.
    install_crash_handlers();
    if (sigsetjmp(sSigJmpBuf, 1) != 0) {
        ensureSafeLog();
        if (safeLog) fprintf(safeLog, "[iSightAudioWrap] CRASH recovered in SetPropertyData obj=%u sel=0x%x\n",
                             objID, addr->mSelector);
        return kAudioHardwareUnsupportedOperationError;
    }
    sSigGuardActive = 1;
    OSStatus r;
    try {
        r = fn(objID, addr, qualSize, qual, inSize, inData);
    } catch (...) {
        r = kAudioHardwareUnsupportedOperationError;
    }
    sSigGuardActive = 0;
    return r;
}

} // extern "C"
