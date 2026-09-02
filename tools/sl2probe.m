//
// sl2probe.m — user-space probe for the Rane SL2 (1cc5:0013).
//
// Verifies the hardware contract documented by the Linux quirk
// (github.com/Reinharderino/rane-sl2-linux) from macOS, without any
// entitlements: claims interface 2 (input stream), selects alternate
// setting 1 and reads isochronous data from EP 0x82.
//
// Expected: 4 channels, 24-bit in 4-byte subslots, fixed 44.1 kHz
// (~80/96-byte microframe packets). Do NOT send SET_CUR sample-rate
// requests — the device stalls them.
//
// Build: clang -o sl2probe tools/sl2probe.m -framework Foundation \
//        -framework IOUSBHost -framework IOKit -fobjc-arc
//

#import <Foundation/Foundation.h>
#import <IOUSBHost/IOUSBHost.h>
#import <IOKit/usb/USB.h>

static io_service_t findInterface(int ifNum) {
  CFMutableDictionaryRef m = IOServiceMatching("IOUSBHostInterface");
  io_iterator_t it = IO_OBJECT_NULL;
  io_service_t found = IO_OBJECT_NULL;
  if (IOServiceGetMatchingServices(kIOMainPortDefault, m, &it) == KERN_SUCCESS) {
    io_service_t s;
    while ((s = IOIteratorNext(it))) {
      NSNumber *vid = CFBridgingRelease(IORegistryEntryCreateCFProperty(s, CFSTR("idVendor"), NULL, 0));
      NSNumber *pid = CFBridgingRelease(IORegistryEntryCreateCFProperty(s, CFSTR("idProduct"), NULL, 0));
      NSNumber *ifn = CFBridgingRelease(IORegistryEntryCreateCFProperty(s, CFSTR("bInterfaceNumber"), NULL, 0));
      if (vid.intValue == 0x1cc5 && pid.intValue == 0x0013 && ifn.intValue == ifNum) { found = s; break; }
      IOObjectRelease(s);
    }
    IOObjectRelease(it);
  }
  return found;
}

int main(void) {
  @autoreleasepool {
    io_service_t svc = findInterface(2);
    if (!svc) { printf("SL2 interface 2 not found (is the SL2 connected?)\n"); return 1; }
    NSError *err = nil;
    IOUSBHostInterface *intf = [[IOUSBHostInterface alloc] initWithIOService:svc options:0
        queue:dispatch_queue_create("usb", NULL) error:&err interestHandler:nil];
    if (!intf) { printf("open interface 2 failed: %s\n", err.description.UTF8String); return 1; }
    printf("interface 2 claimed OK\n");
    if (![intf selectAlternateSetting:1 error:&err]) {
      printf("selectAlt 1 failed: %s\n", err.description.UTF8String); return 1;
    }
    printf("alt setting 1 selected — no SET_CUR, device is fixed 44.1 kHz\n");

    IOUSBHostPipe *inPipe = [intf copyPipeWithAddress:0x82 error:&err];
    if (!inPipe) { printf("copy IN pipe 0x82 failed: %s\n", err.description.UTF8String); return 1; }
    printf("IN pipe 0x82 open\n");

    // bInterval 1 at high speed: one transaction per 125 us microframe.
    const int NF = 64;                 // 8 ms per request
    const uint32_t PKT = 112;          // wMaxPacketSize
    NSMutableData *buf = [NSMutableData dataWithLength:PKT * NF];
    uint64_t totalBytes = 0;
    NSDate *t0 = nil;
    const int ROUNDS = 128;            // ~1 s of audio
    for (int round = 0; round < ROUNDS; round++) {
      IOUSBHostIsochronousFrame frames[NF];
      memset(frames, 0, sizeof(frames));
      for (int i = 0; i < NF; i++) frames[i].requestCount = PKT;
      if (![inPipe sendIORequestWithData:buf frameList:frames frameListCount:NF
                        firstFrameNumber:0 error:&err]) {
        printf("isoch read failed: %s\n", err.description.UTF8String); return 1;
      }
      if (!t0) t0 = [NSDate date];     // start timing after the first transfer
      else {
        for (int i = 0; i < NF; i++) totalBytes += frames[i].completeCount;
      }
      if (round == 1) {
        printf("first packets:");
        for (int i = 0; i < 8; i++) printf(" %u", frames[i].completeCount);
        printf(" bytes (expect 80/96 = 5/6 samples * 16 B)\n");
      }
    }
    double secs = -[t0 timeIntervalSinceNow];
    double frames44 = (double)totalBytes / 16.0;   // 4 ch * 4 B
    printf("received %llu bytes in %.3f s => %.0f Hz (expect ~44100)\n",
           (unsigned long long)totalBytes, secs, frames44 / secs);
  }
  return 0;
}
