# DJM-T1 Audio Driver for macOS 26

Modern AudioDriverKit driver (DriverKit system extension) for the Pioneer
DJM-T1, replacing Pioneer's last official driver (kext v1.2.0 from 2015,
x86_64 only, no longer loadable on macOS 26 / Apple Silicon).

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

## Related work

[yuki-ama/djm-t1-driver](https://github.com/yuki-ama/djm-t1-driver) (MIT) —
independent user-space implementation (libusb bridge daemon + Core Audio
AudioServerPlugin, ~24 ms round trip). Confirms the same hardware contract;
usable today without entitlements. This project aims at the system-native
DriverKit path instead: no root daemon, no shared-memory hop, lower latency
potential.
