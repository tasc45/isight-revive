// Set camera ISO speed register
#include <IOKit/IOKitLib.h>
#include <IOKit/IOCFPlugIn.h>
#include <IOKit/firewire/IOFireWireLib.h>
#include <CoreFoundation/CoreFoundation.h>
#include <arpa/inet.h>
#include <cstdio>
#include <cstdlib>
#include <unistd.h>

int main(int argc, char** argv) {
    uint32_t targetSpeed = 0x40000000; // default S400
    if (argc > 1) {
        if (argv[1][0] == '2') targetSpeed = 0x20000000; // S200
        else if (argv[1][0] == '4') targetSpeed = 0x40000000; // S400
    }
    printf("=== Set iSight ISO Speed to %s ===\n", targetSpeed == 0x40000000 ? "S400" : "S200");

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
    if (!target) { printf("No vendor unit\n"); return 1; }

    IOCFPlugInInterface** plugIn = nullptr; SInt32 score = 0;
    IOCreatePlugInInterfaceForService(target, kIOFireWireLibTypeID, kIOCFPlugInInterfaceID, &plugIn, &score);
    IOObjectRelease(target);
    IOFireWireLibDeviceRef dev = nullptr;
    (*plugIn)->QueryInterface(plugIn, CFUUIDGetUUIDBytes(kIOFireWireDeviceInterfaceID_v9), (LPVOID*)&dev);
    (*plugIn)->Release(plugIn);
    IOReturn r = (*dev)->Open(dev);
    if (r != 0) { printf("Can't open\n"); return 1; }

    UInt32 gen = 0; UInt16 node = 0;
    (*dev)->GetBusGeneration(dev, &gen);
    (*dev)->GetRemoteNodeID(dev, gen, &node);

    FWAddress addr; addr.nodeID = node; addr.addressHi = 0xFFFF;
    addr.addressLo = 0xF0010604;
    uint32_t newSpeed = htonl(targetSpeed);
    r = (*dev)->WriteQuadlet(dev, 0, &addr, newSpeed, true, gen);
    printf("Write ISO_SPEED=0x%08x: 0x%x\n", targetSpeed, r);

    uint32_t val = 0;
    r = (*dev)->ReadQuadlet(dev, 0, &addr, &val, true, gen);
    printf("Verify ISO_SPEED: 0x%08x\n", ntohl(val));

    // Also enable ISO (turns on LED and starts camera transmitting)
    addr.addressLo = 0xF0010614;
    r = (*dev)->WriteQuadlet(dev, 0, &addr, htonl(0x80000000), true, gen);
    printf("Write ISO_EN=0x80000000: 0x%x\n", r);
    usleep(200000);
    r = (*dev)->ReadQuadlet(dev, 0, &addr, &val, true, gen);
    printf("Verify ISO_EN: 0x%08x → %s\n", ntohl(val),
           (ntohl(val) & 0x80000000) ? "LED ON" : "LED off");

    (*dev)->Close(dev);
    (*dev)->Release(dev);
    return 0;
}
