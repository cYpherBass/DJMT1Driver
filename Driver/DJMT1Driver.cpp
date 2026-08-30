//
// DJMT1Driver.cpp
// Driver service: matches interface 0 of the DJM-T1, opens it and
// publishes the audio device.
//

#include <DriverKit/DriverKit.h>
#include <DriverKit/OSSharedPtr.h>
#include <USBDriverKit/USBDriverKit.h>
#include <AudioDriverKit/AudioDriverKit.h>

#include "DJMT1Driver.h"
#include "DJMT1Device.h"

#define LOG(fmt, ...) os_log(OS_LOG_DEFAULT, "DJMT1Driver: " fmt, ##__VA_ARGS__)

struct DJMT1Driver_IVars
{
    OSSharedPtr<IOUSBHostInterface> interface;
    OSSharedPtr<DJMT1Device>        device;
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
        ivars->device.reset();
    }
    IOSafeDeleteNULL(ivars, DJMT1Driver_IVars, 1);
    super::free();
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

    ret = interface->Open(this, 0, nullptr);
    if (ret != kIOReturnSuccess) {
        LOG("failed to open USB interface: 0x%x", ret);
        Stop(provider, SUPERDISPATCH);
        return ret;
    }

    SetTransportType(IOUserAudioTransportType::USB);

    auto deviceUID = OSSharedPtr(OSString::withCString("DJM-T1"), OSNoRetain);
    auto modelUID = OSSharedPtr(OSString::withCString("Pioneer DJM-T1"), OSNoRetain);
    auto manufacturerUID = OSSharedPtr(OSString::withCString("Pioneer DJ"), OSNoRetain);
    auto driverName = OSSharedPtr(OSString::withCString("DJM-T1 Audio"), OSNoRetain);
    SetName(driverName.get());

    DJMT1Device* device = OSTypeAlloc(DJMT1Device);
    if (device == nullptr) {
        LOG("failed to allocate DJMT1Device");
        interface->Close(this, 0);
        Stop(provider, SUPERDISPATCH);
        return kIOReturnNoMemory;
    }
    if (!device->initDevice(this, interface,
                            deviceUID.get(), modelUID.get(), manufacturerUID.get())) {
        LOG("DJMT1Device init failed");
        device->release();
        interface->Close(this, 0);
        Stop(provider, SUPERDISPATCH);
        return kIOReturnError;
    }
    ivars->device = OSSharedPtr(device, OSNoRetain);

    ret = AddObject(ivars->device.get());
    if (ret != kIOReturnSuccess) {
        LOG("AddObject failed: 0x%x", ret);
        interface->Close(this, 0);
        Stop(provider, SUPERDISPATCH);
        return ret;
    }

    ret = RegisterService();
    if (ret != kIOReturnSuccess) {
        LOG("RegisterService failed: 0x%x", ret);
        return ret;
    }

    LOG("started, audio device published");
    return kIOReturnSuccess;
}

kern_return_t IMPL(DJMT1Driver, Stop)
{
    if (ivars->device) {
        RemoveObject(ivars->device.get());
        ivars->device.reset();
    }
    if (ivars->interface) {
        ivars->interface->Close(this, 0);
        ivars->interface.reset();
    }
    return Stop(provider, SUPERDISPATCH);
}
