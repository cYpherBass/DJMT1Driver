//
// DJMT1Device.cpp
// Audio device + USB isochronous engine for the Pioneer DJM-T1.
//
// Hardware contract (reverse-engineered from Pioneer's legacy kext and
// verified live against the device):
//   - interface 0 (vendor-specific), alternate setting 1
//   - EP 0x01 OUT / EP 0x82 IN, isochronous asynchronous, 1024 B, 1 ms
//   - fixed 48 kHz, 6 channels per direction, 24-bit packed little-endian
//     => 864 bytes per USB frame (48 samples * 6 ch * 3 B)
//   - standard UAC1 SET_CUR SAMPLING_FREQ_CONTROL on both endpoints
//

#include <DriverKit/DriverKit.h>
#include <DriverKit/OSSharedPtr.h>
#include <DriverKit/IOBufferMemoryDescriptor.h>
#include <USBDriverKit/USBDriverKit.h>
#include <AudioDriverKit/AudioDriverKit.h>

#include "DJMT1Device.h"

#define LOG(fmt, ...) os_log(OS_LOG_DEFAULT, "DJMT1Device: " fmt, ##__VA_ARGS__)

namespace {
constexpr uint32_t kSampleRate      = 48000;
constexpr uint32_t kChannels        = 6;
constexpr uint32_t kBytesPerSample  = 3;    // 24-bit packed
constexpr uint32_t kBytesPerFrame   = kChannels * kBytesPerSample;  // 18
constexpr uint32_t kSamplesPerMs    = 48;
constexpr uint32_t kBytesPerMs      = kSamplesPerMs * kBytesPerFrame;  // 864
constexpr uint32_t kInPacketSize    = 1024; // wMaxPacketSize of EP 0x82
constexpr uint32_t kUSBFramesPerURB = 8;    // 8 ms per transaction
constexpr uint32_t kNumURBs         = 4;    // per direction, in flight
constexpr uint32_t kRingFrames      = kSamplesPerMs * 256;           // 12288 (256 ms)
constexpr uint32_t kRingBytes       = kRingFrames * kBytesPerFrame;
constexpr uint8_t  kEndpointOut     = 0x01;
constexpr uint8_t  kEndpointIn      = 0x82;
constexpr uint32_t kSafetyOffset    = 3 * kSamplesPerMs;

struct URB {
    OSSharedPtr<IOBufferMemoryDescriptor> data;
    OSSharedPtr<IOBufferMemoryDescriptor> frameList;
    uint8_t*                data_ptr  = nullptr;
    IOUSBIsochronousFrame*  frames    = nullptr;
    OSSharedPtr<OSAction>   completion;
};
}

struct DJMT1Device_IVars
{
    OSSharedPtr<IOUserAudioDriver>  driver;
    OSSharedPtr<IOUSBHostInterface> interface;
    OSSharedPtr<IOUSBHostPipe>      inPipe;
    OSSharedPtr<IOUSBHostPipe>      outPipe;
    OSSharedPtr<IOUserAudioStream>  inStream;
    OSSharedPtr<IOUserAudioStream>  outStream;
    OSSharedPtr<IOBufferMemoryDescriptor> inRing;
    OSSharedPtr<IOBufferMemoryDescriptor> outRing;
    uint8_t* inRingPtr  = nullptr;
    uint8_t* outRingPtr = nullptr;

    URB inURBs[kNumURBs];
    URB outURBs[kNumURBs];

    bool     ioRunning = false;
    uint64_t inSampleCount  = 0;   // absolute captured sample frames
    uint64_t outSampleCount = 0;   // absolute played sample frames
    uint64_t nextZeroSample = 0;   // next zero-timestamp boundary (sample time)
    uint64_t nextInFrameNumber  = 0;
    uint64_t nextOutFrameNumber = 0;
};

