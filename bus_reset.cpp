// Force a FireWire bus reset to reclaim leaked IRM bandwidth
#include <IOKit/IOKitLib.h>
#include <IOKit/IOCFPlugIn.h>
#include <IOKit/firewire/IOFireWireLib.h>
#include <CoreFoundation/CoreFoundation.h>
#include <cstdio>
#include <unistd.h>

int main() {
    printf("=== FireWire Bus Reset ===\n");

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
    if (!target) { printf("No iSight IIDC unit found\n"); return 1; }
    printf("Found iSight IIDC unit\n");

    IOCFPlugInInterface** plugIn = nullptr; SInt32 score = 0;
    IOCreatePlugInInterfaceForService(target, kIOFireWireLibTypeID, kIOCFPlugInInterfaceID, &plugIn, &score);
    IOObjectRelease(target);
    IOFireWireLibDeviceRef dev = nullptr;
    (*plugIn)->QueryInterface(plugIn, CFUUIDGetUUIDBytes(kIOFireWireDeviceInterfaceID_v9), (LPVOID*)&dev);
    (*plugIn)->Release(plugIn);

    IOReturn r = (*dev)->Open(dev);
    printf("Open: 0x%x\n", r);
    if (r != 0) {
        printf("Can't open exclusively, trying with session ref...\n");
        r = (*dev)->OpenWithSessionRef(dev, (IOFireWireSessionRef)0x1234);
        printf("OpenWithSessionRef: 0x%x\n", r);
    }

    // Force bus reset
    r = (*dev)->BusReset(dev);
    printf("BusReset: 0x%x\n", r);

    if (r == 0) {
        printf("Bus reset sent! IRM bandwidth should be reclaimed.\n");
        usleep(500000);
        UInt32 gen = 0; UInt16 node = 0;
        (*dev)->GetBusGeneration(dev, &gen);
        (*dev)->GetRemoteNodeID(dev, gen, &node);
        printf("Post-reset: gen=%u node=0x%04x\n", gen, node);
    }

    (*dev)->Close(dev);
    (*dev)->Release(dev);
    return 0;
}
