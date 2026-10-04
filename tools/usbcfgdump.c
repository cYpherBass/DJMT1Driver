// usbcfgdump.c — prints the raw configuration descriptor of a USB device as
// hex. Used to read the DJM-T1's descriptors (2026-08-30).
//
// Build: clang -o usbcfgdump tools/usbcfgdump.c -framework IOKit -framework CoreFoundation
// Run:   ./usbcfgdump [vid pid]   (decimal; default 2276 350 = DJM-T1,
//                                   Rane SL2 = 7365 19)

#include <stdio.h>
#include <stdlib.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/usb/IOUSBLib.h>
#include <IOKit/IOCFPlugIn.h>
#include <CoreFoundation/CoreFoundation.h>

int main(int argc, char **argv) {
    int vid = argc > 2 ? atoi(argv[1]) : 2276;
    int pid = argc > 2 ? atoi(argv[2]) : 350;
    CFMutableDictionaryRef match = IOServiceMatching("IOUSBHostDevice");
    CFNumberRef v = CFNumberCreate(NULL, kCFNumberIntType, &vid);
    CFNumberRef p = CFNumberCreate(NULL, kCFNumberIntType, &pid);
    CFDictionarySetValue(match, CFSTR("idVendor"), v);
    CFDictionarySetValue(match, CFSTR("idProduct"), p);
    io_service_t svc = IOServiceGetMatchingService(kIOMainPortDefault, match);
    if (!svc) { fprintf(stderr, "device not found\n"); return 1; }
    IOCFPlugInInterface **plug = NULL; SInt32 score;
    kern_return_t kr = IOCreatePlugInInterfaceForService(svc, kIOUSBDeviceUserClientTypeID,
        kIOCFPlugInInterfaceID, &plug, &score);
    if (kr) { fprintf(stderr, "plugin err %x\n", kr); return 1; }
    IOUSBDeviceInterface **dev = NULL;
    (*plug)->QueryInterface(plug, CFUUIDGetUUIDBytes(kIOUSBDeviceInterfaceID), (LPVOID*)&dev);
    IODestroyPlugInInterface(plug);
    if (!dev) { fprintf(stderr, "no dev interface\n"); return 1; }
    IOUSBConfigurationDescriptorPtr cfg = NULL;
    kr = (*dev)->GetConfigurationDescriptorPtr(dev, 0, &cfg);
    if (kr || !cfg) { fprintf(stderr, "getcfg err %x\n", kr); return 1; }
    int len = cfg->wTotalLength;
    unsigned char *b = (unsigned char *)cfg;
    for (int i = 0; i < len; i++) printf("%02x", b[i]);
    printf("\n");
    return 0;
}
