import SwiftUI
import UIKit

/// One game: its art, a Play button, the screenshots taken in it, how it is set up, and how to remove it.
struct GamePage: View {
    let game: InstalledGame
    @ObservedObject private var library = GameLibrary.shared
    @ObservedObject private var session = Session.shared
    @ObservedObject private var jit = JITCoordinator.shared
    @Environment(\.dismiss) private var dismiss

    @State private var touch: GameSettings.TouchControls
    @State private var opacity: Double
    @State private var haptics: Bool
    @State private var viewing: URL?
    @State private var confirmSaves = false
    @State private var confirmRemove = false
    @State private var saveSize: Int64 = 0
    @State private var gameSize: Int64 = 0
    @State private var screenshots: [URL] = []

    init(game: InstalledGame) {
        self.game = game
        let s = GameSettings(gameID: game.id)
        _touch = State(initialValue: s.touchControls)
        _opacity = State(initialValue: s.padOpacity)
        _haptics = State(initialValue: s.haptics)
    }

    private var settings: GameSettings { GameSettings(gameID: game.id) }

    var body: some View {
        List {
            Section {
                PageHero(game: game, subtitle: subtitle) { primary }
            } footer: {
                if let error = session.launchError { Text(error).foregroundStyle(.orange) }
            }
            .listRowInsets(EdgeInsets())
            .listRowBackground(Color.clear)

            Section("Screenshots") {
                if screenshots.isEmpty {
                    Label("In the game, tap the camera at the top of the screen to take one.", systemImage: "camera")
                        .font(.subheadline)
                        .foregroundStyle(.secondary)
                } else {
                    ScrollView(.horizontal, showsIndicators: false) {
                        HStack(spacing: 10) {
                            ForEach(screenshots, id: \.self) { url in
                                Button { viewing = url } label: {
                                    FileImage(url: url, maxPixel: 700, contentMode: .fill)
                                        .frame(width: 220, height: 124)
                                        .background(Console.card)
                                        .clipShape(RoundedRectangle(cornerRadius: 10, style: .continuous))
                                }
                                .buttonStyle(CardButtonStyle())
                            }
                        }
                        .padding(.horizontal, 12)
                    }
                    .listRowInsets(EdgeInsets(top: 10, leading: 0, bottom: 10, trailing: 0))
                }
            }

            if !game.summary.isEmpty && game.summary != game.title {
                Section("About") {
                    Text(game.summary).font(.subheadline)
                }
            }

            Section {
                Picker("Touch Controls", selection: $touch) {
                    ForEach(GameSettings.TouchControls.allCases) { Text($0.title).tag($0) }
                }
                if touch != .off {
                    VStack(alignment: .leading, spacing: 6) {
                        HStack { Text("Opacity"); Spacer(); Text("\(Int(opacity * 100))%").foregroundStyle(.secondary) }
                        Slider(value: $opacity, in: 0.15...1)
                    }
                    Toggle("Haptic Feedback", isOn: $haptics)
                }
            } header: {
                Text("Controls")
            } footer: {
                Text(touch == .auto ? "The touch controller hides while a physical controller is connected."
                     : touch == .always ? "The touch controller is drawn even with a controller connected." : "The touch controller is never drawn.")
            }

            Section("Details") {
                LabeledContent("Publisher", value: game.publisher)
                if !game.version.isEmpty { LabeledContent("Version", value: game.version) }
                if let id = game.titleId { LabeledContent("Title ID", value: id) }
                if !game.executable.isEmpty { LabeledContent("Executable", value: game.executable) }
                if let played = timePlayed { LabeledContent("Played", value: played) }
                LabeledContent("Size", value: gameSize > 0 ? ByteCountFormatter.string(fromByteCount: gameSize, countStyle: .file) : "…")
                LabeledContent("Saves", value: saveSize > 0 ? ByteCountFormatter.string(fromByteCount: saveSize, countStyle: .file) : "None")
            }

            Section {
                Button(role: .destructive) { confirmSaves = true } label: { Label("Delete Saves", systemImage: "trash") }
                    .disabled(saveSize == 0)
                Button(role: .destructive) { confirmRemove = true } label: { Label("Remove Game", systemImage: "xmark.bin") }
            }
        }
        .listStyle(.insetGrouped)
        .navigationTitle(game.title)
        .navigationBarTitleDisplayMode(.inline)
        .sheet(item: Binding(get: { viewing.map { IdentifiedURL(url: $0) } }, set: { viewing = $0?.url })) { item in
            ScreenshotViewer(url: item.url) { url in
                library.deleteScreenshot(url)
                refreshScreenshots()
            }
        }
        .confirmationDialog("Delete the saves of \(game.title)?", isPresented: $confirmSaves, titleVisibility: .visible) {
            Button("Delete saves", role: .destructive) { library.deleteSaves(of: game); loadSizes() }
        } message: { Text("This cannot be undone.") }
        .confirmationDialog("Remove \(game.title)?", isPresented: $confirmRemove, titleVisibility: .visible) {
            Button("Remove game, keep saves", role: .destructive) { library.remove(game, saves: false); dismiss() }
            Button("Remove game and saves", role: .destructive) { library.remove(game, saves: true); dismiss() }
        } message: { Text("The game's files are deleted from this iPhone. You can add the dump again later.") }
        .id(jit.attachGeneration)
        .onAppear { refreshScreenshots(); loadSizes() }
        .onChange(of: touch) { settings.touchControls = $0 }
        .onChange(of: opacity) { settings.padOpacity = $0 }
        .onChange(of: haptics) { settings.haptics = $0 }
    }

