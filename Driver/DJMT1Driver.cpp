//
// DJMT1Driver.cpp
// Driver service for both supported devices. Dispatches on the USB vendor
// ID of the matched interface's device:
//   - Pioneer DJM-T1 (0x08E4:0x015E): matched on interface 0
//   - Rane SL2       (0x1CC5:0x0013): matched on interface 1 (OUT stream);
//     the IN stream lives on interface 2, which is looked up and opened here
//

#include <DriverKit/DriverKit.h>
#include <DriverKit/OSSharedPtr.h>
#include <USBDriverKit/USBDriverKit.h>
#include <AudioDriverKit/AudioDriverKit.h>

#include "DJMT1Driver.h"
#include "DJMT1Device.h"
#include "SL2Device.h"

#define LOG(fmt, ...) os_log(OS_LOG_DEFAULT, "DJMT1Driver: " fmt, ##__VA_ARGS__)

namespace {
constexpr uint16_t kVendorPioneer = 0x08E4;
constexpr uint16_t kVendorRane    = 0x1CC5;
}

struct DJMT1Driver_IVars
{
    OSSharedPtr<IOUSBHostInterface> interface;      // matched interface
    OSSharedPtr<IOUSBHostInterface> secondInterface; // SL2 only: interface 2 (IN)
    OSSharedPtr<IOUserAudioDevice>  device;
    OSSharedPtr<DJMT1Device>        pioneerDevice;   // same object as device,
                                                     // typed for OnIsoch*Complete
};

bool DJMT1Driver::init()
{
    if (!super::init()) {
        return false;
    }
    ivars = IONewZero(DJMT1Driver_IVars, 1);
    return ivars != nullptr;
}

void DJMT1Driver::free()
{
    if (ivars) {
        ivars->interface.reset();
        ivars->secondInterface.reset();
        ivars->device.reset();
        ivars->pioneerDevice.reset();
    }
    IOSafeDeleteNULL(ivars, DJMT1Driver_IVars, 1);
    super::free();
}

// Find the sibling interface with the given bInterfaceNumber on the same
// device. Returns a retained interface, or nullptr.
static IOUSBHostInterface* CopySiblingInterface(IOUSBHostDevice* device,
                                                uint8_t interfaceNumber)
{
    IOUSBHostInterface* found = nullptr;
    uintptr_t iter = 0;
    if (device->CreateInterfaceIterator(&iter) != kIOReturnSuccess) {
        return nullptr;
    }
    for (;;) {
        IOUSBHostInterface* candidate = nullptr;
        if (device->CopyInterface(iter, &candidate) != kIOReturnSuccess ||
            candidate == nullptr) {
            break;
        }
        const IOUSBConfigurationDescriptor* cfg =
            candidate->CopyConfigurationDescriptor();
        uint8_t num = 0xFF;
        if (cfg != nullptr) {
            const IOUSBInterfaceDescriptor* idesc =
                candidate->GetInterfaceDescriptor(cfg);
            if (idesc != nullptr) {
                num = idesc->bInterfaceNumber;
            }
            IOUSBHostFreeDescriptor(cfg);
        }
        if (num == interfaceNumber) {
            found = candidate;
            break;
        }
        candidate->release();
    }
    device->DestroyInterfaceIterator(iter);
    return found;
}