static bool AllocURB(URB& urb, uint32_t dataBytes, OSAction* action)
{
    IOBufferMemoryDescriptor* md = nullptr;
    if (IOBufferMemoryDescriptor::Create(kIOMemoryDirectionInOut, dataBytes, 0, &md)
        != kIOReturnSuccess) {
        return false;
    }
    urb.data = OSSharedPtr(md, OSNoRetain);

    IOBufferMemoryDescriptor* fl = nullptr;
    if (IOBufferMemoryDescriptor::Create(kIOMemoryDirectionInOut,
                                         kUSBFramesPerURB * sizeof(IOUSBIsochronousFrame),
                                         0, &fl) != kIOReturnSuccess) {
        return false;
    }
    urb.frameList = OSSharedPtr(fl, OSNoRetain);

    IOAddressSegment seg{};
    if (urb.data->GetAddressRange(&seg) != kIOReturnSuccess) {
        return false;
    }
    urb.data_ptr = reinterpret_cast<uint8_t*>(seg.address);

    if (urb.frameList->GetAddressRange(&seg) != kIOReturnSuccess) {
        return false;
    }
    urb.frames = reinterpret_cast<IOUSBIsochronousFrame*>(seg.address);

    urb.completion = OSSharedPtr(action, OSRetain);
    return true;
}

static void PrimeFrameList(URB& urb, uint32_t requestCount)
{
    for (uint32_t i = 0; i < kUSBFramesPerURB; i++) {
        urb.frames[i].status        = kIOReturnInvalid;
        urb.frames[i].requestCount  = requestCount;
        urb.frames[i].completeCount = 0;
        urb.frames[i].reserved      = 0;
        urb.frames[i].timeStamp     = 0;
    }
}

bool DJMT1Device::initDevice(IOUserAudioDriver* in_driver,
                             IOUSBHostInterface* in_interface,
                             OSString* in_device_uid,
                             OSString* in_model_uid,
                             OSString* in_manufacturer_uid)
{
    if (!init(in_driver, false, in_device_uid, in_model_uid, in_manufacturer_uid,
              kRingFrames)) {
        return false;
    }

    ivars->driver = OSSharedPtr(in_driver, OSRetain);
    ivars->interface = OSSharedPtr(in_interface, OSRetain);

    SetZeroTimeStampPeriod(kRingFrames);

    double rate = kSampleRate;
    SetAvailableSampleRates(&rate, 1);
    SetSampleRate(rate);

    // Shared IO ring buffers (mapped by coreaudiod).
    IOBufferMemoryDescriptor* md = nullptr;
    if (IOBufferMemoryDescriptor::Create(kIOMemoryDirectionInOut, kRingBytes, 0, &md)
        != kIOReturnSuccess) {
        return false;
    }
    ivars->inRing = OSSharedPtr(md, OSNoRetain);
    md = nullptr;
    if (IOBufferMemoryDescriptor::Create(kIOMemoryDirectionInOut, kRingBytes, 0, &md)
        != kIOReturnSuccess) {
        return false;
    }
    ivars->outRing = OSSharedPtr(md, OSNoRetain);

    IOAddressSegment seg{};
    if (ivars->inRing->GetAddressRange(&seg) != kIOReturnSuccess) {
        return false;
    }
    ivars->inRingPtr = reinterpret_cast<uint8_t*>(seg.address);
    if (ivars->outRing->GetAddressRange(&seg) != kIOReturnSuccess) {
        return false;
    }
    ivars->outRingPtr = reinterpret_cast<uint8_t*>(seg.address);

    // Streams: 6 in / 6 out, 24-bit packed LE, 48 kHz.
    IOUserAudioStreamBasicDescription format{};
    format.mSampleRate       = kSampleRate;
    format.mFormatID         = IOUserAudioFormatID::LinearPCM;
    format.mFormatFlags      = static_cast<IOUserAudioFormatFlags>(
        static_cast<uint32_t>(IOUserAudioFormatFlags::FormatFlagIsSignedInteger) |
        static_cast<uint32_t>(IOUserAudioFormatFlags::FormatFlagIsPacked));
    format.mBytesPerPacket   = kBytesPerFrame;
    format.mFramesPerPacket  = 1;
    format.mBytesPerFrame    = kBytesPerFrame;
    format.mChannelsPerFrame = kChannels;
    format.mBitsPerChannel   = 24;

    ivars->inStream = IOUserAudioStream::Create(in_driver,
                                                IOUserAudioStreamDirection::Input,
                                                ivars->inRing.get());
    ivars->outStream = IOUserAudioStream::Create(in_driver,
                                                 IOUserAudioStreamDirection::Output,
                                                 ivars->outRing.get());
    if (!ivars->inStream || !ivars->outStream) {
        return false;
    }
    ivars->inStream->SetAvailableStreamFormats(&format, 1);
    ivars->inStream->SetCurrentStreamFormat(&format);
    ivars->outStream->SetAvailableStreamFormats(&format, 1);
    ivars->outStream->SetCurrentStreamFormat(&format);

    auto inName = OSSharedPtr(OSString::withCString("DJM-T1 Input"), OSNoRetain);
    auto outName = OSSharedPtr(OSString::withCString("DJM-T1 Output"), OSNoRetain);
    ivars->inStream->SetName(inName.get());
    ivars->outStream->SetName(outName.get());

    if (AddStream(ivars->inStream.get()) != kIOReturnSuccess ||
        AddStream(ivars->outStream.get()) != kIOReturnSuccess) {
        return false;
    }

    SetInputSafetyOffset(kSafetyOffset);
    SetOutputSafetyOffset(kSafetyOffset);
    SetInputLatency(kSamplesPerMs);
    SetOutputLatency(kSamplesPerMs);

    // Per-direction transfer resources.
    for (uint32_t i = 0; i < kNumURBs; i++) {
        OSAction* action = nullptr;
        if (CreateActionHandleIsochInComplete(sizeof(uint32_t), &action)
            != kIOReturnSuccess) {
            return false;
        }
        *reinterpret_cast<uint32_t*>(action->GetReference()) = i;
        bool ok = AllocURB(ivars->inURBs[i], kUSBFramesPerURB * kInPacketSize, action);
        action->release();
        if (!ok) {
            return false;
        }

        action = nullptr;
        if (CreateActionHandleIsochOutComplete(sizeof(uint32_t), &action)
            != kIOReturnSuccess) {
            return false;
        }
        *reinterpret_cast<uint32_t*>(action->GetReference()) = i;
        ok = AllocURB(ivars->outURBs[i], kUSBFramesPerURB * kBytesPerMs, action);
        action->release();
        if (!ok) {
            return false;
        }
    }

    return true;
}

