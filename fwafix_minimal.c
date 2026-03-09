// fwafix_minimal.c — Guard fix + crash cleanup for FireWire iSight
// The binary already has sandbox_init NOP'd at 0xE05D
// No IOKit tracing interpositions — those break FaceTime's CMIO path
//
// Compile: clang -arch x86_64 -shared -o fwafix_minimal.dylib fwafix_minimal.c \
//          -framework IOKit -framework CoreFoundation -lSystem -Wno-deprecated-declarations
// Sign:    codesign -s - fwafix_minimal.dylib
// Install: sudo cp fwafix_minimal.dylib /usr/local/lib/libfwafix.dylib

#include <mach/mach.h>
#include <mach/mach_port.h>
#include <stdio.h>
#include <signal.h>
#include <unistd.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/IOCFPlugIn.h>
#include <IOKit/firewire/IOFireWireLib.h>
#include <CoreFoundation/CoreFoundation.h>

static FILE* gLog = NULL;
static void fwlog(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
static void fwlog(const char* fmt, ...) {
    __builtin_va_list args;
    if (!gLog) gLog = fopen("/tmp/fwafix.log", "a");
    if (gLog) {
        __builtin_va_start(args, fmt);
        vfprintf(gLog, fmt, args);
        __builtin_va_end(args);
        fflush(gLog);
    }
}

// ---- Guard fix: mach_port_deallocate → mach_port_destruct ----
extern kern_return_t mach_port_destruct(ipc_space_t, mach_port_name_t, mach_port_delta_t, mach_port_context_t);

kern_return_t safe_mach_port_deallocate(ipc_space_t task, mach_port_name_t name) {
    mach_port_destruct(task, name, 0, 0);
    return KERN_SUCCESS;
}

// ---- Crash cleanup: bus reset to reclaim leaked IRM bandwidth ----
// When IIDCVideoAssistant crashes, isochronous bandwidth leaks on the FireWire bus.
// If it crash-loops, leaked resources accumulate and eventually cause a kernel panic
// in AppleFWOHCI_ReceiveDCL::link() (stack corruption from stale isochronous state).
// This handler catches crash signals and fires a bus reset before exiting.

static volatile sig_atomic_t in_handler = 0;

static void crash_bus_reset(int sig) {
    if (in_handler) _exit(128 + sig);
    in_handler = 1;

    const char msg[] = "[FWAFIX] Crash caught — doing bus reset to reclaim IRM bandwidth\n";
    write(STDERR_FILENO, msg, sizeof(msg) - 1);

    CFMutableDictionaryRef matching = IOServiceMatching("IOFireWireUnit");
    io_iterator_t iter;
    if (IOServiceGetMatchingServices(kIOMainPortDefault, matching, &iter) == KERN_SUCCESS) {
        io_service_t service, target = 0;
        while ((service = IOIteratorNext(iter))) {
            CFNumberRef a = (CFNumberRef)IORegistryEntryCreateCFProperty(service, CFSTR("Unit_Spec_ID"), kCFAllocatorDefault, 0);
            CFNumberRef b = (CFNumberRef)IORegistryEntryCreateCFProperty(service, CFSTR("Unit_SW_Version"), kCFAllocatorDefault, 0);
            int x = 0, y = 0;
            if (a) { CFNumberGetValue(a, kCFNumberIntType, &x); CFRelease(a); }
            if (b) { CFNumberGetValue(b, kCFNumberIntType, &y); CFRelease(b); }
            if (x == 0xA27 && y == 16) { target = service; break; }
            IOObjectRelease(service);
        }
        IOObjectRelease(iter);

        if (target) {
            IOCFPlugInInterface** plugIn = NULL; SInt32 score = 0;
            IOCreatePlugInInterfaceForService(target, kIOFireWireLibTypeID, kIOCFPlugInInterfaceID, &plugIn, &score);
            IOObjectRelease(target);
            if (plugIn) {
                IOFireWireLibDeviceRef dev = NULL;
                (*plugIn)->QueryInterface(plugIn, CFUUIDGetUUIDBytes(kIOFireWireDeviceInterfaceID_v9), (LPVOID*)&dev);
                (*plugIn)->Release(plugIn);
                if (dev) {
                    IOReturn r = (*dev)->Open(dev);
                    if (r != 0) r = (*dev)->OpenWithSessionRef(dev, (IOFireWireSessionRef)0x1234);
                    if (r == 0) {
                        (*dev)->BusReset(dev);
                        usleep(200000);
                    }
                    (*dev)->Close(dev);
                    (*dev)->Release(dev);
                }
            }
        }
    }

    const char done[] = "[FWAFIX] Bus reset done, exiting\n";
    write(STDERR_FILENO, done, sizeof(done) - 1);

    signal(sig, SIG_DFL);
    raise(sig);
}

// ---- Constructor ----
__attribute__((constructor))
static void fwafix_init(void) {
    fwlog("[FWAFIX] Guard fix + crash handler loaded PID=%d\n", getpid());

    struct sigaction sa;
    sa.sa_handler = crash_bus_reset;
    sa.sa_flags = SA_RESETHAND;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGABRT, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);

    fwlog("[FWAFIX] Crash handler installed — will bus reset on crash\n");
}

// ---- DYLD interposition: ONLY guard fix ----
struct interpose_s { void *replacement; void *original; };
__attribute__((used))
static const struct interpose_s interposers[] __attribute__((section("__DATA,__interpose"))) = {
    { (void *)safe_mach_port_deallocate, (void *)mach_port_deallocate },
};
