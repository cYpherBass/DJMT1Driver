//
// SL2Device.cpp
// Audio device + USB isochronous engine for the Rane SL2.
//
// Hardware contract (documented by the Linux ALSA quirk in
// github.com/Reinharderino/rane-sl2-linux; to be re-verified on macOS with
// tools/sl2probe.m):
//   - interface 1 alt 1: EP 0x06 OUT, interface 2 alt 1: EP 0x82 IN
//     (implicit feedback), isochronous asynchronous, 112 B, bInterval 1
//     => one transaction per 125 us high-speed microframe
//   - fixed 44.1 kHz, 4 channels per direction, 24-bit in 4-byte subslots
//     (little-endian, S32 containers) => 16 B per sample frame,
//     5 or 6 sample frames per microframe (44100/8000 = 5.5125)
//   - NEVER send sample-rate requests: the device stalls both UAC2_CS_CUR
//     and UAC2_CS_RANGE; there is nothing to set, the rate is fixed
//   - interface 3 is HID and belongs to the system driver
//

#include <DriverKit/DriverKit.h>
#include <DriverKit/OSSharedPtr.h>
#include <DriverKit/IOBufferMemoryDescriptor.h>
#include <USBDriverKit/USBDriverKit.h>
#include <AudioDriverKit/AudioDriverKit.h>

#include "SL2Device.h"

#define LOG(fmt, ...) os_log(OS_LOG_DEFAULT, "SL2Device: " fmt, ##__VA_ARGS__)

namespace {
constexpr uint32_t kSampleRate       = 44100;
constexpr uint32_t kChannels         = 4;
constexpr uint32_t kBytesPerSample   = 4;    // 24-bit in a 4-byte subslot
constexpr uint32_t kBytesPerFrame    = kChannels * kBytesPerSample;  // 16
constexpr uint32_t kMicroframesPerSec = 8000; // high speed, bInterval 1
constexpr uint32_t kMaxPacketBytes   = 112;  // wMaxPacketSize (7 frames * 16 B)
constexpr uint32_t kMicroframesPerURB = 64;  // 8 ms per transaction
constexpr uint32_t kNumURBs          = 4;    // per direction, in flight
constexpr uint32_t kRingFrames       = 11025; // 250 ms
constexpr uint32_t kRingBytes        = kRingFrames * kBytesPerFrame;
constexpr uint8_t  kEndpointOut      = 0x06; // on interface 1
constexpr uint8_t  kEndpointIn       = 0x82; // on interface 2
constexpr uint32_t kSafetyOffset     = 132;  // 3 ms
// Nudge the output packet cadence when it drifts more than this many
// sample frames away from the input (implicit feedback pacing).
constexpr int64_t  kFeedbackSlack    = 64;

struct URB {
    OSSharedPtr<IOBufferMemoryDescriptor> data;
    OSSharedPtr<IOBufferMemoryDescriptor> frameList;
    uint8_t*                data_ptr  = nullptr;
    IOUSBIsochronousFrame*  frames    = nullptr;
    OSSharedPtr<OSAction>   completion;
};
}

