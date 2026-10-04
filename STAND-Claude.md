# Stand: DJM-T1 / Rane-SL2-Treiber (04.10.2026)

Laufende Stand-Notiz, liest jede Claude-Sitzung in diesem Ordner zuerst.
Enthält nur Belegtes; offene Punkte sind als offen markiert.

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

**Version 1.0.2 (18)** ist installiert und aktiv und die aktuelle Fassung im Repo
(`systemextensionsctl list`: `[activated enabled]`).

### Seit 21.09. (alles am Gerät gemessen, macOS 27)

- **Läuft auch unter macOS 27** (ab v10 mit Entitlement nur für 2276;
  ein Entitlement mit `[2276, 7365]` lehnt AMFI ab, solange Apple 7365
  nicht im Profil freigegeben hat). Audio und MIDI am T1 vom Inhaber
  bestätigt.
- **Prozess-Leck behoben (v12).** `IONewZero`/`IOSafeDeleteNULL` rufen
  weder Konstruktoren noch Destruktoren auf; die `OSSharedPtr`-Member von
  `DJMT1Device`/`SL2Device` blieben darum nach dem Abziehen referenziert,
  der Dext-Prozess lebte weiter und der Versionswechsel hing
  („terminating for upgrade"). `free()` setzt sie jetzt von Hand zurück.
  Geprüft: Prozess endet nach dem Abziehen, v12→v13 ohne Neustart.
  (Damit ist der Neustart-Hinweis unter „Gelernt" nur noch für ältere
  Fassungen nötig.)
- **Statistik im Log (v13):** Ein-/Ausgangs-Streams loggen Zähler und
  Spitzenpegel (alle 125 Completions bei Auffälligkeit, sonst jede
  zehnte).
- **DVS kam nicht in aDJusted an — Ursache gefunden.** Das T1 schickt
  Timecode (CH1/CH2 „USB"-Schalter) nur dann auf USB 1/2 bzw. 3/4, wenn
  das Routing vorher per USB-Vendor-Request gesetzt wurde; Pioneers
  „DJM-T1 Setting Utility"/AutoSetup macht das beim Anstecken. Ohne
  Pioneer-Software bleiben die Kanäle stumm (digitale Nullen).
  Aus `PioneerDJMSetup.framework` (nur gelesen, nie ausgeführt) gewonnen:
  `0x40/0x03`, wIndex `0x8002`, wValue 0x1103 (USB 1/2 ← CH1 Timecode
  PHONO), 0x2203 (USB 3/4 ← CH2 Timecode PHONO); Lesen des Schalters:
  `0xC0/0x00`, wIndex `0x8002`, 3 Byte (`00 02 01` = CH1 USB, CH2 anderes).
  Mit diesen Befehlen kam der Timecode (998 Hz bei 33 U/min, ca. -20 dBFS,
  91° Phase) ohne Pioneer-Software in aDJusted an.
- **Das Routing überlebt keinen Mixer-Neustart** (gemessen 04.10.: nach
  Power-Cycle Rauschen um -95 dBFS statt Timecode). Darum setzt der Treiber
  es ab v14 bei jedem Anstecken selbst (`ApplyT1Routing` in
  `Start_Impl`). Verifiziert 04.10.: beide Requests liefern `0x0`, Timecode
  liegt ohne Zusatzwerkzeug auf USB 1/2 (-18,6 dBFS).
- **Nicht dekodiert:** USB-Ausgangspegel (`0x40/0x03`, wIndex `0x8003`,
  Wertbedeutung offen), MIDI-Kanal/Tastenmodus aus AutoSetup;
  CD-Timecode (CDJ) wird nicht gesetzt.
- Ein „langsamer Tune" war kein Treiberfehler, aDJusted stand auf 45 U/min
  bei einer 33er Platte.

**Testfassung liegt bereit:** `~/Dropbox/AudioDriver/DJMT1Treiber-1.0.2-17.dmg`
(04.10.) — App + Programme-Verknüpfung + `Anleitung.txt`, DMG selbst
signiert, notariert und gestapelt; Treiberparameter wie v15 (Version 17,
weil v16 schon vergeben war). App und Dext sind Universal-Binaries
(`x86_64 arm64`), gebaut für macOS 14+. Der Tester nutzt Traktor auf einem
Intel-Mac (aDJusted läuft dort nicht), darum steht in der Anleitung nichts
zu aDJusted; Intel ist **noch nie ausprobiert**, das sagt die Anleitung
offen. Nur für Pioneer freigegeben. Die alte Fassung (8) ist aus Dropbox
gelöscht. Gebaute DMG/ZIPs liegen in `build/` (gitignored).

**Strom-Abbruch behoben (v18, 04.10.).** Mit v17 aus der DMG fiel um
17:54:35 der Datenstrom aus: bei einem kurzen Aussetzer des Dext
(Systemlast) lag die nächste Frame-Nummer in der Vergangenheit, jeder
Neuversuch von `IsochIO` schlug mit `kIOReturnIsoTooOld` (`0xe00002ee`)
fehl, der Treiber setzte die Frame-Nummer nie neu, der Strom blieb tot bis
zum Neuanstecken. aDJusted zeigte weiter ein eingefrorenes Signal (CoreAudio
las den Ring zyklisch weiter, Platte wirkungslos), im Mixer kam nichts an.
Behoben: Bei `IsoTooOld` wird auf aktuellen USB-Frame + Vorlauf neu
angesetzt (bis 4 Versuche, Log `input/output stream late … re-anchored`),
verlorene Eingangs-Frames werden als Stille eingetragen, 6 statt 4 Blöcke
im Voraus (10 ms statt 6 ms Reserve). Kosten: Round-Trip bei 128 Frames
26,7 ms statt 21,3 ms, bei 512 Frames 42,7 ms. Geprüft: Installation, Routing
`0x0`, Timecode 1001 Hz/87 %, CPU-Last (2× Kerne) ohne Auffälligkeit. Der
Wiederanlauf selbst wurde noch **nie ausgelöst**, nur kompiliert. Zwei
Dinge, die ich über die Fehlercodes gelernt habe: `0xe00002eb` =
Aborted/`0xe00002d8` = NotReady beim Abziehen sind normal, `0xe00002ee`
im laufenden Betrieb ist der Fehlerfall. Der Tester hat v17 per Mail
bekommen (hat den Fehler).

**Kopfhörer-Cue am T1:** Mit dem Treiber hat es nichts zu tun. Zuerst
schien Cue nicht zu gehen (Tonprobe über `t1tone`: Ton kam an CH1 und Master
an, im Kopfhörer nicht), die Ursache war ein verwechselter Pegelregler am
Mixer, danach ging Cue. aDJusted im Modus „Extern“ überlässt Cue dem
Hardware-Mixer (laut seinem Hilfetext), Pioneers Setup-Software hat keine
Cue-/Kopfhörer-Einstellung.

**v16-Versuch verworfen (04.10.):** USB-Blöcke 1 ms, 5 im Voraus, Safety
Aus 7 ms / Ein 4 ms. Gemessener App-Round-Trip war identisch zu v15
(128 Frames: 21,3 ms bei beiden); die Gerätezeit-Differenz stieg um genau
die eingesparten Millisekunden. Die Untergrenze von rund 16 ms (plus zwei
Puffer) kommt also nicht aus den Safety-Offsets, die Quelle ist ungeklärt.
Nicht committet, v15-Parameter bleiben. Aufklären ginge nur mit
Variantenbauten (z. B. nur `kOutAhead` ändern).

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
  hochgeladen, Antwort an Martin ist raus (18.09.). Update 21.09.: Apple
  bestätigt den Auftrag korrekt („Driver 0x1CC5 (Rane)" zur „DriverKit USB
  Transport - VendorID"-Capability hinzufügen), prüft noch intern — noch
  keine Zusage, noch nicht erledigt.
- **Rane SL2 komplett ungetestet** (siehe oben).
- **Seltener Signalausfall** ca. 90 s nach Engine-Start (digitale Nullen
  auf CoreAudio-Ebene) trat vor v13 auf; mit v13/v14 und Statistik-Logs
  nicht reproduziert, weiter beobachten.
- `tools/midisniff.swift` liegt ungeprüft und uncommittet im Verzeichnis.
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
- `t1levels.swift`: Pegel je Eingangskanal und Tonanalyse (Frequenz,
  Phase) je Stereopaar über CoreAudio. `t1levels DJM-T1 <Sekunden>`.
- `t1route.c`: liest den Schalter-Status des T1 und setzt das Routing mit
  den Pioneer-Requests (`status`, `set <Paar> <Option>`, `tc-phono`).
- `t1rtt.swift`: Round-Trip-Latenz über den Mixer (Ton auf CH1 aus, über
  USB 5/6 wieder ein); gibt hörbare Töne aus.
- `t1tone.swift`: Dauerton auf einem Ausgangspaar (0–2) zum Abhören, was
  der Mixer damit macht.
- `usbcfgdump.c`, `sl2_diag.sh`: SL2-Vorbereitung für den Testtag.
- `midisniff.swift`: unverändert, ungeprüft.

## Build-Ablauf (Release)

Unverändert gegenüber der letzten Notiz — siehe README. Kurzfassung:
`xcodegen generate` → `xcodebuild archive` → `ApplicationProperties` in
der Archiv-Info.plist ergänzen → `Products/System` löschen →
`-exportArchive` mit Developer-ID → `notarytool submit --wait` →
`stapler staple` → nach `/Applications` kopieren →
`DJMT1Installer --activate`. Jede neue Fassung braucht eine höhere
`CURRENT_PROJECT_VERSION`/`CFBundleVersion` (aktuell 18).

## Nächste Schritte

1. Prüfen, ob noch ein Restzittern bleibt (Hörtest in aDJusted, ggf.
   kleinerer Puffer); SL2Device auf dasselbe Zeitmodell bringen, sobald
   ein Gerät da ist.
2. Tester (Traktor, Intel-Mac) auf DMG 1.0.2 (18) umstellen; Rückmeldung
   abwarten.
3. Bei Apple `idVendor 7365` (Rane) weiter nachhalten.
4. Rane SL2: `sl2probe` am Gerät fahren, danach erst dem `SL2Device`-Code
   vertrauen.
5. `tools/midisniff.swift` prüfen und ggf. committen.

## Rund um die Sitzung

- **GitHub:** Gepusht bis `c8e2593`; `b032074`, `2ed7a86`, `96489f3` und
  der v14-Commit liegen lokal (Push nur auf Anweisung). Der Push lief über den vorhandenen Credential-Helper von
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