kern_return_t IMPL(DJMT1Driver, Start)
{
    kern_return_t ret = Start(provider, SUPERDISPATCH);
    if (ret != kIOReturnSuccess) {
        LOG("super::Start failed: 0x%x", ret);
        return ret;
    }

    IOUSBHostInterface* interface = OSDynamicCast(IOUSBHostInterface, provider);
    if (interface == nullptr) {
        LOG("provider is not an IOUSBHostInterface");
        Stop(provider, SUPERDISPATCH);
        return kIOReturnBadArgument;
    }
    ivars->interface = OSSharedPtr(interface, OSRetain);

    // Identify the device behind the matched interface.
    IOUSBHostDevice* usbDevice = nullptr;
    ret = interface->CopyDevice(&usbDevice);
    if (ret != kIOReturnSuccess || usbDevice == nullptr) {
        LOG("CopyDevice failed: 0x%x", ret);
        Stop(provider, SUPERDISPATCH);
        return kIOReturnNoDevice;
    }
    auto device = OSSharedPtr(usbDevice, OSNoRetain);

    uint16_t vid = 0;
    const IOUSBDeviceDescriptor* dd = device->CopyDeviceDescriptor();
    if (dd != nullptr) {
        vid = dd->idVendor;
        IOUSBHostFreeDescriptor(dd);
    }

    ret = interface->Open(this, 0, nullptr);
    if (ret != kIOReturnSuccess) {
        LOG("failed to open USB interface: 0x%x", ret);
        Stop(provider, SUPERDISPATCH);
        return ret;
    }

    SetTransportType(IOUserAudioTransportType::USB);

    if (vid == kVendorPioneer) {
        auto deviceUID = OSSharedPtr(OSString::withCString("DJM-T1"), OSNoRetain);
        auto modelUID = OSSharedPtr(OSString::withCString("Pioneer DJM-T1"), OSNoRetain);
        auto manufacturerUID = OSSharedPtr(OSString::withCString("Pioneer DJ"), OSNoRetain);
        auto driverName = OSSharedPtr(OSString::withCString("DJM-T1 Audio"), OSNoRetain);
        SetName(driverName.get());

        DJMT1Device* audioDevice = OSTypeAlloc(DJMT1Device);
        if (audioDevice == nullptr ||
            !audioDevice->initDevice(this, interface, deviceUID.get(),
                                     modelUID.get(), manufacturerUID.get())) {
            LOG("DJMT1Device init failed");
            OSSafeReleaseNULL(audioDevice);
            goto fail_close;
        }

        // The completion actions must be created here, on DJMT1Driver (a
        // real IOService), not on DJMT1Device -- see DJMT1Driver.iig.
        {
            constexpr uint32_t kNumURBs = 4;  // must match DJMT1Device's kNumURBs
            OSAction* inActions[kNumURBs] = {};
            OSAction* outActions[kNumURBs] = {};
            bool ok = true;
            for (uint32_t i = 0; i < kNumURBs; i++) {
                if (CreateActionHandleIsochInComplete(sizeof(uint32_t), &inActions[i])
                    != kIOReturnSuccess) {
                    LOG("CreateActionHandleIsochInComplete(%u) failed", i);
                    ok = false;
                    break;
                }
                *reinterpret_cast<uint32_t*>(inActions[i]->GetReference()) = i;

                if (CreateActionHandleIsochOutComplete(sizeof(uint32_t), &outActions[i])
                    != kIOReturnSuccess) {
                    LOG("CreateActionHandleIsochOutComplete(%u) failed", i);
                    ok = false;
                    break;
                }
                *reinterpret_cast<uint32_t*>(outActions[i]->GetReference()) = i;
            }
            if (ok) {
                ok = audioDevice->SetupIsochTransfers(inActions, outActions);
            }
            for (uint32_t i = 0; i < kNumURBs; i++) {
                OSSafeReleaseNULL(inActions[i]);
                OSSafeReleaseNULL(outActions[i]);
            }
            if (!ok) {
                LOG("SetupIsochTransfers failed");
                OSSafeReleaseNULL(audioDevice);
                goto fail_close;
            }
        }

        ivars->device = OSSharedPtr<IOUserAudioDevice>(audioDevice, OSNoRetain);
        ivars->pioneerDevice = OSSharedPtr<DJMT1Device>(audioDevice, OSRetain);
    } else if (vid == kVendorRane) {
        // Matched on interface 1 (OUT stream); the IN stream is interface 2.
        IOUSBHostInterface* inInterface = CopySiblingInterface(device.get(), 2);
        if (inInterface == nullptr) {
            LOG("SL2: interface 2 not found");
            goto fail_close;
        }
        ivars->secondInterface = OSSharedPtr(inInterface, OSNoRetain);

        ret = inInterface->Open(this, 0, nullptr);
        if (ret != kIOReturnSuccess) {
            LOG("SL2: failed to open interface 2: 0x%x", ret);
            goto fail_close;
        }

        auto deviceUID = OSSharedPtr(OSString::withCString("Rane-SL2"), OSNoRetain);
        auto modelUID = OSSharedPtr(OSString::withCString("Rane SL 2"), OSNoRetain);
        auto manufacturerUID = OSSharedPtr(OSString::withCString("Rane"), OSNoRetain);
        auto driverName = OSSharedPtr(OSString::withCString("Rane SL2 Audio"), OSNoRetain);
        SetName(driverName.get());

        SL2Device* audioDevice = OSTypeAlloc(SL2Device);
        if (audioDevice == nullptr ||
            !audioDevice->initDevice(this, interface, inInterface, deviceUID.get(),
                                     modelUID.get(), manufacturerUID.get())) {
            LOG("SL2Device init failed");
            OSSafeReleaseNULL(audioDevice);
            goto fail_close;
        }
        ivars->device = OSSharedPtr<IOUserAudioDevice>(audioDevice, OSNoRetain);
    } else {
        LOG("unexpected vendor id 0x%x", vid);
        goto fail_close;
    }

    ret = AddObject(ivars->device.get());
    if (ret != kIOReturnSuccess) {
        LOG("AddObject failed: 0x%x", ret);
        goto fail_close;
    }

    ret = RegisterService();
    if (ret != kIOReturnSuccess) {
        LOG("RegisterService failed: 0x%x", ret);
        return ret;
    }

    LOG("started, audio device published (vid 0x%x)", vid);
    return kIOReturnSuccess;

fail_close:
    if (ivars->secondInterface) {
        ivars->secondInterface->Close(this, 0);
        ivars->secondInterface.reset();
    }
    interface->Close(this, 0);
    Stop(provider, SUPERDISPATCH);
    return kIOReturnError;
}

void DJMT1Driver::HandleIsochInComplete_Impl(OSAction* action, IOReturn status)
{
    if (ivars->pioneerDevice) {
        ivars->pioneerDevice->OnIsochInComplete(action, status);
    }
}

void DJMT1Driver::HandleIsochOutComplete_Impl(OSAction* action, IOReturn status)
{
    if (ivars->pioneerDevice) {
        ivars->pioneerDevice->OnIsochOutComplete(action, status);
    }
}

kern_return_t IMPL(DJMT1Driver, Stop)
{
    if (ivars->device) {
        RemoveObject(ivars->device.get());
        ivars->device.reset();
    }
    ivars->pioneerDevice.reset();
    if (ivars->secondInterface) {
        ivars->secondInterface->Close(this, 0);
        ivars->secondInterface.reset();
    }
    if (ivars->interface) {
        ivars->interface->Close(this, 0);
        ivars->interface.reset();
    }
    return Stop(provider, SUPERDISPATCH);
}