struct SL2Device_IVars
{
    OSSharedPtr<IOUserAudioDriver>  driver;
    OSSharedPtr<IOUSBHostInterface> outInterface;   // interface 1
    OSSharedPtr<IOUSBHostInterface> inInterface;    // interface 2
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
    uint64_t nextZeroSample = 0;   // next zero-timestamp boundary
    uint64_t nextInFrameNumber  = 0;
    uint64_t nextOutFrameNumber = 0;
    uint32_t outAccumulator = 0;   // fractional sample accumulator (mod 8000)
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
                                         kMicroframesPerURB * sizeof(IOUSBIsochronousFrame),
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

bool SL2Device::init(IOUserAudioDriver* in_driver,
                     bool in_supports_prewarming,
                     OSString* in_device_uid,
                     OSString* in_model_uid,
                     OSString* in_manufacturer_uid,
                     uint32_t in_zero_timestamp_period)
{
    if (!super::init(in_driver, in_supports_prewarming, in_device_uid, in_model_uid,
                      in_manufacturer_uid, in_zero_timestamp_period)) {
        return false;
    }
    ivars = IONewZero(SL2Device_IVars, 1);
    return ivars != nullptr;
}

bool SL2Device::initDevice(IOUserAudioDriver* in_driver,
                           IOUSBHostInterface* in_out_interface,
                           IOUSBHostInterface* in_in_interface,
                           OSString* in_device_uid,
                           OSString* in_model_uid,
                           OSString* in_manufacturer_uid)
{
    if (!init(in_driver, false, in_device_uid, in_model_uid, in_manufacturer_uid,
              kRingFrames)) {
        return false;
    }

    // See DJMT1Device::initDevice(): without this the CoreAudio display
    // name stays empty, since SetName() in DJMT1Driver::Start_Impl() only
    // names the driver service, not the audio device object.
    SetName(in_model_uid);

    ivars->driver = OSSharedPtr(in_driver, OSRetain);
    ivars->outInterface = OSSharedPtr(in_out_interface, OSRetain);
    ivars->inInterface = OSSharedPtr(in_in_interface, OSRetain);

    SetZeroTimeStampPeriod(kRingFrames);

    double rate = kSampleRate;
    SetAvailableSampleRates(&rate, 1);
    SetSampleRate(rate);

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

    // Streams: 4 in / 4 out, 24-bit in 32-bit containers LE, 44.1 kHz.
    IOUserAudioStreamBasicDescription format{};
    format.mSampleRate       = kSampleRate;
    format.mFormatID         = IOUserAudioFormatID::LinearPCM;
    format.mFormatFlags      = static_cast<IOUserAudioFormatFlags>(
        static_cast<uint32_t>(IOUserAudioFormatFlags::FormatFlagIsSignedInteger) |
        static_cast<uint32_t>(IOUserAudioFormatFlags::FormatFlagIsAlignedHigh));
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

    auto inName = OSSharedPtr(OSString::withCString("SL2 Input"), OSNoRetain);
    auto outName = OSSharedPtr(OSString::withCString("SL2 Output"), OSNoRetain);
    ivars->inStream->SetName(inName.get());
    ivars->outStream->SetName(outName.get());

    if (AddStream(ivars->inStream.get()) != kIOReturnSuccess ||
        AddStream(ivars->outStream.get()) != kIOReturnSuccess) {
        return false;
    }

    SetInputSafetyOffset(kSafetyOffset);
    SetOutputSafetyOffset(kSafetyOffset);
    SetInputLatency(kSampleRate / 1000);
    SetOutputLatency(kSampleRate / 1000);

    return true;
}

bool SL2Device::SetupIsochTransfers(OSAction** in_isoch_in_actions,
                                    OSAction** in_isoch_out_actions)
{
    for (uint32_t i = 0; i < kNumURBs; i++) {
        if (!AllocURB(ivars->inURBs[i], kMicroframesPerURB * kMaxPacketBytes,
                      in_isoch_in_actions[i])) {
            return false;
        }
        if (!AllocURB(ivars->outURBs[i], kMicroframesPerURB * kMaxPacketBytes,
                      in_isoch_out_actions[i])) {
            return false;
        }
    }
    return true;
}

void SL2Device::free()
{
    IOSafeDeleteNULL(ivars, SL2Device_IVars, 1);
    super::free();
}

// Fill one OUT URB: pick 5 or 6 sample frames per microframe so the long-run
// average is 44100/8000, nudged towards the sample count actually delivered
// by the IN endpoint (implicit feedback — both directions share the device
// clock).
static void FillOutURB(SL2Device_IVars* ivars, URB& urb)
{
    for (uint32_t f = 0; f < kMicroframesPerURB; f++) {
        ivars->outAccumulator += kSampleRate;
        uint32_t samples = ivars->outAccumulator / kMicroframesPerSec;
        ivars->outAccumulator %= kMicroframesPerSec;

        int64_t drift = static_cast<int64_t>(ivars->outSampleCount)
                      - static_cast<int64_t>(ivars->inSampleCount);
        if (ivars->inSampleCount > 0) {
            if (drift > kFeedbackSlack && samples > 5) {
                samples--;
            } else if (drift < -kFeedbackSlack && samples < 7) {
                samples++;
            }
        }

        uint8_t* dst = urb.data_ptr + f * kMaxPacketBytes;
        uint64_t pos = ivars->outSampleCount % kRingFrames;
        uint32_t untilWrap = kRingFrames - static_cast<uint32_t>(pos);
        if (samples <= untilWrap) {
            memcpy(dst, ivars->outRingPtr + pos * kBytesPerFrame,
                   samples * kBytesPerFrame);
        } else {
            memcpy(dst, ivars->outRingPtr + pos * kBytesPerFrame,
                   untilWrap * kBytesPerFrame);
            memcpy(dst + untilWrap * kBytesPerFrame, ivars->outRingPtr,
                   (samples - untilWrap) * kBytesPerFrame);
        }
        ivars->outSampleCount += samples;

        urb.frames[f].status        = kIOReturnInvalid;
        urb.frames[f].requestCount  = samples * kBytesPerFrame;
        urb.frames[f].completeCount = 0;
        urb.frames[f].reserved      = 0;
        urb.frames[f].timeStamp     = 0;
    }
}

static void PrimeInFrameList(URB& urb)
{
    for (uint32_t i = 0; i < kMicroframesPerURB; i++) {
        urb.frames[i].status        = kIOReturnInvalid;
        urb.frames[i].requestCount  = kMaxPacketBytes;
        urb.frames[i].completeCount = 0;
        urb.frames[i].reserved      = 0;
        urb.frames[i].timeStamp     = 0;
    }
}

kern_return_t SL2Device::StartIO(IOUserAudioStartStopFlags in_flags)
{
    kern_return_t ret = IOUserAudioDevice::StartIO(in_flags);
    if (ret != kIOReturnSuccess) {
        return ret;
    }

    // No sample-rate request: the SL2 stalls UAC2_CS_CUR/RANGE and runs at
    // a fixed 44.1 kHz. Just select the streaming alternate settings.
    ret = ivars->outInterface->SelectAlternateSetting(1);
    if (ret != kIOReturnSuccess) {
        LOG("SelectAlternateSetting(1) on OUT interface failed: 0x%x", ret);
        goto fail;
    }
    ret = ivars->inInterface->SelectAlternateSetting(1);
    if (ret != kIOReturnSuccess) {
        LOG("SelectAlternateSetting(1) on IN interface failed: 0x%x", ret);
        goto fail;
    }

    {
        IOUSBHostPipe* pipe = nullptr;
        ret = ivars->inInterface->CopyPipe(kEndpointIn, &pipe);
        if (ret != kIOReturnSuccess) {
            LOG("CopyPipe(in 0x82) failed: 0x%x", ret);
            goto fail;
        }
        ivars->inPipe = OSSharedPtr(pipe, OSNoRetain);
        pipe = nullptr;
        ret = ivars->outInterface->CopyPipe(kEndpointOut, &pipe);
        if (ret != kIOReturnSuccess) {
            LOG("CopyPipe(out 0x06) failed: 0x%x", ret);
            goto fail;
        }
        ivars->outPipe = OSSharedPtr(pipe, OSNoRetain);
    }

    ivars->inSampleCount = 0;
    ivars->outSampleCount = 0;
    ivars->nextZeroSample = kRingFrames;
    ivars->outAccumulator = 0;
    memset(ivars->outRingPtr, 0, kRingBytes);

    UpdateCurrentZeroTimestamp(0, mach_absolute_time());

    {
        uint64_t frameNumber = 0;
        uint64_t frameTime = 0;
        ivars->inInterface->GetFrameNumber(&frameNumber, &frameTime);
        // GetFrameNumber counts 1 ms frames; IsochIO takes the same unit and
        // schedules on microframe granularity internally for bInterval 1.
        ivars->nextInFrameNumber = frameNumber + 4;
        ivars->nextOutFrameNumber = frameNumber + 4;
    }

    ivars->ioRunning = true;

    for (uint32_t i = 0; i < kNumURBs; i++) {
        URB& in = ivars->inURBs[i];
        PrimeInFrameList(in);
        ret = ivars->inPipe->IsochIO(in.data.get(), in.frameList.get(),
                                     ivars->nextInFrameNumber, in.completion.get());
        if (ret != kIOReturnSuccess) {
            LOG("initial IsochIO(in %u) failed: 0x%x", i, ret);
            goto fail;
        }
        ivars->nextInFrameNumber += kMicroframesPerURB / 8;

        URB& out = ivars->outURBs[i];
        FillOutURB(ivars, out);
        ret = ivars->outPipe->IsochIO(out.data.get(), out.frameList.get(),
                                      ivars->nextOutFrameNumber, out.completion.get());
        if (ret != kIOReturnSuccess) {
            LOG("initial IsochIO(out %u) failed: 0x%x", i, ret);
            goto fail;
        }
        ivars->nextOutFrameNumber += kMicroframesPerURB / 8;
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
    ivars->outInterface->SelectAlternateSetting(0);
    ivars->inInterface->SelectAlternateSetting(0);
    IOUserAudioDevice::StopIO(in_flags);
    return ret;
}

kern_return_t SL2Device::StopIO(IOUserAudioStartStopFlags in_flags)
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
    ivars->outInterface->SelectAlternateSetting(0);
    ivars->inInterface->SelectAlternateSetting(0);
    LOG("IO stopped");
    return IOUserAudioDevice::StopIO(in_flags);
}

void SL2Device::OnIsochInComplete(OSAction* action, IOReturn status)
{
    if (!ivars->ioRunning || status == kIOReturnAborted) {
        return;
    }
    uint32_t slot = *reinterpret_cast<uint32_t*>(action->GetReference());
    URB& urb = ivars->inURBs[slot];

    for (uint32_t f = 0; f < kMicroframesPerURB; f++) {
        IOUSBIsochronousFrame& frame = urb.frames[f];
        uint32_t bytes = (frame.status == kIOReturnSuccess) ? frame.completeCount : 0;
        uint32_t samples = bytes / kBytesPerFrame;
        if (samples > 0) {
            uint64_t pos = ivars->inSampleCount % kRingFrames;
            uint32_t untilWrap = kRingFrames - static_cast<uint32_t>(pos);
            uint8_t* src = urb.data_ptr + f * kMaxPacketBytes;
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
        // whenever the ring wraps, anchored to this microframe's completion.
        if (ivars->inSampleCount >= ivars->nextZeroSample && frame.timeStamp != 0) {
            UpdateCurrentZeroTimestamp(ivars->nextZeroSample, frame.timeStamp);
            ivars->nextZeroSample += kRingFrames;
        }
    }

    PrimeInFrameList(urb);
    kern_return_t ret = ivars->inPipe->IsochIO(urb.data.get(), urb.frameList.get(),
                                               ivars->nextInFrameNumber,
                                               urb.completion.get());
    if (ret != kIOReturnSuccess) {
        LOG("IsochIO(in resubmit) failed: 0x%x", ret);
    } else {
        ivars->nextInFrameNumber += kMicroframesPerURB / 8;
    }
}

void SL2Device::OnIsochOutComplete(OSAction* action, IOReturn status)
{
    if (!ivars->ioRunning || status == kIOReturnAborted) {
        return;
    }
    uint32_t slot = *reinterpret_cast<uint32_t*>(action->GetReference());
    URB& urb = ivars->outURBs[slot];

    FillOutURB(ivars, urb);
    kern_return_t ret = ivars->outPipe->IsochIO(urb.data.get(), urb.frameList.get(),
                                                ivars->nextOutFrameNumber,
                                                urb.completion.get());
    if (ret != kIOReturnSuccess) {
        LOG("IsochIO(out resubmit) failed: 0x%x", ret);
    } else {
        ivars->nextOutFrameNumber += kMicroframesPerURB / 8;
    }
}
