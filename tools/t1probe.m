#import <Foundation/Foundation.h>
#import <IOUSBHost/IOUSBHost.h>
#import <IOKit/usb/USB.h>

int main(void) {
  @autoreleasepool {
    NSDictionary *match = @{
      @"IOProviderClass": @"IOUSBHostInterface",
      @"idVendor": @2276, @"idProduct": @350,
      @"bInterfaceNumber": @0,
    };
    // IOUSBHostInterface matching dict
    CFMutableDictionaryRef m = IOServiceMatching("IOUSBHostInterface");
    io_iterator_t it = IO_OBJECT_NULL;
    io_service_t svc = IO_OBJECT_NULL;
    if (IOServiceGetMatchingServices(kIOMainPortDefault, m, &it) == KERN_SUCCESS) {
      io_service_t s;
      while ((s = IOIteratorNext(it))) {
        NSNumber *vid = CFBridgingRelease(IORegistryEntryCreateCFProperty(s, CFSTR("idVendor"), NULL, 0));
        NSNumber *pid = CFBridgingRelease(IORegistryEntryCreateCFProperty(s, CFSTR("idProduct"), NULL, 0));
        NSNumber *ifn = CFBridgingRelease(IORegistryEntryCreateCFProperty(s, CFSTR("bInterfaceNumber"), NULL, 0));
        if (vid.intValue == 2276 && pid.intValue == 350 && ifn.intValue == 0) { svc = s; break; }
        IOObjectRelease(s);
      }
      IOObjectRelease(it);
    }
    if (!svc) { printf("interface 0 not found\n"); return 1; }
    NSError *err = nil;
    IOUSBHostInterface *intf = [[IOUSBHostInterface alloc] initWithIOService:svc options:0 queue:dispatch_queue_create("usb", NULL) error:&err interestHandler:nil];
    if (!intf) { printf("open interface failed: %s\n", err.description.UTF8String); return 1; }
    printf("interface 0 claimed OK\n");
    if (![intf selectAlternateSetting:1 error:&err]) {
      printf("selectAlt 1 failed: %s\n", err.description.UTF8String); return 1;
    }
    printf("alt setting 1 selected\n");

    // SET_CUR SAMPLING_FREQ_CONTROL 48000 on both endpoints
    uint8_t freq[3] = {0x80, 0xBB, 0x00};
    for (int ep = 0; ep < 2; ep++) {
      uint16_t wIndex = ep == 0 ? 0x0001 : 0x0082;
      NSMutableData *d = [NSMutableData dataWithBytes:freq length:3];
      IOUSBDeviceRequest req = { .bmRequestType = 0x22, .bRequest = 0x01, .wValue = 0x0100, .wIndex = wIndex, .wLength = 3 };
      NSInteger done = 0;
      if (![intf sendDeviceRequest:req data:d bytesTransferred:&done completionTimeout:1.0 error:&err]) {
        printf("SET_CUR ep %04x failed: %s\n", wIndex, err.description.UTF8String);
      } else {
        printf("SET_CUR 48000 on ep %04x ok (%ld bytes)\n", wIndex, (long)done);
      }
    }

    IOUSBHostPipe *inPipe = [intf copyPipeWithAddress:0x82 error:&err];
    if (!inPipe) { printf("copy IN pipe failed: %s\n", err.description.UTF8String); return 1; }
    printf("IN pipe 0x82 open\n");

    // read 8 x 1ms isoch frames, a few rounds
    const int NF = 8;
    NSMutableData *buf = [[intf ioDataWithCapacity:1024*NF error:&err] mutableCopy];
    if (!buf) buf = [NSMutableData dataWithLength:1024*NF];
    uint64_t frameNum = 0;
    for (int round = 0; round < 4; round++) {
      IOUSBHostIsochronousFrame frames[NF];
      memset(frames, 0, sizeof(frames));
      for (int i = 0; i < NF; i++) frames[i].requestCount = 1024;
      memset(buf.mutableBytes, 0, buf.length);
      if (![inPipe sendIORequestWithData:buf frameList:frames frameListCount:NF firstFrameNumber:0 error:&err]) {
        printf("isoch read failed: %s\n", err.description.UTF8String); return 1;
      }
      long total = 0, nonzero = 0;
      for (int i = 0; i < NF; i++) total += frames[i].completeCount;
      uint8_t *p = (uint8_t*)buf.bytes;
      for (long i = 0; i < total; i++) if (p[i]) nonzero++;
      printf("round %d: %ld bytes received, %ld non-zero", round, total, nonzero);
      if (total >= 18) {
        printf("  first frame: ");
        for (int i = 0; i < 18; i++) printf("%02x", p[i]);
      }
      printf("\n");
      (void)frameNum;
    }
  }
  return 0;
}
