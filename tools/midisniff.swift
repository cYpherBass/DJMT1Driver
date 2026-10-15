//
// midisniff.swift — logs every MIDI 1.0 channel message from one CoreMIDI
// source. Used to derive the DJM-T1 mapping in DeckLab (2026-09-03).
//
// Build: swiftc -o midisniff tools/midisniff.swift
// Run:   ./midisniff <logfile> [source-name-substring]   (default: DJM-T1)
//

import CoreMIDI
import Foundation

let logPath = CommandLine.arguments.count > 1 ? CommandLine.arguments[1] : "midisniff.log"
let sourceMatch = CommandLine.arguments.count > 2 ? CommandLine.arguments[2] : "DJM-T1"
FileManager.default.createFile(atPath: logPath, contents: nil)
let fh = FileHandle(forWritingAtPath: logPath)!

func log(_ s: String) {
    fh.write((s + "\n").data(using: .utf8)!)
    try? fh.synchronize()
}

var client = MIDIClientRef()
MIDIClientCreate("sniffer" as CFString, nil, nil, &client)

var inPort = MIDIPortRef()
MIDIInputPortCreateWithProtocol(client, "in" as CFString, ._1_0, &inPort) { evtList, _ in
    let packets = evtList.pointee
    var packet = packets.packet
    for _ in 0..<packets.numPackets {
        let words = Mirror(reflecting: packet.words).children.prefix(Int(packet.wordCount))
        for w in words {
            guard let word = w.value as? UInt32 else { continue }
            let mt = (word >> 28) & 0xF
            if mt == 2 { // MIDI 1.0 channel voice
                let status = UInt8((word >> 16) & 0xFF)
                let d1 = UInt8((word >> 8) & 0x7F)
                let d2 = UInt8(word & 0x7F)
                let ch = (status & 0x0F) + 1
                let kind: String
                switch status & 0xF0 {
                case 0x90: kind = d2 > 0 ? "NoteOn " : "NoteOff"
                case 0x80: kind = "NoteOff"
                case 0xB0: kind = "CC     "
                case 0xE0: kind = "Pitch  "
                default:   kind = String(format: "st%02X   ", status)
                }
                log(String(format: "%@ ch%02d d1=%3d (0x%02X) d2=%3d", kind, ch, d1, d1, d2))
            }
        }
        packet = MIDIEventPacketNext(&packet).pointee
    }
}

var src = MIDIEndpointRef()
for i in 0..<MIDIGetNumberOfSources() {
    let s = MIDIGetSource(i)
    var nameRef: Unmanaged<CFString>?
    MIDIObjectGetStringProperty(s, kMIDIPropertyDisplayName, &nameRef)
    let n = nameRef?.takeRetainedValue() as String? ?? ""
    if n.contains(sourceMatch) { src = s; break }
}
guard src != 0 else { log("ERROR: source \(sourceMatch) not found"); exit(1) }
MIDIPortConnectSource(inPort, src, nil)
log("connected to \(sourceMatch), listening...")
RunLoop.main.run()
