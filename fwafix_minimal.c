// fwafix_minimal.c — Minimal guard fix ONLY
// The binary already has sandbox_init NOP'd at 0xE05D
// No IOKit tracing interpositions — those break FaceTime's CMIO path
//
// Compile: clang -arch x86_64 -shared -o fwafix_minimal.dylib fwafix_minimal.c -lSystem -Wno-deprecated-declarations
// Sign:    codesign -s - fwafix_minimal.dylib
// Install: sudo cp fwafix_minimal.dylib /usr/local/lib/libfwafix.dylib

#include <mach/mach.h>
#include <mach/mach_port.h>
#include <stdio.h>
#include <unistd.h>

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

// ---- Constructor ----
__attribute__((constructor))
static void fwafix_init(void) {
    fwlog("[FWAFIX] Minimal guard-fix loaded PID=%d\n", getpid());
}

// ---- DYLD interposition: ONLY guard fix ----
struct interpose_s { void *replacement; void *original; };
__attribute__((used))
static const struct interpose_s interposers[] __attribute__((section("__DATA,__interpose"))) = {
    { (void *)safe_mach_port_deallocate, (void *)mach_port_deallocate },
};
