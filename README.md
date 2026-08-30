# DJM-T1 Audio-Treiber für macOS 26

Moderner AudioDriverKit-Treiber (DriverKit-System-Extension) für den Pioneer
DJM-T1, als Ersatz für Pioneers letzten offiziellen Treiber (Kext v1.2.0 von
2015, x86_64-only, auf macOS 26 / Apple Silicon nicht mehr ladbar).

## Hardware-Vertrag (reverse-engineert & am Gerät verifiziert)

Aus dem Legacy-Kext (`DJM02*`-Klassen, ein Fork von Apples AppleUSBAudio mit
hartkodierter Konfiguration) extrahiert und per User-Space-Prototyp
(`tools/t1probe.m`, IOUSBHost) live am Mixer bestätigt:

| Eigenschaft | Wert |
|---|---|
| USB | VID 0x08E4 (2276), PID 0x015E (350) |
| Audio-Interface | Interface 0 (vendor-specific, Klasse 0xFF), Alt-Setting 1 |
| Endpoints | EP 0x01 OUT / EP 0x82 IN, isochron asynchron, 1024 B, 1 ms |
| Format | fest 48 kHz, 24-bit packed LE (3-Byte-Subframes) |
| Kanäle | 6 rein + 6 raus (je 3 Stereopaare) → 864 B pro USB-Frame |
| Rate setzen | Standard-UAC1 `SET_CUR SAMPLING_FREQ_CONTROL` (0x22/0x01/0x0100) auf beide Endpoints |

Die übrigen Interfaces des T1 (1+2 = USB-MIDI, 3 = HID) sind class-compliant
und laufen bereits ohne Treiber.

## Projektstruktur

- `Driver/` – Dext (`de.cypher.djmt1.audio`): `DJMT1Driver` (IOUserAudioDriver,
  Matching + Publish), `DJMT1Device` (IOUserAudioDevice + Isochron-Engine über
  USBDriverKit `IsochIO`)
- `App/` – `DJMT1Installer` (`de.cypher.djmt1`): aktiviert/deaktiviert die
  System-Extension
- `tools/t1probe.m` – User-Space-Prototyp zum Verifizieren des USB-Protokolls
  (ohne Entitlements lauffähig): `clang -o t1probe tools/t1probe.m -framework
  Foundation -framework IOUSBHost -framework IOKit -fobjc-arc`

## Bauen

```
xcodegen generate
xcodebuild -project DJMT1Driver.xcodeproj -target DJMT1Installer -configuration Debug build
```

## Ausführen – Entitlements nötig

Der Dext braucht `com.apple.developer.driverkit`,
`…driverkit.family.audio` und `…driverkit.transport.usb` (VID 2276).
Diese müssen einmalig bei Apple beantragt werden (Team DBCZNZ63SD):
<https://developer.apple.com/contact/request/system-extension/> —
Begründung: eigener Audiotreiber für ein Bestandsgerät (Pioneer DJM-T1),
dessen Herstellertreiber eingestellt wurde.

Bis die Entitlements genehmigt sind, geht Entwicklung lokal über den
Developer-Modus (SIP-Anpassung in der Recovery nötig):

```
systemextensionsctl developer on
```

Danach: App nach `/Applications` kopieren, starten, „Treiber aktivieren",
Freigabe in Systemeinstellungen → Datenschutz & Sicherheit erteilen.

## Status / offene Punkte

- [x] Hardware-Protokoll reverse-engineert und live verifiziert
- [x] Dext + Installer-App bauen (Universal arm64/x86_64)
- [ ] Entitlement-Antrag bei Apple stellen
- [ ] Erster Ladetest auf dem Gerät (Aktivierung, Matching, Publish in CoreAudio)
- [ ] Streaming-Test: Input-Kanäle in DeckLab/Audio-MIDI-Setup prüfen
- [ ] Clock-Feintuning: Zero-Timestamps werden aus den USB-Frame-Timestamps
  des IN-Streams abgeleitet (v0, per-Frame-Granularität); ggf. Glättung/PLL
- [ ] Kanalnamen/Layout (CH1/CH2/AUX-Zuordnung am Gerät ermitteln)
- [ ] Lautstärke-/Mute-Controls falls das Gerät sie unterstützt (der alte
  Treiber meldete keine)
