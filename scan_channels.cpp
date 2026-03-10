// Scan all 64 FireWire isochronous channels to find where the camera is broadcasting
// Uses a vendor unit (not IIDC) to avoid exclusive access conflicts
#include <IOKit/IOKitLib.h>
#include <IOKit/IOCFPlugIn.h>
#include <IOKit/firewire/IOFireWireLib.h>
#include <IOKit/firewire/IOFireWireLibIsoch.h>
#include <CoreFoundation/CoreFoundation.h>
#include <arpa/inet.h>
#include <cstdio>
#include <unistd.h>
#include <atomic>

static IOFireWireLibDeviceRef gDev = nullptr;
static UInt16 gNode = 0, gLocalNode = 0;
static UInt32 gGen = 0;

struct ChanResult {
    std::atomic<int> pktCount{0};
    int chan;
};

static ChanResult gResults[16]; // scan channels 0-15

static void isochCB(void* refcon, NuDCLRef dcl) {
    ChanResult* cr = (ChanResult*)refcon;
    cr->pktCount++;
}

static bool refreshGen() {
    for (int i = 0; i < 20; i++) {
        if ((*gDev)->GetBusGeneration(gDev, &gGen) == 0 &&
            (*gDev)->GetRemoteNodeID(gDev, gGen, &gNode) == 0 &&
            (*gDev)->GetLocalNodeIDWithGeneration(gDev, gGen, &gLocalNode) == 0 &&
            gNode != 0) return true;
        usleep(200000);
    }
    return false;
}

