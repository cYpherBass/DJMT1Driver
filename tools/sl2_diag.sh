#!/bin/zsh
# sl2_diag.sh -- sammelt alles, was zur Rane SL2 (1cc5:0013) gebraucht wird, in
# eine Textdatei auf dem Schreibtisch. Nur lesend, aendert nichts am System.
#
# Aufruf (SL2 angesteckt):  tools/sl2_diag.sh [Pfad/zu/usbcfgdump]
# Die Textdatei danach einfach hierher geben.

setopt null_glob   # kein Fehler, wenn es (noch) keine Absturzberichte gibt
OUT=~/Desktop/sl2-diag-$(date +%Y%m%d-%H%M%S).txt
CFGDUMP=${1:-$(command -v usbcfgdump || echo ./usbcfgdump)}

section() { printf '\n\n===== %s =====\n' "$1" >> "$OUT"; }

: > "$OUT"
section "Zeit / System"
{ date; sw_vers; uname -m; } >> "$OUT" 2>&1

section "Systemerweiterungen (DJM-T1 Audio muss 'activated enabled' sein)"
systemextensionsctl list >> "$OUT" 2>&1

section "USB: Rane-Geraet (Vendor 0x1cc5) gesehen?"
ioreg -p IOUSB -l -w0 2>&1 | grep -i -B2 -A25 -E '"idVendor" = 7365|rane' >> "$OUT"

section "Wer haelt die SL2-Interfaces? (IOService-Baum um 1cc5)"
ioreg -p IOService -l -w0 2>&1 | grep -i -B3 -A12 -E '"idVendor" = 7365' >> "$OUT"

section "Rohe Konfigurationsdeskriptoren (usbcfgdump 7365 19)"
if [ -x "$CFGDUMP" ]; then "$CFGDUMP" 7365 19 >> "$OUT" 2>&1; else echo "usbcfgdump nicht gefunden: $CFGDUMP" >> "$OUT"; fi

section "Laeuft der Treiberprozess?"
pgrep -fl djmt1 >> "$OUT" 2>&1 || echo "(kein djmt1-Prozess)" >> "$OUT"

section "Log der letzten 15 Minuten: Treiber, SL2, Signatur/AMFI"
/usr/bin/log show --last 15m --predicate \
  'eventMessage CONTAINS[c] "djmt1" OR eventMessage CONTAINS "SL2Device" OR eventMessage CONTAINS "DJMT1Driver" OR eventMessage CONTAINS[c] "1cc5" OR eventMessage CONTAINS "load code signature" OR eventMessage CONTAINS "Unsatisfied Entitlements" OR eventMessage CONTAINS "No matching profile"' \
  >> "$OUT" 2>&1

section "Absturzberichte des Treibers (neueste zuerst)"
ls -t /Library/Logs/DiagnosticReports/ 2>/dev/null | grep -i djmt1 | head -3 >> "$OUT"
for f in $(ls -t /Library/Logs/DiagnosticReports/*djmt1* 2>/dev/null | head -1); do head -c 6000 "$f" >> "$OUT"; done

section "CoreAudio: sieht macOS die SL2 als Audiogeraet?"
system_profiler SPAudioDataType 2>&1 | grep -i -B2 -A10 rane >> "$OUT" || echo "(keine Rane-Audio-Eintraege)" >> "$OUT"

echo "Fertig: $OUT"
