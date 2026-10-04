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
constexpr uint32_t kUSBFramesPerURB = 2;    // 2 ms per transaction
constexpr uint32_t kNumURBs         = 6;    // per direction, in flight; keep in
                                            // step with DJMT1Driver.cpp
constexpr uint32_t kRingFrames      = kSamplesPerMs * 256;           // 12288 (256 ms)
constexpr uint32_t kRingBytes       = kRingFrames * kBytesPerFrame;
constexpr uint8_t  kEndpointOut     = 0x01;
constexpr uint8_t  kEndpointIn      = 0x82;

// Timing model. The input stream is the device clock: inSampleCount is the
// device time at which the last captured sample was taken. A refilled output
// URB plays after the other kNumURBs - 1 queued ones, so its first sample
// goes out kOutAhead frames after "now" and has to be read from the ring at
// that device time. CoreAudio only writes the ring from (device time + output
// safety offset) on, so everything before that is final: the safety offset
// has to cover kOutAhead plus scheduling jitter. Reading further ahead than
// CoreAudio has written returns the previous lap of the ring (256 ms old),
// which is what the first versions did.
// Input data arrives in whole URBs and the completion is delivered a little
// after the URB ends, so the input safety offset covers one URB plus that.
constexpr uint32_t kOutAhead         = (kNumURBs - 1) * kUSBFramesPerURB * kSamplesPerMs;
constexpr uint32_t kOutSafetyOffset  = kOutAhead + 4 * kSamplesPerMs;
constexpr uint32_t kInSafetyOffset   = (kUSBFramesPerURB + 4) * kSamplesPerMs;
constexpr uint32_t kOutResyncFrames  = 2 * kUSBFramesPerURB * kSamplesPerMs;
// A completion that is handled later than the queued URBs cover (a stalled
// dext thread under system load) leaves the next frame number in the past:
// IsochIO then fails with kIOReturnIsoTooOld on every call and the stream
// stays dead. The frame numbers are re-anchored to the current USB frame
// plus this lead instead.
constexpr uint32_t kResubmitLead     = 4;
constexpr uint32_t kResubmitTries    = 4;
constexpr uint32_t kStatURBs         = 1000 / kUSBFramesPerURB;  // one log window per second

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

    // Stream statistics, summed over 125 completions (~1 s). Bad USB frames
    // used to be skipped silently, so a stalled stream left no trace in the
    // log. Lines are written when something is off (bad frames, an all-zero
    // input) and otherwise as a heartbeat every 10th interval.
    uint32_t inStatURBs = 0, inStatURBBad = 0, inStatOk = 0, inStatBad = 0;
    uint32_t inStatBytes = 0, inStatPeak = 0, inStatBeats = 0;
    IOReturn inStatFirstBad = 0, inStatLastURBStatus = 0;
    uint32_t outStatURBs = 0, outStatURBBad = 0, outStatBad = 0, outStatBeats = 0;
    IOReturn outStatFirstBad = 0, outStatLastURBStatus = 0;
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

// Largest absolute value among `count` packed little-endian 24-bit samples.
static uint32_t Peak24(const uint8_t* p, uint32_t count)
{
    uint32_t peak = 0;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t u = static_cast<uint32_t>(p[3 * i]) |
                     (static_cast<uint32_t>(p[3 * i + 1]) << 8) |
                     (static_cast<uint32_t>(p[3 * i + 2]) << 16);
        int32_t v = static_cast<int32_t>(u << 8) >> 8;   // sign-extend 24 bit
        uint32_t a = v < 0 ? static_cast<uint32_t>(-v) : static_cast<uint32_t>(v);
        if (a > peak) {
            peak = a;
        }
    }
    return peak;
}

bool DJMT1Device::init(IOUserAudioDriver* in_driver,
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
    ivars = IONewZero(DJMT1Device_IVars, 1);
    return ivars != nullptr;
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

    // Ohne diesen Aufruf bleibt der CoreAudio-Anzeigename des Geraets leer;
    // SetName() bei DJMT1Driver::Start_Impl() setzt nur den Namen des
    // Treiber-Service, nicht des Audiogeraets selbst.
    SetName(in_model_uid);

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

    SetInputSafetyOffset(kInSafetyOffset);
    SetOutputSafetyOffset(kOutSafetyOffset);
    SetInputLatency(kSamplesPerMs);
    SetOutputLatency(kSamplesPerMs);

    return true;
}

bool DJMT1Device::SetupIsochTransfers(OSAction** in_isoch_in_actions,
                                      OSAction** in_isoch_out_actions)
{
    for (uint32_t i = 0; i < kNumURBs; i++) {
        if (!AllocURB(ivars->inURBs[i], kUSBFramesPerURB * kInPacketSize,
                      in_isoch_in_actions[i])) {
            return false;
        }
        if (!AllocURB(ivars->outURBs[i], kUSBFramesPerURB * kBytesPerMs,
                      in_isoch_out_actions[i])) {
            return false;
        }
    }
    return true;
}

