// t1tone.swift -- spielt einen Dauerton auf einem Ausgangspaar eines
// CoreAudio-Geraets, zum Abhoeren, was der Mixer mit diesem Paar macht
// (Kanal-Eingang, Master, Kopfhoerer-Cue). Beim DJM-T1: Paar 0 = USB 1/2
// (CH1 USB), Paar 1 = USB 3/4 (CH2 USB), Paar 2 = USB 5/6.
//
// Build: swiftc -O tools/t1tone.swift -o t1tone
// Run:   ./t1tone [Geraetename-Teil] [Paar 0-2] [Sekunden (8)] [Pegel dBFS (-30)]
//
// ACHTUNG: gibt hoerbaren Ton aus. DJ-Software-Engine vorher stoppen, Master
// und Kopfhoerer leise stellen.
import CoreAudio
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
func outChannels(_ id: AudioDeviceID) -> Int {
    var a = prop(kAudioDevicePropertyStreamConfiguration, kAudioObjectPropertyScopeOutput)
    var size: UInt32 = 0
    guard AudioObjectGetPropertyDataSize(id, &a, 0, nil, &size) == noErr, size > 0 else { return 0 }
    let raw = UnsafeMutableRawPointer.allocate(byteCount: Int(size), alignment: 16)
    defer { raw.deallocate() }
    AudioObjectGetPropertyData(id, &a, 0, nil, &size, raw)
    return UnsafeMutableAudioBufferListPointer(raw.assumingMemoryBound(to: AudioBufferList.self)).reduce(0) { $0 + Int($1.mNumberChannels) }
}

setvbuf(stdout, nil, _IOLBF, 0)
let args = CommandLine.arguments
let needle = args.count > 1 ? args[1] : "DJM-T1"
let pair = args.count > 2 ? Int(args[2]) ?? 0 : 0
let seconds = args.count > 3 ? Double(args[3]) ?? 8 : 8
let dB = args.count > 4 ? Double(args[4]) ?? -30 : -30
let amp = Float(pow(10.0, dB / 20))

guard let dev = allDevices().first(where: { devName($0).localizedCaseInsensitiveContains(needle) && outChannels($0) > 0 }) else {
    print("Kein Ausgabegeraet mit '\(needle)' gefunden"); exit(1)
}
let nch = outChannels(dev)
guard pair >= 0, pair * 2 + 1 < nch else { print("Paar \(pair) gibt es nicht (\(nch) Kanaele)"); exit(1) }
print("Geraet: \(devName(dev)), \(nch) Ausgangskanaele. Ton 1 kHz, \(Int(dB)) dBFS auf Paar \(pair) (Kanal \(pair * 2 + 1)/\(pair * 2 + 2)), \(Int(seconds)) s")

var phase = 0.0
var proc: AudioDeviceIOProcID?
let st = AudioDeviceCreateIOProcIDWithBlock(&proc, dev, DispatchQueue(label: "io")) { _, _, _, outData, _ in
    let list = UnsafeMutableAudioBufferListPointer(outData)
    guard let b = list.first, let p = b.mData?.assumingMemoryBound(to: Float.self) else { return }
    let n = Int(b.mNumberChannels), frames = Int(b.mDataByteSize) / 4 / max(n, 1)
    memset(b.mData, 0, Int(b.mDataByteSize))
    for f in 0..<frames {
        let v = amp * Float(sin(phase)); phase += 2 * Double.pi * 1000 / 48000
        p[f * n + pair * 2] = v; p[f * n + pair * 2 + 1] = v
    }
}
guard st == noErr, let proc = proc, AudioDeviceStart(dev, proc) == noErr else { print("Start fehlgeschlagen"); exit(2) }
Thread.sleep(forTimeInterval: seconds)
AudioDeviceStop(dev, proc)
print("fertig")
