// Read iSight IIDC control/status registers via FireWire
// Uses vendor unit (Unit_Spec_ID=0xA27, SW=16) to avoid exclusive access conflicts
#include <IOKit/IOKitLib.h>
#include <IOKit/IOCFPlugIn.h>
#include <IOKit/firewire/IOFireWireLib.h>
#include <CoreFoundation/CoreFoundation.h>
#include <arpa/inet.h>
#include <cstdio>

static IOFireWireLibDeviceRef gDev = nullptr;
static UInt16 gNode = 0;
static UInt32 gGen = 0;

static uint32_t readReg(uint32_t addrLo, IOReturn *outResult = nullptr) {
    FWAddress addr;
    addr.nodeID = gNode;
    addr.addressHi = 0xFFFF;
    addr.addressLo = addrLo;
    uint32_t val = 0;
    IOReturn r = (*gDev)->ReadQuadlet(gDev, 0, &addr, &val, true, gGen);
    if (outResult) *outResult = r;
    if (r != 0) return 0xDEADDEAD;
    return ntohl(val);
}

int main() {
    printf("=== iSight IIDC Register Dump ===\n\n");

    // Find Apple vendor unit (Unit_Spec_ID=0xA27, SW=16)
    CFMutableDictionaryRef matching = IOServiceMatching("IOFireWireUnit");
    io_iterator_t iter;
    IOServiceGetMatchingServices(kIOMainPortDefault, matching, &iter);
    io_service_t service, target = 0;
    while ((service = IOIteratorNext(iter))) {
        CFNumberRef a = (CFNumberRef)IORegistryEntryCreateCFProperty(
            service, CFSTR("Unit_Spec_ID"), kCFAllocatorDefault, 0);
        CFNumberRef b = (CFNumberRef)IORegistryEntryCreateCFProperty(
            service, CFSTR("Unit_SW_Version"), kCFAllocatorDefault, 0);
        int x = 0, y = 0;
        if (a) { CFNumberGetValue(a, kCFNumberIntType, &x); CFRelease(a); }
        if (b) { CFNumberGetValue(b, kCFNumberIntType, &y); CFRelease(b); }
        if (x == 0x0A27 && y == 16) { target = service; break; }
        IOObjectRelease(service);
    }
    IOObjectRelease(iter);
    if (!target) { printf("ERROR: No vendor unit (0xA27/16) found\n"); return 1; }
    printf("Found vendor unit (Unit_Spec_ID=0xA27, SW=16)\n");

    // Create plugin interface
    IOCFPlugInInterface** plugIn = nullptr;
    SInt32 score = 0;
    kern_return_t kr = IOCreatePlugInInterfaceForService(
        target, kIOFireWireLibTypeID, kIOCFPlugInInterfaceID, &plugIn, &score);
    IOObjectRelease(target);
    if (kr != kIOReturnSuccess || !plugIn) {
        printf("ERROR: Plugin creation failed: 0x%x\n", kr);
        return 1;
    }

    // Query v9 device interface
    (*plugIn)->QueryInterface(plugIn,
        CFUUIDGetUUIDBytes(kIOFireWireDeviceInterfaceID_v9), (LPVOID*)&gDev);
    (*plugIn)->Release(plugIn);
    if (!gDev) { printf("ERROR: QueryInterface failed\n"); return 1; }

    // Open device
    IOReturn r = (*gDev)->Open(gDev);
    if (r != kIOReturnSuccess) {
        printf("ERROR: Open failed: 0x%x\n", r);
        (*gDev)->Release(gDev);
        return 1;
    }
    printf("Device opened successfully\n");

    // Get bus info
    (*gDev)->GetBusGeneration(gDev, &gGen);
    UInt16 localNode = 0;
    (*gDev)->GetLocalNodeIDWithGeneration(gDev, gGen, &localNode);
    (*gDev)->GetRemoteNodeID(gDev, gGen, &gNode);

    printf("\n--- Bus Info ---\n");
    printf("Bus generation:  %u\n", gGen);
    printf("Local node ID:   0x%04x\n", localNode);
    printf("Remote node ID:  0x%04x (camera)\n", gNode);

    // GetSpeedToNode
    IOFWSpeed speed = kFWSpeed100MBit;
    r = (*gDev)->GetSpeedToNode(gDev, gGen, &speed);
    const char* speedNames[] = {"S100", "S200", "S400", "S800"};
    if (r == kIOReturnSuccess) {
        printf("Speed to node:   %s (%d)\n",
               (speed <= 3) ? speedNames[speed] : "unknown", speed);
    } else {
        printf("GetSpeedToNode:  failed 0x%x\n", r);
    }

    // --- Section 1: IIDC CSR at 0xF0F00000-0xF0F00020 ---
    printf("\n--- IIDC CSR (0xF0F00000-0xF0F00020) ---\n");
    const struct { uint32_t offset; const char* name; } csrRegs[] = {
        {0xF0F00000, "INITIALIZE"},
        {0xF0F00004, "V_FORMAT_INQ (alt)"},
        {0xF0F00008, "V_MODE_INQ (alt)"},
        {0xF0F0000C, "V_RATE_INQ (alt)"},
        {0xF0F00010, "CAMERA_POWER"},
        {0xF0F00014, "reserved/14"},
        {0xF0F00018, "reserved/18"},
        {0xF0F0001C, "reserved/1C"},
        {0xF0F00020, "reserved/20"},
    };
    for (auto &reg : csrRegs) {
        IOReturn rr;
        uint32_t val = readReg(reg.offset, &rr);
        printf("  0x%08X  %-22s = 0x%08X%s\n", reg.offset, reg.name, val,
               rr != 0 ? "  (read failed)" : "");
    }

    // --- Section 2: IIDC Format/Mode/Rate Inquiry (0xF0F00100-0xF0F00140) ---
    printf("\n--- IIDC Format/Mode/Rate Inquiry (0xF0F00100-0xF0F00140) ---\n");
    for (uint32_t off = 0xF0F00100; off <= 0xF0F00140; off += 4) {
        IOReturn rr;
        uint32_t val = readReg(off, &rr);
        printf("  0x%08X = 0x%08X%s\n", off, val, rr != 0 ? "  (read failed)" : "");
    }

    // --- Section 3: IIDC Current Settings (0xF0F00600-0xF0F00640) ---
    printf("\n--- IIDC Current Settings (0xF0F00600-0xF0F00640) ---\n");
    const struct { uint32_t offset; const char* name; } settingsRegs[] = {
        {0xF0F00600, "ISO_CHANNEL"},
        {0xF0F00604, "ISO_SPEED"},
        {0xF0F00608, "CUR_V_FORMAT"},
        {0xF0F0060C, "CUR_V_MODE"},
        {0xF0F00610, "CUR_V_RATE"},
        {0xF0F00614, "ISO_EN"},
        {0xF0F00618, "reserved/618"},
        {0xF0F0061C, "reserved/61C"},
        {0xF0F00620, "MEMORY_SAVE_CH"},
        {0xF0F00624, "CUR_MEM_CH"},
        {0xF0F00628, "reserved/628"},
        {0xF0F0062C, "reserved/62C"},
        {0xF0F00630, "reserved/630"},
        {0xF0F00634, "reserved/634"},
        {0xF0F00638, "reserved/638"},
        {0xF0F0063C, "reserved/63C"},
        {0xF0F00640, "reserved/640"},
    };
    for (auto &reg : settingsRegs) {
        IOReturn rr;
        uint32_t val = readReg(reg.offset, &rr);
        printf("  0x%08X  %-18s = 0x%08X%s\n", reg.offset, reg.name, val,
               rr != 0 ? "  (read failed)" : "");
    }

    // Decode key settings
    {
        uint32_t isoCh = readReg(0xF0F00600);
        uint32_t isoSpd = readReg(0xF0F00604);
        uint32_t fmt = readReg(0xF0F00608);
        uint32_t mode = readReg(0xF0F0060C);
        uint32_t rate = readReg(0xF0F00610);
        uint32_t isoEn = readReg(0xF0F00614);

        printf("\n  Decoded:\n");
        printf("    ISO Channel:  %u (bits 31-28)\n", isoCh >> 28);
        printf("    ISO Speed:    %u => %s\n", isoSpd >> 29,
               (isoSpd >> 29) <= 3 ? speedNames[isoSpd >> 29] : "unknown");
        printf("    Format:       %u\n", fmt >> 29);
        printf("    Mode:         %u\n", mode >> 29);
        printf("    Rate:         %u\n", rate >> 29);
        printf("    ISO Enabled:  %s (0x%08X)\n",
               (isoEn & 0x80000000) ? "YES" : "NO", isoEn);
    }

    // --- Section 4: Camera ISO Registers (0xF0010600-0xF0010620) ---
    printf("\n--- Camera ISO Registers (0xF0010600-0xF0010620) ---\n");
    const struct { uint32_t offset; const char* name; } camIsoRegs[] = {
        {0xF0010600, "ISO_CHANNEL (cam)"},
        {0xF0010604, "ISO_SPEED (cam)"},
        {0xF0010608, "CUR_V_FORMAT (cam)"},
        {0xF001060C, "CUR_V_MODE (cam)"},
        {0xF0010610, "CUR_V_RATE (cam)"},
        {0xF0010614, "ISO_EN (cam)"},
        {0xF0010618, "cam/618"},
        {0xF001061C, "cam/61C"},
        {0xF0010620, "cam/620"},
    };
    for (auto &reg : camIsoRegs) {
        IOReturn rr;
        uint32_t val = readReg(reg.offset, &rr);
        printf("  0x%08X  %-22s = 0x%08X%s\n", reg.offset, reg.name, val,
               rr != 0 ? "  (read failed)" : "");
    }

    // Decode camera ISO settings
    {
        uint32_t camCh = readReg(0xF0010600);
        uint32_t camSpd = readReg(0xF0010604);
        if (camCh != 0xDEADDEAD) {
            printf("\n  Decoded (camera base 0xF0010000):\n");
            printf("    ISO Channel:  %u (bits 31-29: %u)\n", camCh >> 28, camCh >> 29);
            printf("    ISO Speed:    0x%08X => %s\n", camSpd,
                   (camSpd >> 29) <= 3 ? speedNames[camSpd >> 29] : "unknown");
        }
    }

    // --- Section 5: Bus Speed / MAX_REC (0xF0010000) ---
    printf("\n--- Bus Info Block (0xF0010000) ---\n");
    for (uint32_t off = 0xF0010000; off <= 0xF0010014; off += 4) {
        IOReturn rr;
        uint32_t val = readReg(off, &rr);
        printf("  0x%08X = 0x%08X%s\n", off, val, rr != 0 ? "  (read failed)" : "");
    }
    {
        uint32_t busInfo = readReg(0xF0010000);
        if (busInfo != 0xDEADDEAD) {
            uint32_t maxRec = (busInfo >> 12) & 0xF;
            uint32_t linkSpd = busInfo & 0x7;
            printf("\n  Decoded bus info:\n");
            printf("    max_rec:      %u (max async payload = %u bytes)\n",
                   maxRec, maxRec > 0 ? (1u << (maxRec + 1)) : 0);
            printf("    link_speed:   %u => %s\n", linkSpd,
                   linkSpd <= 3 ? speedNames[linkSpd] : "unknown");
        }
    }

    // --- Also read from config ROM base for comparison ---
    printf("\n--- Config ROM (0xF0000400+) ---\n");
    for (uint32_t off = 0xF0000400; off <= 0xF0000420; off += 4) {
        IOReturn rr;
        uint32_t val = readReg(off, &rr);
        printf("  0x%08X = 0x%08X%s\n", off, val, rr != 0 ? "  (read failed)" : "");
    }
    {
        uint32_t busInfoQ = readReg(0xF0000408);
        if (busInfoQ != 0xDEADDEAD) {
            uint32_t maxRec = (busInfoQ >> 12) & 0xF;
            uint32_t maxSpd = busInfoQ & 0x7;
            printf("\n  Config ROM bus_info_block:\n");
            printf("    max_rec:      %u (max async = %u bytes)\n",
                   maxRec, maxRec > 0 ? (1u << (maxRec + 1)) : 0);
            printf("    max_speed:    %u => %s\n", maxSpd,
                   maxSpd <= 3 ? speedNames[maxSpd] : "unknown");
        }
    }

    // --- IRM registers (on local node) ---
    printf("\n--- IRM Registers (local node 0x%04x) ---\n", localNode);
    FWAddress irmAddr;
    irmAddr.nodeID = localNode;
    irmAddr.addressHi = 0xFFFF;
    uint32_t irmVal = 0;
    const struct { uint32_t offset; const char* name; } irmRegs[] = {
        {0xF0000220, "BANDWIDTH_AVAILABLE"},
        {0xF0000224, "CHANNELS_AVAILABLE_HI"},
        {0xF0000228, "CHANNELS_AVAILABLE_LO"},
    };
    for (auto &reg : irmRegs) {
        irmAddr.addressLo = reg.offset;
        irmVal = 0;
        IOReturn rr = (*gDev)->ReadQuadlet(gDev, 0, &irmAddr, &irmVal, true, gGen);
        irmVal = ntohl(irmVal);
        printf("  0x%08X  %-26s = 0x%08X%s\n", reg.offset, reg.name, irmVal,
               rr != 0 ? "  (read failed)" : "");
    }

    // Cleanup
    (*gDev)->Close(gDev);
    (*gDev)->Release(gDev);
    printf("\nDone.\n");
    return 0;
}
