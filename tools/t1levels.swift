// t1levels.swift -- liest einen CoreAudio-Eingang und zeigt Pegel je Kanal,
// danach je Stereopaar die dominante Frequenz und die Phasenlage L/R.
// Damit laesst sich unabhaengig von der DJ-Software pruefen, ob ein
// Timecode-Signal (ca. 1 kHz, 90 Grad zwischen L und R) am Rechner ankommt.
//
// Build: swiftc -O tools/t1levels.swift -o t1levels
// Run:   ./t1levels [Geraetename-Teil] [Sekunden]    (Standard: "DJM-T1", 6)
//
// Braucht die Mikrofon-Freigabe (TCC) fuer den startenden Prozess; ohne sie
// liefert CoreAudio Nullen. Das Programm gibt den Freigabestatus aus.
import CoreAudio
import AVFoundation
import Foundation

func prop(_ sel: AudioObjectPropertySelector, scope: AudioObjectPropertyScope = kAudioObjectPropertyScopeGlobal) -> AudioObjectPropertyAddress {
    AudioObjectPropertyAddress(mSelector: sel, mScope: scope, mElement: kAudioObjectPropertyElementMain)
}
func allDevices() -> [AudioDeviceID] {
    var a = prop(kAudioHardwarePropertyDevices)
    var size: UInt32 = 0
    AudioObjectGetPropertyDataSize(AudioObjectID(kAudioObjectSystemObject), &a, 0, nil, &size)
    var ids = [AudioDeviceID](repeating: 0, count: Int(size) / MemoryLayout<AudioDeviceID>.size)
    AudioObjectGetPropertyData(AudioObjectID(kAudioObjectSystemObject), &a, 0, nil, &size, &ids)
    return ids
}
func devName(_ id: AudioDeviceID) -> String {
    var a = prop(kAudioObjectPropertyName)
    var cf: Unmanaged<CFString>?
    var size = UInt32(MemoryLayout<Unmanaged<CFString>?>.size)
    AudioObjectGetPropertyData(id, &a, 0, nil, &size, &cf)
    return (cf?.takeRetainedValue() as String?) ?? "?"
}
func inChannels(_ id: AudioDeviceID) -> Int {
    var a = prop(kAudioDevicePropertyStreamConfiguration, scope: kAudioObjectPropertyScopeInput)
    var size: UInt32 = 0
    guard AudioObjectGetPropertyDataSize(id, &a, 0, nil, &size) == noErr, size > 0 else { return 0 }
    let raw = UnsafeMutableRawPointer.allocate(byteCount: Int(size), alignment: 16)
    defer { raw.deallocate() }
    AudioObjectGetPropertyData(id, &a, 0, nil, &size, raw)
    let list = UnsafeMutableAudioBufferListPointer(raw.assumingMemoryBound(to: AudioBufferList.self))
    return list.reduce(0) { $0 + Int($1.mNumberChannels) }
}
func sampleRate(_ id: AudioDeviceID) -> Double {
    var a = prop(kAudioDevicePropertyNominalSampleRate)
    var r: Float64 = 0; var size = UInt32(MemoryLayout<Float64>.size)
    AudioObjectGetPropertyData(id, &a, 0, nil, &size, &r)
    return r
}
func db(_ x: Double) -> String { x <= 1e-9 ? "  -inf" : String(format: "%6.1f", 20 * log10(x)) }

let needle = CommandLine.arguments.count > 1 ? CommandLine.arguments[1] : "DJM-T1"
let seconds = CommandLine.arguments.count > 2 ? Int(CommandLine.arguments[2]) ?? 6 : 6

let auth = AVCaptureDevice.authorizationStatus(for: .audio)
print("Mikrofon-Freigabe (TCC): \(auth.rawValue) (0=offen 1=eingeschraenkt 2=verweigert 3=erlaubt)")
if auth == .notDetermined {
    let sem = DispatchSemaphore(value: 0)
    AVCaptureDevice.requestAccess(for: .audio) { _ in sem.signal() }
    _ = sem.wait(timeout: .now() + 30)
}

