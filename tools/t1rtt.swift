// t1rtt.swift -- misst die Round-Trip-Latenz eines CoreAudio-Geraets ueber den
// Mixer: gibt kurze 1-kHz-Toene auf den Ausgangskanaelen 0/1 aus und sucht,
// wann sie auf einem Eingangskanal wieder ankommen. Beim DJM-T1: CH1-Eingangs-
// schalter auf USB, CH1-Fader auf, USB 5/6 = Post CH1 Fader; dann kommt der
// Ton auf Eingangskanal 4/5 zurueck.
//
// Build: swiftc -O tools/t1rtt.swift -o t1rtt
// Run:   ./t1rtt [Geraetename-Teil] [Rueckkanal (Standard 4)] [Amplitude (0.05)] [Puffer in Frames]
//
// Zwei Werte: "Geraetezeit" ist die Differenz der Sample-Zeitstempel zwischen
// Ausgabe und Rueckkehr (ohne Puffer des Audiosystems), "App" ist die Zeit
// zwischen dem Callback, der den Ton schreibt, und dem Callback, der ihn im
// Eingang sieht (mit Puffern; auf etwa einen Puffer genau).
//
// ACHTUNG: gibt hoerbaren Ton aus (Standard -26 dBFS, 5 ms, 8 Mal). Vorher
// Master/Booth leise stellen und die DJ-Software-Engine stoppen.
import CoreAudio
import AVFoundation
import Foundation

