# DJ Legacy Audio Drivers for macOS 26

Modern AudioDriverKit drivers (DriverKit system extension) for discontinued
DVS audio interfaces whose vendor drivers no longer load on current macOS:

- **Pioneer DJM-T1** (implemented) — replaces Pioneer's last official driver
  (kext v1.2.0 from 2015, x86_64 only, not loadable on macOS 26 / Apple
  Silicon)
- **Rane SL2** (planned) — replaces Rane's Core Audio driver (last supported
  on macOS 10.15)

## Hardware contract (reverse-engineered & verified on the device)

Extracted from the legacy kext (`DJM02*` classes — a fork of Apple's
AppleUSBAudio with a hardcoded configuration) and confirmed live against the
mixer with a user-space prototype (`tools/t1probe.m`, IOUSBHost):

| Property | Value |
|---|---|
| USB | VID 0x08E4 (2276), PID 0x015E (350) |
| Audio interface | Interface 0 (vendor-specific, class 0xFF), alternate setting 1 |
| Endpoints | EP 0x01 OUT / EP 0x82 IN, isochronous asynchronous, 1024 B, 1 ms |
| Format | fixed 48 kHz, 24-bit packed LE (3-byte subframes) |
| Channels | 6 in + 6 out (3 stereo pairs each way) → 864 B per USB frame |
| Rate setting | standard UAC1 `SET_CUR SAMPLING_FREQ_CONTROL` (0x22/0x01/0x0100) on both endpoints |

The T1's remaining interfaces (1+2 = USB MIDI, 3 = HID) are class compliant
and already work without a driver.

## Project layout

- `Driver/` – dext (`de.cypher.djmt1.audio`): `DJMT1Driver` (IOUserAudioDriver,
  matching + publishing), `DJMT1Device` (IOUserAudioDevice + isochronous engine
  built on USBDriverKit `IsochIO`)
- `App/` – `DJMT1Installer` (`de.cypher.djmt1`): activates/deactivates the
  system extension
- `tools/t1probe.m` – user-space prototype to verify the USB protocol (runs
  without any entitlements): `clang -o t1probe tools/t1probe.m -framework
  Foundation -framework IOUSBHost -framework IOKit -fobjc-arc`

## Building

```
xcodegen generate
xcodebuild -project DJMT1Driver.xcodeproj -target DJMT1Installer -configuration Debug build
```

## Running – entitlements required

The dext needs `com.apple.developer.driverkit`,
`…driverkit.family.audio` and `…driverkit.transport.usb` (VID 2276).
These must be requested from Apple once (team DBCZNZ63SD):
<https://developer.apple.com/contact/request/system-extension/> —
rationale: independent audio driver for existing hardware (Pioneer DJM-T1)
whose vendor driver has been discontinued.

Until the entitlements are granted, local development works via developer
mode (requires adjusting SIP from recoveryOS):

```
systemextensionsctl developer on
```

Then: copy the app to `/Applications`, launch it, click "Aktivieren", and
approve the extension in System Settings → Privacy & Security.

## Status / open items

- [x] Hardware protocol reverse-engineered and verified live
- [x] Dext + installer app build (universal arm64/x86_64)
- [ ] File the entitlement request with Apple
- [ ] First load test on the device (activation, matching, publishing to CoreAudio)
- [ ] Streaming test: check input channels in DeckLab / Audio MIDI Setup
- [ ] Clock fine-tuning: zero timestamps are derived from the USB frame
  timestamps of the IN stream (v0, per-frame granularity); consider
  smoothing/PLL
- [ ] Channel names/layout (map CH1/CH2/AUX assignments on the device)
- [ ] Volume/mute controls if the device supports them (the legacy driver
  reported none)

## Rane SL2 (planned)

Full hardware contract is already known — no reverse engineering needed. The
SL2 (VID 0x1CC5, PID 0x0013) is a nearly textbook **USB Audio Class 2.0**
device with valid class-specific descriptors (`bInterfaceProtocol =
UAC_VERSION_2`); it is only rejected by class drivers because it reports
vendor-specific interface classes (0xFF) and omits the interface association
descriptor. Documented by the Linux quirk in
[Reinharderino/rane-sl2-linux](https://github.com/Reinharderino/rane-sl2-linux)
(full `lsusb -v` dump and ALSA quirks-table patch):

| Property | Value |
|---|---|
| USB | VID 0x1CC5 (7365), PID 0x0013 (19), high speed |
| Interfaces | 0 = audio control (UAC2, vendor-coded), 1 alt 1 = OUT stream, 2 alt 1 = IN stream, 3 = HID (leave to the system) |
| Endpoints | EP 0x06 OUT / EP 0x82 IN (implicit feedback), isochronous asynchronous, 112 B, bInterval 1 (125 µs microframes) |
| Format | fixed 44.1 kHz, 24-bit in 4-byte subslots (S32_LE containers), 4 in + 4 out |
| Rate setting | **none** — the device stalls both `UAC2_CS_CUR` and `UAC2_CS_RANGE`; never send sample-rate requests |
| Clocking | OUT is paced by implicit feedback from the IN endpoint |
| Known quirk | slow enumeration on some xHCI hosts (device wakes slower than the host waits) |

Differences from the DJM-T1 engine: microframe pacing (125 µs instead of
1 ms), variable packet sizes (5/6 samples ≙ 80/96 B at 44.1 kHz), implicit
feedback pacing for the OUT stream, and two separate streaming interfaces to
claim instead of one. `tools/sl2probe.m` verifies the contract from macOS
user space once an SL2 is connected. The USB transport entitlement in
`Driver/DJMT1AudioDriver.entitlements` already includes both device IDs.

## Related work

[yuki-ama/djm-t1-driver](https://github.com/yuki-ama/djm-t1-driver) (MIT) —
independent user-space implementation (libusb bridge daemon + Core Audio
AudioServerPlugin, ~24 ms round trip). Confirms the same hardware contract;
usable today without entitlements. This project aims at the system-native
DriverKit path instead: no root daemon, no shared-memory hop, lower latency
potential.