guard let dev = allDevices().first(where: { devName($0).localizedCaseInsensitiveContains(needle) && inChannels($0) > 0 }) else {
    print("Kein Eingangsgeraet mit '\(needle)' gefunden. Vorhanden:")
    for d in allDevices() where inChannels(d) > 0 { print("  \(devName(d)) (\(inChannels(d)) ch)") }
    exit(1)
}
let nch = inChannels(dev)
let rate = sampleRate(dev)
print("Geraet: \(devName(dev)), \(nch) Eingangskanaele, \(rate) Hz, \(seconds) s")

var peak = [Float](repeating: 0, count: nch)
var sumSq = [Double](repeating: 0, count: nch)
var frames = 0
var tail = [[Float]](repeating: [], count: nch)   // letzte ~0,5 s je Kanal
let tailLen = Int(rate / 2)

var proc: AudioDeviceIOProcID?
let q = DispatchQueue(label: "io")
let st = AudioDeviceCreateIOProcIDWithBlock(&proc, dev, q) { _, inData, _, _, _ in
    let list = UnsafeMutableAudioBufferListPointer(UnsafeMutablePointer(mutating: inData))
    var base = 0
    for buf in list {
        let n = Int(buf.mNumberChannels)
        guard let p = buf.mData?.assumingMemoryBound(to: Float.self), n > 0 else { continue }
        let count = Int(buf.mDataByteSize) / MemoryLayout<Float>.size / n
        for c in 0..<n where base + c < nch {
            for f in 0..<count {
                let v = p[f * n + c]
                if abs(v) > peak[base + c] { peak[base + c] = abs(v) }
                sumSq[base + c] += Double(v * v)
                tail[base + c].append(v)
            }
            if tail[base + c].count > tailLen { tail[base + c].removeFirst(tail[base + c].count - tailLen) }
        }
        if base == 0 { frames += count }
        base += n
    }
}
guard st == noErr, let proc = proc, AudioDeviceStart(dev, proc) == noErr else { print("Start fehlgeschlagen"); exit(2) }

for s in 1...seconds {
    Thread.sleep(forTimeInterval: 1)
    let fr = Double(max(frames, 1))
    var line = String(format: "t=%2ds frames=%6d |", s, frames)
    for c in 0..<nch {
        line += " ch\(c) pk" + db(Double(peak[c])) + " rms" + db((sumSq[c] / fr).squareRoot()) + (c % 2 == 1 ? " |" : "")
    }
    print(line)
    peak = [Float](repeating: 0, count: nch); sumSq = [Double](repeating: 0, count: nch); frames = 0
}
AudioDeviceStop(dev, proc)

func dft(_ x: [Float], _ f: Double) -> (re: Double, im: Double) {
    var re = 0.0, im = 0.0
    let w = 2.0 * Double.pi * f / rate
    for i in 0..<x.count { let a = w * Double(i); re += Double(x[i]) * cos(a); im -= Double(x[i]) * sin(a) }
    return (re, im)
}
func mag(_ d: (re: Double, im: Double)) -> Double { (d.re * d.re + d.im * d.im).squareRoot() }

print("\nTonanalyse der letzten ~0,5 s je Stereopaar:")
for pair in 0..<(nch / 2) {
    let l = tail[pair * 2], r = tail[pair * 2 + 1]
    guard l.count > 1000, r.count == l.count else { continue }
    var bestF = 0.0, bestA = 0.0
    var f = 200.0
    while f <= 6000 { let a = mag(dft(l, f)); if a > bestA { bestA = a; bestF = f }; f += 25 }
    for ff in stride(from: bestF - 25, through: bestF + 25, by: 1.0) { let a = mag(dft(l, ff)); if a > bestA { bestA = a; bestF = ff } }
    let dl = dft(l, bestF), dr = dft(r, bestF)
    let n = Double(l.count)
    let al = mag(dl) * 2 / n, ar = mag(dr) * 2 / n
    var ph = (atan2(dr.im, dr.re) - atan2(dl.im, dl.re)) * 180 / Double.pi
    while ph > 180 { ph -= 360 }; while ph < -180 { ph += 360 }
    let energy = l.reduce(0.0) { $0 + Double($1 * $1) } / n
    print(String(format: "Paar %d (ch%d/ch%d): %.0f Hz | Ton L %@ dBFS, R %@ dBFS | Phase R gegen L %.0f Grad | Tonanteil %.0f %%",
                 pair, pair * 2, pair * 2 + 1, bestF, db(al), db(ar), ph, energy > 0 ? 100 * (al * al / 2) / energy : 0))
}
