//
// DJMT1InstallerApp.swift
// Minimal host app that activates/deactivates the DJM-T1 audio driver
// system extension. Must run from /Applications for activation to work.
//

import SwiftUI
import SystemExtensions

private let driverBundleID = "de.cypher.djmt1.audio"

final class ExtensionManager: NSObject, ObservableObject, OSSystemExtensionRequestDelegate {
    @Published var status = "Bereit."

    func activate() {
        status = "Aktiviere Treiber…"
        let request = OSSystemExtensionRequest.activationRequest(
            forExtensionWithIdentifier: driverBundleID, queue: .main)
        request.delegate = self
        OSSystemExtensionManager.shared.submitRequest(request)
    }

    func deactivate() {
        status = "Deaktiviere Treiber…"
        let request = OSSystemExtensionRequest.deactivationRequest(
            forExtensionWithIdentifier: driverBundleID, queue: .main)
        request.delegate = self
        OSSystemExtensionManager.shared.submitRequest(request)
    }

    func request(_ request: OSSystemExtensionRequest,
                 actionForReplacingExtension existing: OSSystemExtensionProperties,
                 withExtension ext: OSSystemExtensionProperties)
        -> OSSystemExtensionRequest.ReplacementAction {
        return .replace
    }

    func requestNeedsUserApproval(_ request: OSSystemExtensionRequest) {
        status = "Warte auf Freigabe in den Systemeinstellungen "
            + "(Datenschutz & Sicherheit)…"
    }

    func request(_ request: OSSystemExtensionRequest,
                 didFinishWithResult result: OSSystemExtensionRequest.Result) {
        switch result {
        case .completed:
            status = "Fertig. Treiber ist aktiv."
        case .willCompleteAfterReboot:
            status = "Aktiv nach Neustart."
        @unknown default:
            status = "Abgeschlossen: \(result)"
        }
    }

    func request(_ request: OSSystemExtensionRequest, didFailWithError error: Error) {
        status = "Fehler: \(error.localizedDescription)"
    }
}

/// Kommandozeilenmodus: `DJMT1Installer --activate` feuert dieselbe Anfrage
/// wie der Knopf, schreibt das Ergebnis nach stdout und beendet sich. So laesst
/// sich die Aktivierung ohne Klicken protokollieren.
private final class CLIDelegate: NSObject, OSSystemExtensionRequestDelegate {
    func request(_ request: OSSystemExtensionRequest,
                 actionForReplacingExtension existing: OSSystemExtensionProperties,
                 withExtension ext: OSSystemExtensionProperties)
        -> OSSystemExtensionRequest.ReplacementAction {
        print("replacing \(existing.bundleVersion) with \(ext.bundleVersion)")
        return .replace
    }

    func requestNeedsUserApproval(_ request: OSSystemExtensionRequest) {
        print("NEEDS USER APPROVAL — Systemeinstellungen oeffnen")
    }

    func request(_ request: OSSystemExtensionRequest,
                 didFinishWithResult result: OSSystemExtensionRequest.Result) {
        print("RESULT: \(result.rawValue)")
        exit(0)
    }

    func request(_ request: OSSystemExtensionRequest, didFailWithError error: Error) {
        let nsError = error as NSError
        print("FAILED: domain=\(nsError.domain) code=\(nsError.code)")
        print("  \(nsError.localizedDescription)")
        print("  userInfo: \(nsError.userInfo)")
        exit(1)
    }
}

private func runCLI() -> Never {
    let delegate = CLIDelegate()
    print("bundle: \(Bundle.main.bundlePath)")
    let request = OSSystemExtensionRequest.activationRequest(
        forExtensionWithIdentifier: driverBundleID, queue: .main)
    request.delegate = delegate
    OSSystemExtensionManager.shared.submitRequest(request)
    // Der Delegate beendet den Prozess; ohne Antwort nach 60 s selbst abbrechen.
    DispatchQueue.main.asyncAfter(deadline: .now() + 60) {
        print("TIMEOUT — keine Antwort")
        exit(2)
    }
    RunLoop.main.run()
    fatalError()
}

@main
struct DJMT1InstallerApp: App {
    @StateObject private var manager = ExtensionManager()

    init() {
        if CommandLine.arguments.contains("--activate") { runCLI() }
    }

    var body: some Scene {
        WindowGroup("DJM-T1 Treiber") {
            VStack(spacing: 16) {
                Text("Pioneer DJM-T1 Audio-Treiber")
                    .font(.title2)
                Text(manager.status)
                    .font(.callout)
                    .foregroundStyle(.secondary)
                    .multilineTextAlignment(.center)
                HStack {
                    Button("Treiber aktivieren") { manager.activate() }
                        .buttonStyle(.borderedProminent)
                    Button("Treiber entfernen") { manager.deactivate() }
                }
            }
            .padding(32)
            .frame(minWidth: 420, minHeight: 200)
        }
    }
}
