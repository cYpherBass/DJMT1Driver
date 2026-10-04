// t1route.c -- liest den Eingangsschalter-Status des Pioneer DJM-T1 und stellt
// die Quelle der USB-Ausgaenge ein, so wie es Pioneers "DJM-T1 Setting
// Utility" bzw. dessen AutoSetup beim Anstecken tut.
//
// Gewonnen aus PioneerDJMSetup.framework (DJM-T1_M_1.2.0.dmg), nur gelesen,
// nie ausgefuehrt:
//   - lesen: bmRequestType 0xC0 (vendor, device-to-host), bRequest 0x00,
//            wValue 0, wIndex 0x8002, 3 Byte   (Stellung der Eingangsschalter)
//   - setzen: bmRequestType 0x40, bRequest 0x03, wIndex 0x8002,
//             wValue = Code aus Pioneers Tabelle __convTable_MID, wLength 0
// Der aktuelle Routing-Stand laesst sich vom Mixer nicht zuruecklesen; Pioneers
// Software merkt ihn sich in einer Einstellungsdatei auf dem Mac.
//
// Build: clang -o t1route tools/t1route.c -framework IOKit -framework CoreFoundation
// Run:   ./t1route status
//        ./t1route set 1 0     # USB 1/2 <- CH1 Timecode PHONO
//        ./t1route set 2 0     # USB 3/4 <- CH2 Timecode PHONO
//        ./t1route set 3 <n>   # USB 5/6 (siehe Tabelle unten)
//        ./t1route tc-phono    # beides, USB 1/2 und USB 3/4 auf Timecode PHONO
//
// Aenderungen am Mixer machst du damit auf eigene Verantwortung: genau die
// Befehle, die Pioneers Software sendet, nicht mehr.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/usb/IOUSBLib.h>
#include <IOKit/IOCFPlugIn.h>
#include <CoreFoundation/CoreFoundation.h>

#define VID 2276
#define PID 350

static const char *kPairName[3] = { "USB 1/2", "USB 3/4", "USB 5/6" };
static const char *kOption[3][6] = {
    { "CH1 Timecode PHONO", "CH1 Timecode CD", 0, 0, 0, 0 },
    { "CH2 Timecode PHONO", "CH2 Timecode CD", 0, 0, 0, 0 },
    { "Post CH1 Fader", "Post CH2 Fader", "Cross Fader A", "Cross Fader B",
      "MIC/AUX Post EQ", "REC OUT" },
};
// Pioneers __convTable_MID, je 6 Eintraege pro USB-Paar.
static const unsigned short kConv[3][6] = {
    { 0x1103, 0x1100, 0xffff, 0xffff, 0xffff, 0xffff },
    { 0x2203, 0x2200, 0xffff, 0xffff, 0xffff, 0xffff },
    { 0x1306, 0x2306, 0x0307, 0x0308, 0x0309, 0x030a },
};

static IOUSBDeviceInterface **openDevice(void) {
    int vid = VID, pid = PID;
    CFMutableDictionaryRef match = IOServiceMatching("IOUSBHostDevice");
    CFNumberRef v = CFNumberCreate(NULL, kCFNumberIntType, &vid);
    CFNumberRef p = CFNumberCreate(NULL, kCFNumberIntType, &pid);
    CFDictionarySetValue(match, CFSTR("idVendor"), v);
    CFDictionarySetValue(match, CFSTR("idProduct"), p);
    io_service_t svc = IOServiceGetMatchingService(kIOMainPortDefault, match);
    if (!svc) { fprintf(stderr, "DJM-T1 nicht gefunden (VID %d PID %d)\n", VID, PID); return NULL; }
    IOCFPlugInInterface **plug = NULL; SInt32 score;
    kern_return_t kr = IOCreatePlugInInterfaceForService(svc, kIOUSBDeviceUserClientTypeID,
        kIOCFPlugInInterfaceID, &plug, &score);
    IOObjectRelease(svc);
    if (kr || !plug) { fprintf(stderr, "Plugin-Fehler 0x%x\n", kr); return NULL; }
    IOUSBDeviceInterface **dev = NULL;
    (*plug)->QueryInterface(plug, CFUUIDGetUUIDBytes(kIOUSBDeviceInterfaceID), (LPVOID *)&dev);
    (*plug)->Release(plug);
    if (!dev) { fprintf(stderr, "kein IOUSBDeviceInterface\n"); return NULL; }
    kr = (*dev)->USBDeviceOpen(dev);
    if (kr) { fprintf(stderr, "USBDeviceOpen fehlgeschlagen: 0x%x\n", kr); (*dev)->Release(dev); return NULL; }
    return dev;
}

static int cmdStatus(IOUSBDeviceInterface **dev) {
    unsigned char buf[8] = {0};
    IOUSBDevRequest req = { .bmRequestType = 0xC0, .bRequest = 0x00, .wValue = 0,
                            .wIndex = 0x8002, .wLength = 3, .pData = buf };
    kern_return_t kr = (*dev)->DeviceRequest(dev, &req);
    if (kr) { fprintf(stderr, "Lesen fehlgeschlagen: 0x%x\n", kr); return 1; }
    printf("Eingangsschalter-Antwort (%u Byte): %02x %02x %02x\n", (unsigned)req.wLenDone, buf[0], buf[1], buf[2]);
    printf("  Byte 1 (CH1): %u   Byte 2 (CH2): %u   (Pioneers Software wertet 2 als \"USB\" aus)\n", buf[1], buf[2]);
    return 0;
}

static int cmdSet(IOUSBDeviceInterface **dev, int pair, int opt) {
    if (pair < 1 || pair > 3 || opt < 0 || opt > 5 || kConv[pair - 1][opt] == 0xffff
        || !kOption[pair - 1][opt]) {
        fprintf(stderr, "ungueltige Auswahl\n");
        return 2;
    }
    unsigned short wValue = kConv[pair - 1][opt];
    unsigned char zero[2] = {0, 0};
    IOUSBDevRequest req = { .bmRequestType = 0x40, .bRequest = 0x03, .wValue = wValue,
                            .wIndex = 0x8002, .wLength = 0, .pData = zero };
    printf("%s <- %s  (wValue 0x%04x) ... ", kPairName[pair - 1], kOption[pair - 1][opt], wValue);
    fflush(stdout);
    kern_return_t kr = (*dev)->DeviceRequest(dev, &req);
    if (kr) { printf("FEHLER 0x%x\n", kr); return 1; }
    printf("ok\n");
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "Aufruf: t1route status | set <1-3> <option> | tc-phono\n");
        for (int p = 0; p < 3; p++)
            for (int o = 0; o < 6; o++)
                if (kOption[p][o]) fprintf(stderr, "  set %d %d   %s <- %s\n", p + 1, o, kPairName[p], kOption[p][o]);
        return 2;
    }
    IOUSBDeviceInterface **dev = openDevice();
    if (!dev) return 1;
    int rc = 0;
    if (!strcmp(argv[1], "status")) rc = cmdStatus(dev);
    else if (!strcmp(argv[1], "set") && argc == 4) rc = cmdSet(dev, atoi(argv[2]), atoi(argv[3]));
    else if (!strcmp(argv[1], "tc-phono")) { rc = cmdSet(dev, 1, 0); if (!rc) rc = cmdSet(dev, 2, 0); }
    else { fprintf(stderr, "unbekannter Befehl\n"); rc = 2; }
    (*dev)->USBDeviceClose(dev);
    (*dev)->Release(dev);
    return rc;
}