void DJMT1Device::free()
{
    IOSafeDeleteNULL(ivars, DJMT1Device_IVars, 1);
    super::free();
}

kern_return_t DJMT1Device::StartIO(IOUserAudioStartStopFlags in_flags)
{
    kern_return_t ret = IOUserAudioDevice::StartIO(in_flags);
    if (ret != kIOReturnSuccess) {
        return ret;
    }

    IOUSBHostInterface* intf = ivars->interface.get();

    ret = intf->SelectAlternateSetting(1);
    if (ret != kIOReturnSuccess) {
        LOG("SelectAlternateSetting(1) failed: 0x%x", ret);
        goto fail;
    }

    // UAC1 SET_CUR SAMPLING_FREQ_CONTROL, 48000 Hz, on both endpoints.
    {
        IOBufferMemoryDescriptor* freqMD = nullptr;
        if (IOBufferMemoryDescriptor::Create(kIOMemoryDirectionInOut, 3, 0, &freqMD)
            != kIOReturnSuccess) {
            ret = kIOReturnNoMemory;
            goto fail;
        }
        auto freq = OSSharedPtr(freqMD, OSNoRetain);
        IOAddressSegment seg{};
        freq->GetAddressRange(&seg);
        uint8_t* p = reinterpret_cast<uint8_t*>(seg.address);
        p[0] = 0x80; p[1] = 0xBB; p[2] = 0x00;  // 48000 little-endian

        const uint8_t endpoints[2] = { kEndpointOut, kEndpointIn };
        for (uint8_t ep : endpoints) {
            uint16_t transferred = 0;
            ret = intf->DeviceRequest(0x22 /* class, endpoint, host-to-device */,
                                      0x01 /* SET_CUR */,
                                      0x0100 /* SAMPLING_FREQ_CONTROL << 8 */,
                                      ep, 3, freq.get(), &transferred, 1000);
            if (ret != kIOReturnSuccess) {
                LOG("SET_CUR on ep 0x%x failed: 0x%x", ep, ret);
                goto fail;
            }
        }
    }

    {
        IOUSBHostPipe* pipe = nullptr;
        ret = intf->CopyPipe(kEndpointIn, &pipe);
        if (ret != kIOReturnSuccess) {
            LOG("CopyPipe(in) failed: 0x%x", ret);
            goto fail;
        }
        ivars->inPipe = OSSharedPtr(pipe, OSNoRetain);
        pipe = nullptr;
        ret = intf->CopyPipe(kEndpointOut, &pipe);
        if (ret != kIOReturnSuccess) {
            LOG("CopyPipe(out) failed: 0x%x", ret);
            goto fail;
        }
        ivars->outPipe = OSSharedPtr(pipe, OSNoRetain);
    }

    ivars->inSampleCount = 0;
    ivars->outSampleCount = 0;
    ivars->nextZeroSample = kRingFrames;
    memset(ivars->outRingPtr, 0, kRingBytes);

    UpdateCurrentZeroTimestamp(0, mach_absolute_time());

    {
        uint64_t frameNumber = 0;
        uint64_t frameTime = 0;
        intf->GetFrameNumber(&frameNumber, &frameTime);
        // Leave a small scheduling lead before the first transfer.
        ivars->nextInFrameNumber = frameNumber + 4;
        ivars->nextOutFrameNumber = frameNumber + 4;
    }

    ivars->ioRunning = true;

    for (uint32_t i = 0; i < kNumURBs; i++) {
        URB& in = ivars->inURBs[i];
        PrimeFrameList(in, kInPacketSize);
        ret = ivars->inPipe->IsochIO(in.data.get(), in.frameList.get(),
                                     ivars->nextInFrameNumber, in.completion.get());
        if (ret != kIOReturnSuccess) {
            LOG("initial IsochIO(in %u) failed: 0x%x", i, ret);
            goto fail;
        }
        ivars->nextInFrameNumber += kUSBFramesPerURB;

        URB& out = ivars->outURBs[i];
        PrimeFrameList(out, kBytesPerMs);
        memset(out.data_ptr, 0, kUSBFramesPerURB * kBytesPerMs);
        ivars->outSampleCount += kUSBFramesPerURB * kSamplesPerMs;
        ret = ivars->outPipe->IsochIO(out.data.get(), out.frameList.get(),
                                      ivars->nextOutFrameNumber, out.completion.get());
        if (ret != kIOReturnSuccess) {
            LOG("initial IsochIO(out %u) failed: 0x%x", i, ret);
            goto fail;
        }
        ivars->nextOutFrameNumber += kUSBFramesPerURB;
    }

    LOG("IO started");
    return kIOReturnSuccess;

fail:
    ivars->ioRunning = false;
    if (ivars->inPipe) {
        ivars->inPipe->Abort(kIOUSBAbortSynchronous, kIOReturnAborted, nullptr);
        ivars->inPipe.reset();
    }
    if (ivars->outPipe) {
        ivars->outPipe->Abort(kIOUSBAbortSynchronous, kIOReturnAborted, nullptr);
        ivars->outPipe.reset();
    }
    intf->SelectAlternateSetting(0);
    IOUserAudioDevice::StopIO(in_flags);
    return ret;
}

