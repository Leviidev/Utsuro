import SwiftUI
import UIKit
import UniformTypeIdentifiers

struct HomeView: View {
    @ObservedObject private var jit = JITCoordinator.shared
    @ObservedObject private var library = GameLibrary.shared
    @State private var playing = false
    @State private var picking = false
    @State private var shareLog = false
    @State private var playError: String?
    @Environment(\.scenePhase) private var scenePhase

    var body: some View {
        if playing, let dump = library.dump {
            GameScreen(dump: dump) { playing = false }
        } else {
            home
        }
    }

    private var home: some View {
        NavigationStack {
            ScrollView {
                VStack(spacing: 14) {
                    gameCard
                    JITCard()
                    playButton
                    if let playError { Text(playError).font(.footnote).foregroundStyle(.orange).padding(.horizontal, 16) }
                    toolsCard
                    Text(buildLine).font(.caption2).foregroundStyle(Theme.textDim)
                }
                .padding(.vertical, 14)
                .frame(maxWidth: 720)
                .frame(maxWidth: .infinity)
            }
            .background(Theme.backdrop)
            .navigationTitle("Utsuro")
        }
        .sheet(isPresented: $jit.showSetup) { JITSetupFlow() }
        .sheet(isPresented: $shareLog) { ShareSheet(items: logFiles) }
        .fileImporter(isPresented: $picking, allowedContentTypes: [.folder]) { result in
            switch result {
            case .success(let url): library.importDump(from: url)
            case .failure(let failure): library.importError = failure.localizedDescription
            }
        }
        .onAppear {
            library.refresh()
            jit.enableAtLaunchIfAsked()
            claimIfAttached()
        }
        .onChange(of: jit.attachGeneration) { _ in claimIfAttached() }
        .onChange(of: scenePhase) { phase in if phase == .active { library.refresh(); claimIfAttached() } }
    }

    private func claimIfAttached() {
        if JITBootstrap.debuggedFlag, !JITBootstrap.prewarmed { JITBootstrap.prewarm() }
    }

    private var gameCard: some View {
        VStack(alignment: .leading, spacing: 10) {
            HStack(spacing: 12) {
                Image(systemName: library.dump == nil ? "shippingbox" : "checkmark.circle.fill")
                    .font(.title2).foregroundStyle(library.dump == nil ? Theme.textDim : Theme.good)
                VStack(alignment: .leading, spacing: 2) {
                    Text("Minecraft: Xbox One Edition").font(.headline)
                    Text(library.dump == nil ? "No game files yet" : "Game files found").font(.subheadline).foregroundStyle(.secondary)
                }
                Spacer(minLength: 0)
            }
            if library.dump == nil {
                Text("Utsuro runs a decrypted dump. Copy the folder that holds Mount and EmbeddedXvd into Utsuro's folder in the Files app "
                     + "(On My iPhone › Utsuro), or pick it here and Utsuro will copy it in.")
                    .font(.subheadline)
            }
            if library.importing {
                HStack(spacing: 10) { ProgressView(); Text(library.importProgress ?? "Copying…").font(.subheadline).foregroundStyle(.secondary) }
            } else {
                Button { picking = true } label: {
                    Label(library.dump == nil ? "Import Game Folder" : "Replace Game Folder", systemImage: "square.and.arrow.down")
                }
                .buttonStyle(.bordered)
            }
            if let failure = library.importError { Text(failure).font(.footnote).foregroundStyle(.orange) }
        }
        .padding(16)
        .frame(maxWidth: .infinity, alignment: .leading)
        .utsuroCard()
        .padding(.horizontal, 16)
    }

    private var playButton: some View {
        Button { play() } label: {
            HStack { Spacer(); Label("Play", systemImage: "play.fill").font(.headline); Spacer() }
        }
        .buttonStyle(.borderedProminent)
        .controlSize(.large)
        .disabled(library.dump == nil || library.importing)
        .padding(.horizontal, 16)
    }

    private func play() {
        playError = nil
        if !JITBootstrap.prewarmed { JITBootstrap.prewarm() }
        guard JITBootstrap.prewarmed else {
            if let failure = JITBootstrap.lastFailure { playError = failure }
            else { playError = "JIT is not on yet. Turn it on above, then press Play." }
            jit.enable()
            return
        }
        UtsuroLog.log("ui", "play")
        playing = true
    }

    private var toolsCard: some View {
        VStack(spacing: 0) {
            Button { shareLog = true } label: {
                HStack { Label("Share Logs", systemImage: "square.and.arrow.up"); Spacer() }.padding(14).contentShape(Rectangle())
            }
            .buttonStyle(.plain)
        }
        .utsuroCard()
        .padding(.horizontal, 16)
    }

    private var logFiles: [URL] {
        let docs = GameLibrary.documents
        return ["utsuro.log", "utsuro-prev.log", "utsuro-crash.log"].map { docs.appendingPathComponent($0) }
            .filter { FileManager.default.fileExists(atPath: $0.path) }
    }

    private var buildLine: String {
        let info = Bundle.main.infoDictionary
        let version = info?["CFBundleShortVersionString"] as? String ?? "?"
        let commit = info?["UtsuroBuildCommit"] as? String ?? "dev"
        return "Utsuro \(version) · \(commit)"
    }
}

struct ShareSheet: UIViewControllerRepresentable {
    let items: [Any]
    func makeUIViewController(context: Context) -> UIActivityViewController { UIActivityViewController(activityItems: items, applicationActivities: nil) }
    func updateUIViewController(_ controller: UIActivityViewController, context: Context) {}
}
