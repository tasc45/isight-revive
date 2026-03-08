#ifndef ISIGHTREVIVE_IIDCCAMERA_H
#define ISIGHTREVIVE_IIDCCAMERA_H

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/firewire/IOFireWireLib.h>
#include <IOKit/firewire/IOFireWireLibIsoch.h>
#include <IOKit/firewire/IOFireWireFamilyCommon.h>
#include <functional>
#include <cstdint>
#include <pthread.h>

// Callback for completed video frames
// data: raw YUV422 pixel data, length: byte count
using FrameCallback = std::function<void(const uint8_t* data, size_t length,
                                          uint32_t width, uint32_t height)>;

class IIDCCamera {
public:
    IIDCCamera();
    ~IIDCCamera();

    // Discovery — find the first IIDC camera on the FireWire bus
    bool discover();

    // Open the FireWire device and read config ROM
    bool open();

    // Read IIDC capability registers
    bool readCapabilities();

    // Configure video format/mode/rate
    bool configure(uint32_t format, uint32_t mode, uint32_t rate);

    // Set up isochronous receive and start streaming
    bool startStreaming(FrameCallback callback);

    // Stop streaming and tear down isoch
    void stopStreaming();

    // Close device
    void close();

    // Device info
    uint64_t getGUID() const { return mGUID; }
    bool isOpen() const { return mDeviceInterface != nullptr; }
    bool isStreaming() const { return mStreaming; }

    // Current configuration
    uint32_t getWidth() const { return mWidth; }
    uint32_t getHeight() const { return mHeight; }
    double getFrameRate() const { return mFrameRateHz; }

private:
    // FireWire register read/write via the command object
    bool readQuadlet(uint64_t address, uint32_t* value);
    bool writeQuadlet(uint64_t address, uint32_t value);

    // Parse config ROM to find IIDC command registers base
    bool parseConfigROM();

    // Isochronous receive support
    bool setupIsoch();
    void teardownIsoch();

    // Isoch callback — called on the isoch thread when packets arrive
    static void isochCallback(void* refcon, NuDCLRef dcl);

    // Isoch thread entry
    static void* isochThreadEntry(void* arg);
    void isochThreadRun();

    // Accumulate packet data into frame buffer
    void handleIsochPacket(const uint8_t* payload, size_t length, uint32_t sy);

    // IOKit objects
    io_service_t mService = 0;
    IOFireWireLibDeviceRef mDeviceInterface = nullptr;
    IOFireWireLibNuDCLPoolRef mDCLPool = nullptr;
    IOFireWireLibLocalIsochPortRef mLocalPort = nullptr;
    IOFireWireLibRemoteIsochPortRef mRemotePort = nullptr;
    IOFireWireLibIsochChannelRef mChannel = nullptr;

    // Device identity
    uint64_t mGUID = 0;

    // IIDC register base address (found from config ROM)
    uint64_t mCommandBase = 0;

    // Current video config
    uint32_t mFormat = 0;
    uint32_t mMode = 3;    // 640x480 YUV422
    uint32_t mRate = 4;    // 30fps
    uint32_t mWidth = 640;
    uint32_t mHeight = 480;
    double mFrameRateHz = 30.0;
    uint32_t mISOChannel = 0;

    // Streaming state
    bool mStreaming = false;
    FrameCallback mFrameCallback;
    pthread_t mIsochThread = nullptr;
    CFRunLoopRef mIsochRunLoop = nullptr;
    bool mIsochThreadRunning = false;

    // Frame assembly buffer
    uint8_t* mFrameBuffer = nullptr;
    size_t mFrameBufferSize = 0;
    size_t mFrameBufferOffset = 0;
    bool mFrameStarted = false;

    // NuDCL receive buffers
    static constexpr int kNumDCLBuffers = 64;
    static constexpr int kDCLBufferSize = 4096;  // Max isoch packet payload
    uint8_t* mDCLBuffers = nullptr;
    NuDCLRef mDCLs[kNumDCLBuffers] = {};
};

#endif // ISIGHTREVIVE_IIDCCAMERA_H