kern_return_t DJMT1Device::StopIO(IOUserAudioStartStopFlags in_flags)
{
    ivars->ioRunning = false;
    if (ivars->inPipe) {
        ivars->inPipe->Abort(kIOUSBAbortSynchronous, kIOReturnAborted, nullptr);
        ivars->inPipe.reset();
    }
    if (ivars->outPipe) {
        ivars->outPipe->Abort(kIOUSBAbortSynchronous, kIOReturnAborted, nullptr);
        ivars->outPipe.reset();
    }
    ivars->interface->SelectAlternateSetting(0);
    LOG("IO stopped");
    return IOUserAudioDevice::StopIO(in_flags);
}

void DJMT1Device::HandleIsochInComplete_Impl(OSAction* action, IOReturn status)
{
    if (!ivars->ioRunning || status == kIOReturnAborted) {
        return;
    }
    uint32_t slot = *reinterpret_cast<uint32_t*>(action->GetReference());
    URB& urb = ivars->inURBs[slot];

    for (uint32_t f = 0; f < kUSBFramesPerURB; f++) {
        IOUSBIsochronousFrame& frame = urb.frames[f];
        uint32_t bytes = (frame.status == kIOReturnSuccess) ? frame.completeCount : 0;
        uint32_t samples = bytes / kBytesPerFrame;
        if (samples > 0) {
            uint64_t pos = ivars->inSampleCount % kRingFrames;
            uint32_t untilWrap = kRingFrames - static_cast<uint32_t>(pos);
            uint8_t* src = urb.data_ptr + f * kInPacketSize;
            if (samples <= untilWrap) {
                memcpy(ivars->inRingPtr + pos * kBytesPerFrame, src,
                       samples * kBytesPerFrame);
            } else {
                memcpy(ivars->inRingPtr + pos * kBytesPerFrame, src,
                       untilWrap * kBytesPerFrame);
                memcpy(ivars->inRingPtr, src + untilWrap * kBytesPerFrame,
                       (samples - untilWrap) * kBytesPerFrame);
            }
            ivars->inSampleCount += samples;
        }
        // The input stream is the clock master: publish a zero timestamp
        // whenever the ring wraps, anchored to this frame's completion time.
        if (ivars->inSampleCount >= ivars->nextZeroSample && frame.timeStamp != 0) {
            UpdateCurrentZeroTimestamp(ivars->nextZeroSample, frame.timeStamp);
            ivars->nextZeroSample += kRingFrames;
        }
    }

    PrimeFrameList(urb, kInPacketSize);
    kern_return_t ret = ivars->inPipe->IsochIO(urb.data.get(), urb.frameList.get(),
                                               ivars->nextInFrameNumber,
                                               urb.completion.get());
    if (ret != kIOReturnSuccess) {
        LOG("IsochIO(in resubmit) failed: 0x%x", ret);
    } else {
        ivars->nextInFrameNumber += kUSBFramesPerURB;
    }
}