    // MARK: - Pieces

    private var subtitle: String {
        [game.publisher, game.lastPlayedText].compactMap { $0 }.joined(separator: " · ")
    }

    private var timePlayed: String? {
        let seconds = settings.playSeconds
        guard seconds >= 60 else { return nil }
        let hours = Int(seconds) / 3600, minutes = (Int(seconds) % 3600) / 60
        return hours > 0 ? "\(hours) h \(minutes) min" : "\(minutes) min"
    }

    /// Play, or what stands in its way.
    @ViewBuilder
    private var primary: some View {
        if !JITControl.isOn {
            if jit.busy {
                HStack(spacing: 10) { ProgressView(); Text(jit.status ?? "Turning on JIT…").foregroundStyle(.secondary) }
                    .frame(maxWidth: .infinity).frame(height: 50)
            } else {
                BigButton(title: "Turn On JIT to Play", systemImage: "bolt.fill") { JITControl.turnOn(jit) }
            }
        } else {
            BigButton(title: "Play", systemImage: "play.fill") { session.play(game) }
        }
    }

    // MARK: - Data

    private func refreshScreenshots() { screenshots = library.screenshots(for: game) }

    private func loadSizes() {
        let dump = game.dump, saves = GameLibrary.saves(for: game)
        DispatchQueue.global(qos: .utility).async {
            let g = GameLibrary.size(of: dump), s = GameLibrary.size(of: saves)
            DispatchQueue.main.async { gameSize = g; saveSize = s }
        }
    }
}

private struct IdentifiedURL: Identifiable { let url: URL; var id: URL { url } }

/// The top of a game's page: its splash, its box art and name over it, and the one thing to do as a big button.
private struct PageHero<Action: View>: View {
    let game: InstalledGame
    let subtitle: String
    @ViewBuilder var action: Action

    var body: some View {
        VStack(spacing: 0) {
            ZStack(alignment: .bottomLeading) {
                GameBackdrop(game: game, fallback: Color(uiColor: .tertiarySystemFill))
                LinearGradient(colors: [.clear, .black.opacity(0.7)], startPoint: .top, endPoint: .bottom)
                HStack(alignment: .bottom, spacing: 14) {
                    GameIcon(game: game, size: 84)
                        .shadow(color: .black.opacity(0.35), radius: 12, y: 5)
                    VStack(alignment: .leading, spacing: 4) {
                        Text(game.title)
                            .font(.system(.title2, design: .rounded).weight(.heavy))
                            .lineLimit(2)
                            .minimumScaleFactor(0.8)
                        Text(subtitle)
                            .font(.subheadline)
                            .foregroundStyle(.white.opacity(0.75))
                            .lineLimit(2)
                    }
                    .foregroundStyle(.white)
                    Spacer(minLength: 0)
                }
                .padding(16)
            }
            .frame(height: 210)
            action
                .padding(14)
        }
        .background(Color(uiColor: .secondarySystemGroupedBackground))
        .clipShape(RoundedRectangle(cornerRadius: 24, style: .continuous))
    }
}

/// The page's main button.
private struct BigButton: View {
    let title: String
    let systemImage: String
    let action: () -> Void

    var body: some View {
        Button(action: action) {
            Label(title, systemImage: systemImage)
                .font(.system(.headline, design: .rounded))
                .frame(maxWidth: .infinity)
                .frame(height: 50)
                .foregroundStyle(Color.white)
                .background(Color.accentColor, in: RoundedRectangle(cornerRadius: 15, style: .continuous))
        }
        .buttonStyle(CardButtonStyle())
    }
}

/// A screenshot, full size, with share and delete.
struct ScreenshotViewer: View {
    let url: URL
    let onDelete: (URL) -> Void
    @Environment(\.dismiss) private var dismiss
    @State private var confirmDelete = false

    var body: some View {
        NavigationStack {
            ZStack {
                Color.black.ignoresSafeArea()
                FileImage(url: url, maxPixel: 2400)
            }
            .navigationTitle("Screenshot")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .topBarLeading) { Button("Done") { dismiss() } }
                ToolbarItemGroup(placement: .topBarTrailing) {
                    ShareLink(item: url) { Image(systemName: "square.and.arrow.up") }
                    Button(role: .destructive) { confirmDelete = true } label: { Image(systemName: "trash") }
                }
            }
            .confirmationDialog("Delete this screenshot?", isPresented: $confirmDelete, titleVisibility: .visible) {
                Button("Delete", role: .destructive) { onDelete(url); dismiss() }
            }
        }
        .tint(Theme.accentBright)
        .preferredColorScheme(.dark)
    }
}
