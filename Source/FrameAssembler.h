#ifndef ISIGHTREVIVE_FRAMEASSEMBLER_H
#define ISIGHTREVIVE_FRAMEASSEMBLER_H

#include <CoreMedia/CoreMedia.h>
#include <CoreVideo/CoreVideo.h>
#include <cstdint>
#include <functional>

// Callback for assembled CMSampleBuffers ready for CMIO delivery
using SampleBufferCallback = std::function<void(CMSampleBufferRef)>;

class FrameAssembler {
public:
    FrameAssembler();
    ~FrameAssembler();

    // Initialize with frame dimensions
    bool initialize(uint32_t width, uint32_t height, double frameRate);

    // Called by IIDCCamera when a complete frame of raw YUV422 data is ready
    void deliverFrame(const uint8_t* data, size_t length,
                      uint32_t width, uint32_t height);

    // Set the callback for assembled sample buffers
    void setCallback(SampleBufferCallback callback) { mCallback = callback; }

    // Cleanup
    void shutdown();

private:
    CVPixelBufferPoolRef mPixelBufferPool = nullptr;
    CMVideoFormatDescriptionRef mFormatDesc = nullptr;
    SampleBufferCallback mCallback;

    uint32_t mWidth = 0;
    uint32_t mHeight = 0;
    double mFrameRate = 30.0;
    uint64_t mFrameCount = 0;
    CMTime mBaseTime;
};

#endif // ISIGHTREVIVE_FRAMEASSEMBLER_H
