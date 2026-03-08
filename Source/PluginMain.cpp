#include "PluginInterface.h"
#include "Logging.h"

#include <CoreFoundation/CoreFoundation.h>
#include <CoreMediaIO/CMIOHardwarePlugIn.h>

// ============================================================================
// CFPlugIn Factory Function
//
// This is the entry point called by CoreMediaIO when it loads our DAL plugin.
// The function name must match CFPlugInFactories in Info.plist.
// ============================================================================

extern "C" {

void* iSightRevivePluginFactory(CFAllocatorRef allocator, CFUUIDRef typeUUID) {
    // Verify we're being asked for the right type
    if (!CFEqual(typeUUID, kCMIOHardwarePlugInTypeID)) {
        ISR_LOG_ERROR("Factory called with wrong type UUID");
        return nullptr;
    }

    ISR_LOG("iSightRevive factory called — creating plugin interface");

    // Return our singleton plugin interface
    // CMIO expects a CMIOHardwarePlugInInterface** (pointer to pointer to vtable)
    return PluginInterface::instance().getInterface();
}

} // extern "C"
