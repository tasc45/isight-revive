#include "FrameAssembler.h"
#include "Logging.h"

#include <CoreMedia/CoreMedia.h>
#include <CoreVideo/CoreVideo.h>
#include <mach/mach_time.h>
#include <cstring>

FrameAssembler::FrameAssembler() {
    mBaseTime = CMClockGetTime(CMClockGetHostTimeClock());
}

FrameAssembler::~FrameAssembler() {
    shutdown();
}

bool FrameAssembler::initialize(uint32_t width, uint32_t height, double frameRate) {
    mWidth = width;
    mHeight = height;
    mFrameRate = frameRate;
    mFrameCount = 0;
    mBaseTime = CMClockGetTime(CMClockGetHostTimeClock());

    // Create pixel buffer pool for efficient CVPixelBuffer reuse
    NSDictionary* poolAttrs = @{
        (NSString*)kCVPixelBufferWidthKey: @(width),
        (NSString*)kCVPixelBufferHeightKey: @(height),
        (NSString*)kCVPixelBufferPixelFormatTypeKey: @(kCVPixelFormatType_422YpCbCr8),
        (NSString*)kCVPixelBufferIOSurfacePropertiesKey: @{},
    };

    CVReturn cvr = CVPixelBufferPoolCreate(kCFAllocatorDefault, nullptr,
                                            (__bridge CFDictionaryRef)poolAttrs,
                                            &mPixelBufferPool);
    if (cvr != kCVReturnSuccess) {
        ISR_LOG_ERROR("Failed to create pixel buffer pool: %d", cvr);
        return false;
    }

    // Create video format description (2vuy = kCVPixelFormatType_422YpCbCr8)
    OSStatus status = CMVideoFormatDescriptionCreate(
        kCFAllocatorDefault,
        kCVPixelFormatType_422YpCbCr8,
        (int32_t)width,
        (int32_t)height,
        nullptr,  // extensions
        &mFormatDesc
    );

    if (status != noErr) {
        ISR_LOG_ERROR("Failed to create format description: %d", (int)status);
        return false;
    }

    ISR_LOG("FrameAssembler initialized: %ux%u @ %.1ffps", width, height, frameRate);
    return true;
}

void FrameAssembler::deliverFrame(const uint8_t* data, size_t length,
                                   uint32_t width, uint32_t height) {
    if (!mPixelBufferPool || !mFormatDesc || !mCallback) return;

    size_t expectedSize = (size_t)width * height * 2;
    if (length < expectedSize) {
        ISR_LOG_DEBUG("Frame too small: %zu < %zu", length, expectedSize);
        return;
    }

    // Get a pixel buffer from the pool
    CVPixelBufferRef pixelBuffer = nullptr;
    CVReturn cvr = CVPixelBufferPoolCreatePixelBuffer(kCFAllocatorDefault,
                                                       mPixelBufferPool,
                                                       &pixelBuffer);
    if (cvr != kCVReturnSuccess || !pixelBuffer) {
        ISR_LOG_ERROR("Failed to get pixel buffer from pool: %d", cvr);
        return;
    }

    // Lock and copy YUV data
    cvr = CVPixelBufferLockBaseAddress(pixelBuffer, 0);
    if (cvr != kCVReturnSuccess) {
        CVPixelBufferRelease(pixelBuffer);
        return;
    }

    void* baseAddr = CVPixelBufferGetBaseAddress(pixelBuffer);
    size_t bytesPerRow = CVPixelBufferGetBytesPerRow(pixelBuffer);
    size_t srcBytesPerRow = (size_t)width * 2;

    if (bytesPerRow == srcBytesPerRow) {
        // Stride matches — single copy
        memcpy(baseAddr, data, expectedSize);
    } else {
        // Copy row by row (handle stride mismatch)
        for (uint32_t row = 0; row < height; row++) {
            memcpy((uint8_t*)baseAddr + row * bytesPerRow,
                   data + row * srcBytesPerRow,
                   srcBytesPerRow);
        }
    }

    CVPixelBufferUnlockBaseAddress(pixelBuffer, 0);

    // Create timing info
    CMTime pts = CMTimeAdd(mBaseTime,
                           CMTimeMake((int64_t)mFrameCount * 1000, (int32_t)(mFrameRate * 1000)));
    CMTime duration = CMTimeMake(1000, (int32_t)(mFrameRate * 1000));

    CMSampleTimingInfo timing;
    timing.presentationTimeStamp = pts;
    timing.duration = duration;
    timing.decodeTimeStamp = kCMTimeInvalid;

    // Create CMSampleBuffer wrapping the pixel buffer
    CMSampleBufferRef sampleBuffer = nullptr;
    OSStatus status = CMSampleBufferCreateForImageBuffer(
        kCFAllocatorDefault,
        pixelBuffer,
        true,       // dataReady
        nullptr,    // makeDataReadyCallback
        nullptr,    // makeDataReadyRefcon
        mFormatDesc,
        &timing,
        &sampleBuffer
    );

    CVPixelBufferRelease(pixelBuffer);

    if (status != noErr || !sampleBuffer) {
        ISR_LOG_ERROR("Failed to create CMSampleBuffer: %d", (int)status);
        return;
    }

    mFrameCount++;

    // Deliver to CMIO stream
    mCallback(sampleBuffer);

    // The callback is expected to retain the sample buffer if needed
    CFRelease(sampleBuffer);
}

void FrameAssembler::shutdown() {
    if (mFormatDesc) {
        CFRelease(mFormatDesc);
        mFormatDesc = nullptr;
    }
    if (mPixelBufferPool) {
        CVPixelBufferPoolRelease(mPixelBufferPool);
        mPixelBufferPool = nullptr;
    }
    mCallback = nullptr;
}