int main() {
    printf("=== FireWire Channel Scanner ===\n");
    printf("Will listen on channels 0-15 to find camera broadcast\n\n");

    // Use vendor unit SW=16 (not IIDC) to avoid exclusive access issues
    CFMutableDictionaryRef matching = IOServiceMatching("IOFireWireUnit");
    io_iterator_t iter;
    IOServiceGetMatchingServices(kIOMainPortDefault, matching, &iter);
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
    if (!target) { printf("No vendor unit found\n"); return 1; }

    IOCFPlugInInterface** plugIn = nullptr; SInt32 score = 0;
    IOCreatePlugInInterfaceForService(target, kIOFireWireLibTypeID, kIOCFPlugInInterfaceID, &plugIn, &score);
    IOObjectRelease(target);
    (*plugIn)->QueryInterface(plugIn, CFUUIDGetUUIDBytes(kIOFireWireDeviceInterfaceID_v9), (LPVOID*)&gDev);
    (*plugIn)->Release(plugIn);
    IOReturn r = (*gDev)->Open(gDev);
    printf("Open: 0x%x\n", r);
    if (r != 0) { printf("Cannot open device\n"); return 1; }
    refreshGen();
    printf("Node=0x%04x Local=0x%04x Gen=%u\n\n", gNode, gLocalNode, gGen);

    // Read camera registers via this unit
    FWAddress addr; addr.nodeID = gNode; addr.addressHi = 0xFFFF;
    addr.addressLo = 0xF0010600;
    uint32_t val = 0;
    r = (*gDev)->ReadQuadlet(gDev, 0, &addr, &val, true, gGen);
    printf("ISO_CH register: 0x%08x (read: 0x%x)\n", ntohl(val), r);
    addr.addressLo = 0xF0010614;
    r = (*gDev)->ReadQuadlet(gDev, 0, &addr, &val, true, gGen);
    printf("ISO_EN register: 0x%08x (read: 0x%x)\n", ntohl(val), r);

    // Force-enable ISO from within this connection (no bus reset between write and scan)
    printf("Writing ISO_SPEED=S200, ISO_EN=enabled from within open device...\n");
    addr.addressLo = 0xF0010604;
    uint32_t speed = htonl(0x20000000);
    r = (*gDev)->WriteQuadlet(gDev, 0, &addr, speed, true, gGen);
    printf("  ISO_SPEED write: 0x%x\n", r);
    addr.addressLo = 0xF0010614;
    uint32_t en = htonl(0x80000000);
    r = (*gDev)->WriteQuadlet(gDev, 0, &addr, en, true, gGen);
    printf("  ISO_EN write: 0x%x\n", r);
    // Verify
    addr.addressLo = 0xF0010614;
    r = (*gDev)->ReadQuadlet(gDev, 0, &addr, &val, true, gGen);
    printf("  ISO_EN verify: 0x%08x (read: 0x%x)\n", ntohl(val), r);
    usleep(500000); // let camera start
    printf("\n");

    // Set up listeners on multiple channels
    // We'll scan one channel at a time for simplicity
    (*gDev)->AddIsochCallbackDispatcherToRunLoop(gDev, CFRunLoopGetMain());
    (*gDev)->AddCallbackDispatcherToRunLoop(gDev, CFRunLoopGetMain());

    for (int ch = 0; ch < 16; ch++) {
        gResults[ch].chan = ch;
        gResults[ch].pktCount = 0;

        const int kN = 32, kBuf = 4096;
        uint8_t* bufs = (uint8_t*)calloc(kN, kBuf);
        NuDCLRef dcls[32] = {};

        auto pool = (*gDev)->CreateNuDCLPool(gDev, kN, CFUUIDGetUUIDBytes(kIOFireWireNuDCLPoolInterfaceID));
        for (int i = 0; i < kN; i++) {
            IOVirtualRange range = { (IOVirtualAddress)(bufs + i * kBuf), (IOByteCount)kBuf };
            dcls[i] = (*pool)->AllocateReceivePacket(pool, nullptr, 4, 1, &range);
            (*pool)->SetDCLCallback(dcls[i], isochCB);
            (*pool)->SetDCLRefcon(dcls[i], &gResults[ch]);
        }
        for (int i = 0; i < kN; i++)
            (*pool)->SetDCLBranch(dcls[i], dcls[(i+1) % kN]);

        DCLCommand* prog = (*pool)->GetProgram(pool);
        IOVirtualRange bufRange = { (IOVirtualAddress)bufs, (IOByteCount)(kN * kBuf) };

        // Create local isoch port for this channel
        auto localPort = (*gDev)->CreateLocalIsochPortWithOptions(gDev, false, prog,
            0, 0, 0, nullptr, 0, &bufRange, 1, kFWIsochPortDefaultOptions,
            CFUUIDGetUUIDBytes(kIOFireWireLocalIsochPortInterfaceID));

        if (!localPort) {
            printf("Ch%2d: CreateLocalIsochPort FAILED\n", ch);
            (*pool)->Release(pool);
            free(bufs);
            continue;
        }

        // Create remote port (talker) that supports this specific channel
        struct ChData { int ch; };
        static int sCurCh;
        sCurCh = ch;

        auto remotePort = (*gDev)->CreateRemoteIsochPort(gDev, true,
            CFUUIDGetUUIDBytes(kIOFireWireRemoteIsochPortInterfaceID));

        auto getSup = [](IOFireWireLibIsochPortRef self, IOFWSpeed* maxSpeed, UInt64* chanSupported) -> IOReturn {
            *maxSpeed = kFWSpeed400MBit;
            *chanSupported = (1ULL << (63 - sCurCh));
            return kIOReturnSuccess;
        };
        auto allocP = [](IOFireWireLibIsochPortRef self, IOFWSpeed speed, UInt32 chan) -> IOReturn {
            return kIOReturnSuccess;
        };
        auto relP = [](IOFireWireLibIsochPortRef self) -> IOReturn { return kIOReturnSuccess; };
        auto startP = [](IOFireWireLibIsochPortRef self) -> IOReturn { return kIOReturnSuccess; };
        auto stopP = [](IOFireWireLibIsochPortRef self) -> IOReturn { return kIOReturnSuccess; };

        (*remotePort)->SetGetSupportedHandler(remotePort, getSup);
        (*remotePort)->SetAllocatePortHandler(remotePort, allocP);
        (*remotePort)->SetReleasePortHandler(remotePort, relP);
        (*remotePort)->SetStartHandler(remotePort, startP);
        (*remotePort)->SetStopHandler(remotePort, stopP);

        auto channel = (*gDev)->CreateIsochChannel(gDev, false, kBuf,
            kFWSpeed400MBit, CFUUIDGetUUIDBytes(kIOFireWireIsochChannelInterfaceID));
        (*channel)->SetTalker(channel, (IOFireWireLibIsochPortRef)remotePort);
        (*channel)->AddListener(channel, (IOFireWireLibIsochPortRef)localPort);

        r = (*channel)->AllocateChannel(channel);
        if (r != 0) {
            printf("Ch%2d: AllocateChannel FAILED 0x%x\n", ch, r);
            (*channel)->Release(channel);
            (*remotePort)->Release(remotePort);
            (*localPort)->Release(localPort);
            (*pool)->Release(pool);
            free(bufs);
            continue;
        }

        r = (*channel)->Start(channel);
        if (r != 0) {
            printf("Ch%2d: Start FAILED 0x%x\n", ch, r);
        }

        // Listen for 1 second
        for (int i = 0; i < 10; i++)
            CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.1, false);

        int pkts = gResults[ch].pktCount.load();

        // Check DMA buffers
        int nonZero = 0;
        for (int i = 0; i < kN; i++)
            for (int j = 0; j < kBuf; j++)
                if (bufs[i*kBuf+j]) { nonZero++; break; }

        if (pkts > 0 || nonZero > 0) {
            printf("Ch%2d: *** %d PACKETS, %d non-zero buffers! ***\n", ch, pkts, nonZero);
            if (nonZero > 0) {
                printf("  First non-zero buffer: ");
                for (int i = 0; i < kN; i++) {
                    bool has = false;
                    for (int j = 0; j < kBuf; j++) if (bufs[i*kBuf+j]) { has = true; break; }
                    if (has) {
                        for (int j = 0; j < 64; j++) printf("%02x ", bufs[i*kBuf+j]);
                        printf("\n");
                        break;
                    }
                }
            }
        } else {
            printf("Ch%2d: 0 packets, 0 buffers\n", ch);
        }

        // Cleanup this channel
        (*channel)->Stop(channel);
        (*channel)->ReleaseChannel(channel);
        (*channel)->Release(channel);
        (*remotePort)->Release(remotePort);
        (*localPort)->Release(localPort);
        (*pool)->Release(pool);
        free(bufs);
    }

    (*gDev)->RemoveIsochCallbackDispatcherFromRunLoop(gDev);
    (*gDev)->RemoveCallbackDispatcherFromRunLoop(gDev);
    (*gDev)->Close(gDev);
    (*gDev)->Release(gDev);

    printf("\n=== Summary ===\n");
    for (int ch = 0; ch < 16; ch++) {
        if (gResults[ch].pktCount > 0)
            printf("Ch%2d: %d packets\n", ch, gResults[ch].pktCount.load());
    }
    printf("Done.\n");
    return 0;
}
