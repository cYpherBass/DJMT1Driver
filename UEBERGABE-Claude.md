# Übergabe: DJM-T1 / Rane-SL2-Treiber (Stand 18.09.2026)

Übergabenotiz für eine neue Claude-Sitzung. Enthält nur Belegtes; offene
Punkte sind als offen markiert.

## Ziel

AudioDriverKit-System-Extension (Dext) für den Pioneer DJM-T1 (und später die
Rane SL2) unter macOS 26, als Ersatz für Pioneers x86_64-Kext von 2015.
Repo: https://github.com/cYpherBass/DJMT1Driver (öffentlich).

## Stand

**Der DJM-T1 läuft — mit echtem Ton.** Am Gerät verifiziert (18.09.):
Treiber lädt, das Gerät erscheint in CoreAudio als „Pioneer DJM-T1" (6 in
/ 6 out, 48 kHz, korrekter Name), die isochronen USB-Transfers laufen
durchgehend ohne Abbruch (Zero-Timestamp-Updates alle ~256 ms, minutenlang
beobachtet, kein Timeout mehr) — und der Inhaber hat in aDJusted
tatsächlich Musikstücke durch den T1 abgespielt und gehört. MIDI (Play/
Pause, Cue-Punkte) funktioniert ebenfalls, lief aber schon vorher
unabhängig vom Audiotreiber (class-compliant USB-MIDI, kein eigener
Treiber nötig).

Drei echte Fehler haben das bis dahin verhindert, der Reihe nach gefunden
und behoben (Commits `93fba6c`, `17ae619`):

1. **Absturz beim Laden.** `DJMT1Device` hatte keine `init()`-Überschreibung,
   die `ivars` anlegt — der erste Schreibzugriff darauf hat den
   Treiberprozess mit `EXC_BAD_ACCESS` abgeschossen. Sichtbar im Log als
   `DK: ...::start(...) fail` plus Absturzbericht in
   `/Library/Logs/DiagnosticReports/`.
2. **Kein Anzeigename.** `DJMT1Device` hat nie `SetName()` auf sich selbst
   aufgerufen; `SetName()` in `DJMT1Driver::Start_Impl()` setzt nur den
   Namen des Treiber-Service, nicht das Audiogerät-Objekt.
3. **Der eigentliche Blocker:** Die isochronen USB-Completion-Callbacks
   (`HandleIsochInComplete`/`HandleIsochOutComplete`, `TYPE(IOUSBHostPipe::
   CompleteAsyncIsochIO)`) waren auf `DJMT1Device` deklariert. `DJMT1Device`
   ist kein `IOService` (nur `DJMT1Driver` ist das) und hat keine von
   DriverKit tatsächlich bediente Dispatch-Queue. `IsochIO()` hat immer
   `kIOReturnSuccess` zurückgegeben, aber `CompleteAsyncIsochIO` ist **nie**
   gefeuert — auch nicht nach zehn Sekunden, unabhängig von der
   Vorlaufzeit beim Scheduling (4 ms und 50 ms wurden beide getestet,
   beide erfolglos). Ein eigenständiges User-Space-Tool
   (`tools/t1probe.m`, synchrone `IOUSBHost`-API statt der asynchronen
   DriverKit-API) hat mit denselben Endpunkten echte Audiodaten gelesen —
   das hat Hardware und USB-Protokoll als Ursache ausgeschlossen und auf
   die Completion-Zustellung eingegrenzt. Fix: Die Callbacks liegen jetzt
   auf `DJMT1Driver` (echter `IOService`) und reichen an
   `DJMT1Device::OnIsochInComplete`/`OnIsochOutComplete` per normalem
   Methodenaufruf weiter.

**Version 1.0.2 (8)** ist installiert und aktiv
(`systemextensionsctl list`: `[activated enabled]`).

**Testfassung liegt bereit:** `~/Dropbox/AudioDriver/DJMT1Treiber-1.0.2-8.dmg`
— App + Programme-Verknüpfung + kurze Anleitung, DMG selbst (nicht nur die
App drin) signiert, notariert und gestapelt. Nur für Pioneer freigegeben,
das steht auch in der Anleitung. Eine reine Zip-Fassung
(`build/DJMT1Installer-1.0.2-8.zip`, gitignored) liegt zusätzlich lokal,
aber nicht in Dropbox — auf Anweisung so gelassen.

**Rane SL2:** Dieselben drei Fehler waren identisch in `SL2Device` und sind
nach derselben Begründung ebenfalls behoben (Commit `17ae619`) — aber
**ungetestet**, es liegt kein Rane-Gerät vor. Vor dem nächsten Kontakt mit
dem Gerät zuerst `tools/sl2probe.m` laufen lassen (README).

## Offen

- **Rane-Entitlement fehlt weiterhin, aber in Bearbeitung.** `idVendor 7365`
  ist bei Apple noch nicht genehmigt; `Driver/DJMT1AudioDriver.entitlements`
  enthält nur 2276. Apple Developer Support (Case 102965305978, Kontakt
  Martin) hat am 18.09. mitgeteilt: die zweite Vendor-ID lässt sich nicht
  selbst im Certificates-Portal eintragen (bestätigt: bei „DriverKit USB
  Transport - VendorID" gibt es dort keinen „Configure"-Knopf wie bei
  anderen Capabilities), Apple muss sie manuell für Eskalation ans
  Ops-Team ergänzen. Screenshot davon ist über Apples sicheren Upload-Link
  hochgeladen, Antwort an Martin ist raus (18.09.) — wartet auf
  Rückmeldung.