static void ResetURB(URB& urb)
{
    urb.completion.reset();
    urb.frameList.reset();
    urb.data.reset();
    urb.data_ptr = nullptr;
    urb.frames = nullptr;
}

void DJMT1Device::free()
{
    LOG("free");
    if (ivars) {
        // IONewZero()/IOSafeDeleteNULL() never run constructors or
        // destructors, so every OSSharedPtr in the ivars has to be released
        // by hand. Without this the device kept a strong reference to
        // DJMT1Driver (ivars->driver, and the completion actions, whose
        // target is the driver): DJMT1Driver::free() never ran, the dext
        // process stayed alive after the T1 was unplugged, and it blocked
        // later version upgrades until a reboot.
        for (uint32_t i = 0; i < kNumURBs; i++) {
            ResetURB(ivars->inURBs[i]);
            ResetURB(ivars->outURBs[i]);
        }
        ivars->inPipe.reset();
        ivars->outPipe.reset();
        ivars->inStream.reset();
        ivars->outStream.reset();
        ivars->inRing.reset();
        ivars->outRing.reset();
        ivars->interface.reset();
        ivars->driver.reset();
    }
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
        intf->GetFrameNumber(&frameNumber, nullptr);
        // A lead of only 4 frames (4 ms) once looked suspicious while the
        // real bug (see DJMT1Driver.iig) was still masking every isoch
        // completion; kept at 50 since that is the value verified working
        // end to end once the completions actually arrived.
        ivars->nextInFrameNumber = frameNumber + 50;
        ivars->nextOutFrameNumber = frameNumber + 50;
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

// Writes silence into the input ring and advances the device clock, so a gap
// in the capture does not shift the timeline.
static void AppendInputSilence(DJMT1Device_IVars* iv, uint64_t frames)
{
    while (frames > 0) {
        uint64_t pos = iv->inSampleCount % kRingFrames;
        uint32_t run = kRingFrames - static_cast<uint32_t>(pos);
        if (run > frames) {
            run = static_cast<uint32_t>(frames);
        }
        memset(iv->inRingPtr + pos * kBytesPerFrame, 0, run * kBytesPerFrame);
        iv->inSampleCount += run;
        frames -= run;
    }
}

void DJMT1Device::OnIsochInComplete(OSAction* action, IOReturn status)
{
    if (!ivars->ioRunning || status == kIOReturnAborted) {
        return;
    }
    uint32_t slot = *reinterpret_cast<uint32_t*>(action->GetReference());
    URB& urb = ivars->inURBs[slot];

    ivars->inStatURBs++;
    if (status != kIOReturnSuccess) {
        ivars->inStatURBBad++;
        ivars->inStatLastURBStatus = status;
    }

    for (uint32_t f = 0; f < kUSBFramesPerURB; f++) {
        IOUSBIsochronousFrame& frame = urb.frames[f];
        uint32_t bytes = (frame.status == kIOReturnSuccess) ? frame.completeCount : 0;
        uint32_t samples = bytes / kBytesPerFrame;
        if (frame.status == kIOReturnSuccess) {
            ivars->inStatOk++;
            ivars->inStatBytes += frame.completeCount;
            uint32_t pk = Peak24(urb.data_ptr + f * kInPacketSize, samples * kChannels);
            if (pk > ivars->inStatPeak) {
                ivars->inStatPeak = pk;
            }
        } else {
            ivars->inStatBad++;
            if (ivars->inStatFirstBad == 0) {
                ivars->inStatFirstBad = frame.status;
            }
        }
        if (frame.status != kIOReturnSuccess) {
            AppendInputSilence(ivars, kSamplesPerMs);
        } else if (samples > 0) {
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

    if (ivars->inStatURBs >= kStatURBs) {
        ivars->inStatBeats++;
        bool odd = ivars->inStatBad > 0 || ivars->inStatURBBad > 0 || ivars->inStatPeak == 0;
        if (odd || ivars->inStatBeats % 10 == 0) {
            LOG("in stats: urbs=%u urbBad=%u (last 0x%x) framesOk=%u framesBad=%u (first 0x%x) "
                "bytes=%u peak=%u inSamples=%llu nextFrame=%llu",
                ivars->inStatURBs, ivars->inStatURBBad, ivars->inStatLastURBStatus,
                ivars->inStatOk, ivars->inStatBad, ivars->inStatFirstBad,
                ivars->inStatBytes, ivars->inStatPeak, ivars->inSampleCount,
                ivars->nextInFrameNumber);
        }
        ivars->inStatURBs = ivars->inStatURBBad = ivars->inStatOk = ivars->inStatBad = 0;
        ivars->inStatBytes = ivars->inStatPeak = 0;
        ivars->inStatFirstBad = ivars->inStatLastURBStatus = 0;
    }

    PrimeFrameList(urb, kInPacketSize);
    kern_return_t ret = ivars->inPipe->IsochIO(urb.data.get(), urb.frameList.get(),
                                               ivars->nextInFrameNumber,
                                               urb.completion.get());
    for (uint32_t attempt = 0; ret == kIOReturnIsoTooOld && attempt < kResubmitTries; attempt++) {
        uint64_t now = 0;
        ivars->interface->GetFrameNumber(&now, nullptr);
        uint64_t target = now + kResubmitLead * (attempt + 1);
        uint64_t lost = target > ivars->nextInFrameNumber ? target - ivars->nextInFrameNumber : 0;
        AppendInputSilence(ivars, lost * kSamplesPerMs);
        LOG("input stream late by %llu ms, re-anchored to frame %llu", lost, target);
        ivars->nextInFrameNumber = target;
        PrimeFrameList(urb, kInPacketSize);
        ret = ivars->inPipe->IsochIO(urb.data.get(), urb.frameList.get(),
                                     ivars->nextInFrameNumber, urb.completion.get());
    }
    if (ret != kIOReturnSuccess) {
        LOG("IsochIO(in resubmit) failed: 0x%x", ret);
    } else {
        ivars->nextInFrameNumber += kUSBFramesPerURB;
    }
}

void DJMT1Device::OnIsochOutComplete(OSAction* action, IOReturn status)
{
    if (!ivars->ioRunning || status == kIOReturnAborted) {
        return;
    }
    uint32_t slot = *reinterpret_cast<uint32_t*>(action->GetReference());
    URB& urb = ivars->outURBs[slot];

    ivars->outStatURBs++;
    if (status != kIOReturnSuccess) {
        ivars->outStatURBBad++;
        ivars->outStatLastURBStatus = status;
    }
    for (uint32_t f = 0; f < kUSBFramesPerURB; f++) {
        if (urb.frames[f].status != kIOReturnSuccess) {
            ivars->outStatBad++;
            if (ivars->outStatFirstBad == 0) {
                ivars->outStatFirstBad = urb.frames[f].status;
            }
        }
    }
    if (ivars->outStatURBs >= kStatURBs) {
        ivars->outStatBeats++;
        if (ivars->outStatBad > 0 || ivars->outStatURBBad > 0 || ivars->outStatBeats % 10 == 0) {
            LOG("out stats: urbs=%u urbBad=%u (last 0x%x) framesBad=%u (first 0x%x) "
                "outSamples=%llu nextFrame=%llu",
                ivars->outStatURBs, ivars->outStatURBBad, ivars->outStatLastURBStatus,
                ivars->outStatBad, ivars->outStatFirstBad, ivars->outSampleCount,
                ivars->nextOutFrameNumber);
        }
        ivars->outStatURBs = ivars->outStatURBBad = ivars->outStatBad = 0;
        ivars->outStatFirstBad = ivars->outStatLastURBStatus = 0;
    }

    // Refill this URB from the output ring. The read position follows the
    // device clock; it only moves if the two have drifted apart by more than
    // the phase difference between input and output completions.
    int64_t wanted = static_cast<int64_t>(ivars->inSampleCount) + kOutAhead;
    int64_t diff = wanted - static_cast<int64_t>(ivars->outSampleCount);
    if (diff > static_cast<int64_t>(kOutResyncFrames) ||
        diff < -static_cast<int64_t>(kOutResyncFrames)) {
        LOG("out position resync: %lld frames (in %llu, out %llu)", diff,
            ivars->inSampleCount, ivars->outSampleCount);
        ivars->outSampleCount = static_cast<uint64_t>(wanted);
    }
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
    for (uint32_t attempt = 0; ret == kIOReturnIsoTooOld && attempt < kResubmitTries; attempt++) {
        uint64_t now = 0;
        ivars->interface->GetFrameNumber(&now, nullptr);
        uint64_t target = now + kResubmitLead * (attempt + 1);
        LOG("output stream late, re-anchored to frame %llu", target);
        ivars->nextOutFrameNumber = target;
        PrimeFrameList(urb, kBytesPerMs);
        ret = ivars->outPipe->IsochIO(urb.data.get(), urb.frameList.get(),
                                      ivars->nextOutFrameNumber, urb.completion.get());
    }
    if (ret != kIOReturnSuccess) {
        LOG("IsochIO(out resubmit) failed: 0x%x", ret);
    } else {
        ivars->nextOutFrameNumber += kUSBFramesPerURB;
    }
}