void DJMT1Device::HandleIsochOutComplete_Impl(OSAction* action, IOReturn status)
{
    if (!ivars->ioRunning || status == kIOReturnAborted) {
        return;
    }
    uint32_t slot = *reinterpret_cast<uint32_t*>(action->GetReference());
    URB& urb = ivars->outURBs[slot];

    // Refill this URB from the output ring at the current play position.
    for (uint32_t f = 0; f < kUSBFramesPerURB; f++) {
        uint64_t pos = ivars->outSampleCount % kRingFrames;
        uint32_t untilWrap = kRingFrames - static_cast<uint32_t>(pos);
        uint8_t* dst = urb.data_ptr + f * kBytesPerMs;
        if (kSamplesPerMs <= untilWrap) {
            memcpy(dst, ivars->outRingPtr + pos * kBytesPerFrame, kBytesPerMs);
        } else {
            memcpy(dst, ivars->outRingPtr + pos * kBytesPerFrame,
                   untilWrap * kBytesPerFrame);
            memcpy(dst + untilWrap * kBytesPerFrame, ivars->outRingPtr,
                   (kSamplesPerMs - untilWrap) * kBytesPerFrame);
        }
        ivars->outSampleCount += kSamplesPerMs;
    }

    PrimeFrameList(urb, kBytesPerMs);
    kern_return_t ret = ivars->outPipe->IsochIO(urb.data.get(), urb.frameList.get(),
                                                ivars->nextOutFrameNumber,
                                                urb.completion.get());
    if (ret != kIOReturnSuccess) {
        LOG("IsochIO(out resubmit) failed: 0x%x", ret);
    } else {
        ivars->nextOutFrameNumber += kUSBFramesPerURB;
    }
}