- **Rane SL2 komplett ungetestet** (siehe oben).
- Zwei neue Tools liegen unangetastet und ungetestet im Arbeitsverzeichnis,
  noch nicht committet: `tools/midisniff.swift`, `tools/usbcfgdump.c`.
- Noch nicht systematisch geprüft: Verhalten bei längerer Laufzeit
  (Stunden), Sample-Rate-Wechsel, Schlaf/Aufwachen des Macs, mehrfaches
  Ab-/Anstecken während des Betriebs.

## Gelernt (belegt, jeweils aus Fehler und Fix)

- **Ein Versionswechsel einer aktiven System-Extension kann hängen
  bleiben.** Wenn der alte Dext-Prozess gerade ein Gerät bedient
  (`systemextensionsctl list` zeigt dann `terminating for upgrade via
  delegate` oder `terminating for uninstall but still running`), reicht
  ein einfaches Aus-/Anstecken des USB-Geräts oft **nicht**, um die neue
  Version zu laden — der Kernel matched weiter auf die alte, jetzt
  kaputte Version. Ein Neustart löst es zuverlässig auf; das ist während
  dieser Sitzung mehrfach reproduziert worden.
- **`systemextensionsctl uninstall` braucht abgeschaltetes SIP** und
  funktioniert hier nicht. Zum Deaktivieren zum Testen (z. B. für
  `t1probe.m`) den „Treiber entfernen"-Knopf in der `DJMT1Installer`-App
  benutzen — das geht ohne SIP-Änderung, kann aber eine Freigabe in
  Systemeinstellungen → Allgemein → Anmeldeobjekte & Erweiterungen →
  Treiber-Erweiterungen verlangen.
- **`log` ist bei zsh ein Shell-Builtin** und schluckt `log show`-Aufrufe
  mit Fehler „too many arguments". Immer `/usr/bin/log` explizit
  aufrufen.
- Frühere Einträge (weiterhin gültig): Dext-Bundle muss wie seine
  Bundle-ID heißen; `CFBundlePackageType = DEXT` ist Pflicht; „Apple
  Development"-Signatur lädt nur mit `systemextensionsctl developer on`
  (SIP aus) — deshalb Developer ID + Notarisierung; gleiche
  `CFBundleVersion` wird nicht ersetzt; Xcode cached Profile, bei
  App-ID-Änderungen die Profile aus
  `~/Library/Developer/Xcode/UserData/Provisioning Profiles/` entfernen.

## Werkzeuge (`tools/`)

- `t1probe.m`: DJM-T1-Audio aus dem User-Space lesen. Hat diese Sitzung
  entscheidend weitergebracht (siehe oben) — liest über die synchrone
  `IOUSBHost`-API, unabhängig von der DriverKit-Completion-Kette des
  eigentlichen Treibers. Braucht keine Entitlements, geht aber nur, wenn
  der Dext das Interface nicht hält (siehe „Gelernt" oben zum
  Deaktivieren).
- `sl2probe.m`: dasselbe für die Rane SL2. Noch nie am Gerät gelaufen —
  das ist der zwingende nächste Schritt, bevor dem SL2-Code vertraut wird.
- `usbcfgdump.c`, `midisniff.swift`: unverändert seit der letzten Notiz,
  ungetestet in dieser Sitzung.

## Build-Ablauf (Release)

Unverändert gegenüber der letzten Notiz — siehe README. Kurzfassung:
`xcodegen generate` → `xcodebuild archive` → `ApplicationProperties` in
der Archiv-Info.plist ergänzen → `Products/System` löschen →
`-exportArchive` mit Developer-ID → `notarytool submit --wait` →
`stapler staple` → nach `/Applications` kopieren →
`DJMT1Installer --activate`. Jede neue Fassung braucht eine höhere
`CURRENT_PROJECT_VERSION`/`CFBundleVersion` (aktuell 8).

## Nächste Schritte

1. Bei Apple `idVendor 7365` (Rane) nachbeantragen.
2. Rane SL2: `sl2probe` am Gerät fahren, danach erst dem `SL2Device`-Code
   vertrauen.
3. `tools/midisniff.swift` und `tools/usbcfgdump.c` prüfen und ggf.
   committen.

## Rund um die Sitzung

- **GitHub:** Alle vier Commits dieser Sitzung sind gepusht (bis `1873d56`,
  auf Anweisung). Der Push lief über den vorhandenen Credential-Helper von
  `git` (`https://cYpherBass@github.com/...`), nicht über `gh` — `gh` ist
  weiterhin nicht angemeldet, das Token dafür ist weiterhin weg.
- **Git-Autor in diesem Repo:** `cYpherBass
  <93675118+cYpherBass@users.noreply.github.com>` (repo-lokal
  konfiguriert, unverändert).
- Diese Sitzung hat ausschließlich an `DJMT1Driver` gearbeitet, keine
  Änderungen an DeckLab oder anderen Repos.

## Regeln des Inhabers

Autor nur cYpherBass ohne Co-Authored-By. Keine Annahmen ohne Beleg. Nichts
pushen oder installieren ohne Anweisung.

Hinweis: Das Repo ist öffentlich. Vor einem Push entscheidet der Inhaber, ob
diese Notiz mit hinein soll.
