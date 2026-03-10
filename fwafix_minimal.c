// fwafix_minimal.c — Guard fix + DMA retry + crash handler
// The binary already has sandbox_init NOP'd at 0xE05D
//
// Two DYLD interpositions:
//   1. Guard fix: mach_port_deallocate → mach_port_destruct (Sonoma guarded ports)
//   2. DMA retry: IOConnectCallScalarMethod — retries DMA Start (sel 0x20031) on
//      kIOReturnDMAError (0xe00002d4) which happens when old DCL buffers land above 4GB
//
// Compile: clang -arch x86_64 -shared -o fwafix_minimal.dylib fwafix_minimal.c -lSystem -framework IOKit -Wno-deprecated-declarations
// Sign:    codesign -s - fwafix_minimal.dylib
// Install: sudo cp fwafix_minimal.dylib /usr/local/lib/libfwafix.dylib

#include <mach/mach.h>
#include <mach/mach_port.h>
#include <IOKit/IOKitLib.h>
#include <stdio.h>
#include <unistd.h>
#include <signal.h>
#include <stdlib.h>

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

// ---- DMA retry: old DCL buffers above 4GB cause kIOReturnDMAError ----
kern_return_t retrying_IOConnectCallScalarMethod(
    io_connect_t connection, uint32_t selector,
    const uint64_t *input, uint32_t inputCnt,
    uint64_t *output, uint32_t *outputCnt) {

    kern_return_t kr = IOConnectCallScalarMethod(connection, selector,
        input, inputCnt, output, outputCnt);

    // DMA Start (sel=0x20031) error 0xe00002d4 = kIOReturnDMAError
    // Old DCL buffers may land above 4GB; retry forces new allocation.
    // LIMIT to 3 retries to avoid kernel panic (ReceiveDCL::link() stack
    // overflow triggers after repeated compile() calls). If 3 retries fail,
    // exit and let launchd respawn us with fresh memory allocation.
    if (selector == 0x20031 && kr == (kern_return_t)0xe00002d4) {
        for (int retry = 1; retry <= 3; retry++) {
            fwlog("[FWAFIX] DMA Start failed (0xe00002d4), retry %d/3...\n", retry);
            usleep(200000); // 200ms — longer delay to let kernel settle
            kr = IOConnectCallScalarMethod(connection, selector,
                input, inputCnt, output, outputCnt);
            if (kr == KERN_SUCCESS) {
                fwlog("[FWAFIX] DMA Start SUCCEEDED on retry %d!\n", retry);
                break;
            }
        }
        if (kr != KERN_SUCCESS) {
            fwlog("[FWAFIX] DMA Start FAILED after 3 retries — exiting for launchd respawn\n");
            // Exit cleanly — launchd will respawn with fresh memory layout
            // This is safer than hammering the kernel with more retries
            _exit(1);
        }
    }

    // Log isoch-range calls and errors only (minimal overhead)
    if (selector >= 0x20000 || kr != KERN_SUCCESS) {
        fwlog("[FWAFIX] ScalarMethod(sel=0x%x) -> 0x%x%s\n",
              selector, kr, kr != KERN_SUCCESS ? " ERROR" : "");
    }
    return kr;
}

// ---- Crash handler: bus reset on fatal signal to reclaim IRM bandwidth ----
static void crash_handler(int sig) {
    fwlog("[FWAFIX] FATAL signal %d — attempting FireWire bus reset\n", sig);
    system("/usr/local/libexec/fw_bus_reset 2>/dev/null");
    fwlog("[FWAFIX] Bus reset attempted, exiting\n");
    if (gLog) fflush(gLog);
    _exit(128 + sig);
}

// ---- Constructor ----
__attribute__((constructor))
static void fwafix_init(void) {
    fwlog("[FWAFIX] Guard fix + DMA retry loaded PID=%d\n", getpid());
    signal(SIGSEGV, crash_handler);
    signal(SIGABRT, crash_handler);
    signal(SIGBUS, crash_handler);
}

// ---- DYLD interposition: guard fix + DMA retry ----
struct interpose_s { void *replacement; void *original; };
__attribute__((used))
static const struct interpose_s interposers[] __attribute__((section("__DATA,__interpose"))) = {
    { (void *)safe_mach_port_deallocate, (void *)mach_port_deallocate },
    { (void *)retrying_IOConnectCallScalarMethod, (void *)IOConnectCallScalarMethod },
};
