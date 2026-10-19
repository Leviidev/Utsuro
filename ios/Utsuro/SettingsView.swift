import SwiftUI
import UIKit
import GameController

/// App-wide things: JIT, controllers, logs, and what this build is.
struct SettingsView: View {
    @ObservedObject private var jit = JITCoordinator.shared
    @ObservedObject private var library = GameLibrary.shared
    @State private var shareLog = false
    @State private var controllers: [String] = SettingsView.connectedControllers()
    @State private var usedBytes: Int64 = 0

    var body: some View {
        NavigationStack {
            ScrollView {
                VStack(alignment: .leading, spacing: 26) {
                    section("JIT") { JITCard().padding(.horizontal, -16).padding(.vertical, -4) }
                    section("Controllers") {
                        VStack(alignment: .leading, spacing: 8) {
                            if controllers.isEmpty {
                                Label("No controller connected", systemImage: "gamecontroller").foregroundStyle(Theme.textDim)
                                Text("Connect an Xbox, PlayStation or MFi controller in Settings › Bluetooth, and the touch controls step aside. "
                                     + "Each game can override that on its page.").font(.footnote).foregroundStyle(Theme.textDim)
                            } else {
                                ForEach(controllers, id: \.self) { Label($0, systemImage: "gamecontroller.fill").foregroundStyle(Theme.text) }
                            }
                        }
                        .padding(16).frame(maxWidth: .infinity, alignment: .leading).utsuroCard()
                    }
                    section("Storage") {
                        VStack(spacing: 0) {
                            info("Games", "\(library.games.count)")
                            info("Used by Utsuro", usedBytes > 0 ? ByteCountFormatter.string(fromByteCount: usedBytes, countStyle: .file) : "…")
                            info("Files app folder", "On My iPhone › Utsuro", last: true)
                        }
                        .utsuroCard()
                    }
                    section("Support") {
                        VStack(spacing: 0) {
                            Button { shareLog = true } label: {
                                HStack { Label("Share logs", systemImage: "square.and.arrow.up"); Spacer(); Image(systemName: "chevron.right").font(.footnote).foregroundStyle(Theme.textDim) }
                                    .padding(16).contentShape(Rectangle())
                            }
                            .buttonStyle(.plain)
                        }
                        .utsuroCard()
                    }
                    section("About") {
                        VStack(spacing: 0) {
                            info("Utsuro", versionLine)
                            info("Runs", "Xbox One games on arm64", last: true)
                        }
                        .utsuroCard()
                    }
                }
                .padding(16)
                .frame(maxWidth: 760)
                .frame(maxWidth: .infinity)
            }
            .background(Theme.backdrop)
            .navigationTitle("Settings")
            .navigationBarTitleDisplayMode(.large)
        }
        .sheet(isPresented: $shareLog) { ShareSheet(items: logFiles) }
        .onAppear {
            controllers = Self.connectedControllers()
            DispatchQueue.global(qos: .utility).async {
                let total = GameLibrary.size(of: GameLibrary.documents)
                DispatchQueue.main.async { usedBytes = total }
            }
        }
        .onReceive(NotificationCenter.default.publisher(for: .GCControllerDidConnect)) { _ in controllers = Self.connectedControllers() }
        .onReceive(NotificationCenter.default.publisher(for: .GCControllerDidDisconnect)) { _ in controllers = Self.connectedControllers() }
    }

    private func section<Content: View>(_ title: String, @ViewBuilder _ content: () -> Content) -> some View {
        VStack(alignment: .leading, spacing: 10) {
            SectionTitle(title)
            content()
        }
    }

    private func info(_ label: String, _ value: String, last: Bool = false) -> some View {
        VStack(spacing: 0) {
            HStack {
                Text(label).foregroundStyle(Theme.textDim)
                Spacer(minLength: 16)
                Text(value).foregroundStyle(Theme.text).multilineTextAlignment(.trailing)
            }
            .font(.system(size: 14))
            .padding(.horizontal, 16).padding(.vertical, 12)
            if !last { Divider().overlay(Theme.hairline).padding(.leading, 16) }
        }
    }

    private static func connectedControllers() -> [String] {
        GCController.controllers().map { $0.vendorName ?? "Controller" }
    }

    private var logFiles: [URL] {
        let docs = GameLibrary.documents
        return ["utsuro.log", "utsuro-prev.log", "utsuro-crash.log"].map { docs.appendingPathComponent($0) }
            .filter { FileManager.default.fileExists(atPath: $0.path) }
    }

    private var versionLine: String {
        let info = Bundle.main.infoDictionary
        let version = info?["CFBundleShortVersionString"] as? String ?? "?"
        let commit = info?["UtsuroBuildCommit"] as? String ?? "dev"
        return "\(version) · \(commit)"
    }
}

struct ShareSheet: UIViewControllerRepresentable {
    let items: [Any]
    func makeUIViewController(context: Context) -> UIActivityViewController { UIActivityViewController(activityItems: items, applicationActivities: nil) }
    func updateUIViewController(_ controller: UIActivityViewController, context: Context) {}
}