func prop(_ sel: AudioObjectPropertySelector, _ scope: AudioObjectPropertyScope = kAudioObjectPropertyScopeGlobal) -> AudioObjectPropertyAddress {
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
func channels(_ id: AudioDeviceID, _ scope: AudioObjectPropertyScope) -> Int {
    var a = prop(kAudioDevicePropertyStreamConfiguration, scope)
    var size: UInt32 = 0
    guard AudioObjectGetPropertyDataSize(id, &a, 0, nil, &size) == noErr, size > 0 else { return 0 }
    let raw = UnsafeMutableRawPointer.allocate(byteCount: Int(size), alignment: 16)
    defer { raw.deallocate() }
    AudioObjectGetPropertyData(id, &a, 0, nil, &size, raw)
    return UnsafeMutableAudioBufferListPointer(raw.assumingMemoryBound(to: AudioBufferList.self)).reduce(0) { $0 + Int($1.mNumberChannels) }
}
func u32(_ id: AudioDeviceID, _ sel: AudioObjectPropertySelector, _ scope: AudioObjectPropertyScope) -> UInt32 {
    var a = prop(sel, scope); var v: UInt32 = 0; var size = UInt32(4)
    AudioObjectGetPropertyData(id, &a, 0, nil, &size, &v); return v
}

setvbuf(stdout, nil, _IOLBF, 0)
let needle = CommandLine.arguments.count > 1 ? CommandLine.arguments[1] : "DJM-T1"
let retCh = CommandLine.arguments.count > 2 ? Int(CommandLine.arguments[2]) ?? 4 : 4
let amp = Float(CommandLine.arguments.count > 3 ? Double(CommandLine.arguments[3]) ?? 0.05 : 0.05)
let wantBuf = CommandLine.arguments.count > 4 ? UInt32(CommandLine.arguments[4]) : nil
var hostTB = mach_timebase_info_data_t(); mach_timebase_info(&hostTB)
func hostMs(_ t: UInt64) -> Double { Double(t) * Double(hostTB.numer) / Double(hostTB.denom) / 1e6 }

if AVCaptureDevice.authorizationStatus(for: .audio) == .notDetermined {
    let sem = DispatchSemaphore(value: 0)
    AVCaptureDevice.requestAccess(for: .audio) { _ in sem.signal() }
    _ = sem.wait(timeout: .now() + 30)
}
guard let dev = allDevices().first(where: { devName($0).localizedCaseInsensitiveContains(needle) && channels($0, kAudioObjectPropertyScopeInput) > 0 && channels($0, kAudioObjectPropertyScopeOutput) > 0 }) else {
    print("Kein Geraet mit Ein- und Ausgang und '\(needle)' gefunden"); exit(1)
}
let rate = 48000.0
let inL = u32(dev, kAudioDevicePropertyLatency, kAudioObjectPropertyScopeInput)
let outL = u32(dev, kAudioDevicePropertyLatency, kAudioObjectPropertyScopeOutput)
let inS = u32(dev, kAudioDevicePropertySafetyOffset, kAudioObjectPropertyScopeInput)
let outS = u32(dev, kAudioDevicePropertySafetyOffset, kAudioObjectPropertyScopeOutput)
if var w = wantBuf {
    var a = prop(kAudioDevicePropertyBufferFrameSize)
    let r = AudioObjectSetPropertyData(dev, &a, 0, nil, 4, &w)
    if r != noErr { print("Puffer \(w) nicht setzbar (Fehler \(r))") }
}
let buf = u32(dev, kAudioDevicePropertyBufferFrameSize, kAudioObjectPropertyScopeGlobal)
func ms(_ n: UInt32) -> String { String(format: "%.1f ms", Double(n) / rate * 1000) }
print("Geraet: \(devName(dev)), Puffer \(buf) Frames (\(ms(buf)))")
print("Gemeldet: Eingang Latenz \(ms(inL)) + Safety \(ms(inS)), Ausgang Latenz \(ms(outL)) + Safety \(ms(outS))")
print("  => gemeldete Summe ohne Puffer: \(ms(inL + inS + outL + outS)), mit je einem Puffer: \(ms(inL + inS + outL + outS + 2 * buf))")

let lock = NSLock()
let burstLen = 240.0, period = 24000.0, nBursts = 8
var starts: [Double] = []         // Ausgabe-Samplezeit je Burst
var found: [Double: Double] = [:] // burstStart -> Eingangs-Samplezeit der ersten Reaktion
var writtenHost: [Double: Double] = [:]  // burstStart -> Host-ms des schreibenden Callbacks
var foundHost: [Double: Double] = [:]    // burstStart -> Host-ms des erkennenden Callbacks
var nextBurst = -1.0
var done = false
let thresh: Float = 0.004          // -48 dBFS, Grundrauschen liegt bei etwa -90

var proc: AudioDeviceIOProcID?
let st = AudioDeviceCreateIOProcIDWithBlock(&proc, dev, DispatchQueue(label: "io")) { _, inData, inTime, outData, outTime in
    lock.lock(); defer { lock.unlock() }
    let nowMs = hostMs(mach_absolute_time())
    let outList = UnsafeMutableAudioBufferListPointer(outData)
    if let ob = outList.first, let p = ob.mData?.assumingMemoryBound(to: Float.self) {
        let n = Int(ob.mNumberChannels), frames = Int(ob.mDataByteSize) / 4 / max(n, 1)
        memset(ob.mData, 0, Int(ob.mDataByteSize))
        let t0 = outTime.pointee.mSampleTime
        if nextBurst < 0 { nextBurst = (t0 / period).rounded(.up) * period + 2 * period }
        for f in 0..<frames {
            let s = t0 + Double(f)
            if starts.count < nBursts, s >= nextBurst {
                if s < nextBurst + burstLen {
                    if starts.last != nextBurst { starts.append(nextBurst); writtenHost[nextBurst] = nowMs }
                    let v = amp * Float(sin(2 * Double.pi * 1000 * (s - nextBurst) / rate))
                    p[f * n] = v; if n > 1 { p[f * n + 1] = v }
                } else { nextBurst += period }
            }
        }
    }
    let inList = UnsafeMutableAudioBufferListPointer(UnsafeMutablePointer(mutating: inData))
    if let ib = inList.first, let p = ib.mData?.assumingMemoryBound(to: Float.self) {
        let n = Int(ib.mNumberChannels), frames = Int(ib.mDataByteSize) / 4 / max(n, 1)
        let t0 = inTime.pointee.mSampleTime
        for f in 0..<frames where retCh < n {
            let s = t0 + Double(f)
            if let b = starts.last(where: { $0 <= s }), found[b] == nil, abs(p[f * n + retCh]) > thresh, s - b < period {
                found[b] = s; foundHost[b] = nowMs
            }
        }
    }
    if starts.count >= nBursts && found.count >= nBursts { done = true }
}
guard st == noErr, let proc = proc, AudioDeviceStart(dev, proc) == noErr else { print("Start fehlgeschlagen"); exit(2) }
let deadline = Date().addingTimeInterval(10)
while Date() < deadline { Thread.sleep(forTimeInterval: 0.1); lock.lock(); let d = done; lock.unlock(); if d { break } }
AudioDeviceStop(dev, proc)

lock.lock()
let appRtts = starts.compactMap { b -> Double? in if let w = writtenHost[b], let f = foundHost[b] { return f - w } else { return nil } }
let rtts = starts.compactMap { found[$0].map { ($0 - Double(0)) } != nil ? (found[$0]! - $0) / rate * 1000 : nil }
lock.unlock()
print("Toene ausgegeben: \(starts.count), erkannt: \(rtts.count)")
for (i, r) in rtts.enumerated() { print(String(format: "  Ton %d: Round-Trip %.1f ms", i + 1, r)) }
if !rtts.isEmpty {
    let sorted = rtts.sorted()
    print(String(format: "Geraetezeit: Median %.1f ms, min %.1f ms, max %.1f ms", sorted[sorted.count / 2], sorted.first!, sorted.last!))
    let app = appRtts.sorted()
    if !app.isEmpty {
        print(String(format: "App (mit Puffern): Median %.1f ms, min %.1f ms, max %.1f ms", app[app.count / 2], app.first!, app.last!))
    }
} else {
    print("Nichts empfangen: CH1-Schalter auf USB? CH1-Fader auf? USB 5/6 = Post CH1 Fader? Rueckkanal \(retCh) richtig?")
}
