import SwiftUI
import UIKit

/// One game: its splash, whole, at the top; its name in serif; three rings for how it runs, how long it has been played and
/// what it has saved; Play; then its screenshots, its controls, what it is, and removing it.
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
        ScrollView {
            VStack(alignment: .leading, spacing: 0) {
                GameBackdrop(game: game, pixels: 1600)
                    .aspectRatio(16.0 / 10.5, contentMode: .fit)
                    .clipShape(UnevenRoundedRectangle(bottomLeadingRadius: 30, bottomTrailingRadius: 30, style: .continuous))

                VStack(alignment: .leading, spacing: 6) {
                    Caps(["Xbox One", game.publisher].joined(separator: " · "))
                    Text(game.title)
                        .font(.title(36))
                        .foregroundStyle(Ink.text)
                        .lineLimit(3)
                        .minimumScaleFactor(0.7)
                        .fixedSize(horizontal: false, vertical: true)
                }
                .padding(.horizontal, 20)
                .padding(.top, 18)

                HStack(spacing: 0) {
                    StatRing(value: game.compatibility.label, label: "Status", progress: game.readiness, color: game.compatibility.color)
                    StatRing(value: shortPlayed, label: "Played", progress: min(settings.playSeconds / 36_000, 1))
                    StatRing(value: saveSize > 0 ? ByteCountFormatter.string(fromByteCount: saveSize, countStyle: .file) : "None",
                             label: "Saves", progress: min(Double(saveSize) / 50_000_000, 1), color: Ink.dim)
                }
                .padding(.horizontal, 10)
                .padding(.top, 18)

                primary
                    .padding(.horizontal, 16)
                    .padding(.top, 20)
                if let error = session.launchError {
                    Text(error).font(.footnote).foregroundStyle(.orange)
                        .fixedSize(horizontal: false, vertical: true)
                        .padding(.horizontal, 20).padding(.top, 10)
                }

                screenshotsSection.padding(.top, 30)
                if !game.summary.isEmpty && game.summary != game.title { aboutSection.padding(.top, 30) }
                controlsSection.padding(.top, 30)
                detailsSection.padding(.top, 30)
                manageSection.padding(.top, 22)
            }
            .padding(.bottom, 30)
            .frame(maxWidth: 760)
            .frame(maxWidth: .infinity)
        }
        .scrollIndicators(.hidden)
        .ignoresSafeArea(edges: .top)
        .background(Ink.page.ignoresSafeArea())
        .navigationBarTitleDisplayMode(.inline)
        .toolbarBackground(.hidden, for: .navigationBar)
        .toolbar {
            ToolbarItem(placement: .topBarTrailing) {
                Menu {
                    Button { session.play(game) } label: { Label("Play", systemImage: "play.fill") }
                    Divider()
                    Button(role: .destructive) { confirmSaves = true } label: { Label("Delete Saves", systemImage: "trash") }
                        .disabled(saveSize == 0)
                    Button(role: .destructive) { confirmRemove = true } label: { Label("Remove Game", systemImage: "xmark.bin") }
                } label: { Image(systemName: "ellipsis") }
            }
        }
        .sheet(item: Binding(get: { viewing.map { IdentifiedURL(url: $0) } }, set: { viewing = $0?.url })) { item in
            ScreenshotViewer(url: item.url) { url in
                library.deleteScreenshot(url)
                refreshScreenshots()
            }
        }
        .confirmationDialog("Delete the saves of \(game.title)?", isPresented: $confirmSaves, titleVisibility: .visible) {
            Button("Delete saves", role: .destructive) { library.deleteSaves(of: game); loadSizes() }
        } message: { Text("This cannot be undone.") }
        .removeGameDialog(game, isPresented: $confirmRemove) { dismiss() }
        .id(jit.attachGeneration)
        .onAppear { refreshScreenshots(); loadSizes() }
        .onChange(of: touch) { settings.touchControls = $0 }
        .onChange(of: opacity) { settings.padOpacity = $0 }
        .onChange(of: haptics) { settings.haptics = $0 }
    }

    // MARK: - Pieces

    private var shortPlayed: String {
        let seconds = settings.playSeconds
        guard seconds >= 60 else { return "—" }
        let hours = Int(seconds) / 3600, minutes = (Int(seconds) % 3600) / 60
        return hours > 0 ? "\(hours)h \(minutes)m" : "\(minutes)m"
    }

    /// Play, or what stands in its way.
    @ViewBuilder
    private var primary: some View {
        if !JITControl.isOn {
            if jit.busy {
                HStack(spacing: 10) { ProgressView(); Text(jit.status ?? "Turning on JIT…").foregroundStyle(Ink.dim) }
                    .frame(maxWidth: .infinity).frame(height: 54)
            } else {
                Button { JITControl.turnOn(jit) } label: { Label("Turn On JIT to Play", systemImage: "bolt.fill") }
                    .buttonStyle(PillButtonStyle())
            }
        } else {
            Button { session.play(game) } label: { Label("Play", systemImage: "play.fill") }
                .buttonStyle(PillButtonStyle())
        }
    }

    private var screenshotsSection: some View {
        VStack(alignment: .leading, spacing: 12) {
            SectionHeading(title: "Screenshots", count: screenshots.isEmpty ? nil : screenshots.count).padding(.horizontal, 20)
            if screenshots.isEmpty {
                Text("In the game, tap the camera at the top of the screen to keep a moment here.")
                    .font(.subheadline).foregroundStyle(Ink.dim)
                    .fixedSize(horizontal: false, vertical: true)
                    .padding(.horizontal, 20)
            } else {
                ScrollView(.horizontal, showsIndicators: false) {
                    HStack(spacing: 10) {
                        ForEach(screenshots, id: \.self) { url in
                            Button { viewing = url } label: {
                                FileImage(url: url, maxPixel: 700, contentMode: .fill)
                                    .frame(width: 210, height: 118)
                                    .background(Ink.card)
                                    .clipShape(RoundedRectangle(cornerRadius: 16, style: .continuous))
                            }
                            .buttonStyle(PressScale())
                        }
                    }
                    .padding(.horizontal, 16)
                }
            }
        }
    }

    private var aboutSection: some View {
        VStack(alignment: .leading, spacing: 10) {
            SectionHeading(title: "About")
            Text(game.summary).font(.system(size: 15)).foregroundStyle(Ink.text.opacity(0.85))
                .fixedSize(horizontal: false, vertical: true)
        }
        .padding(.horizontal, 20)
    }

    private var controlsSection: some View {
        VStack(alignment: .leading, spacing: 12) {
            SectionHeading(title: "Controls").padding(.horizontal, 4)
            VStack(spacing: 0) {
                HStack {
                    Text("Touch controls")
                    Spacer()
                    Picker("Touch controls", selection: $touch) {
                        ForEach(GameSettings.TouchControls.allCases) { Text($0.title).tag($0) }
                    }
                    .labelsHidden()
                    .tint(Ink.dim)
                }
                .padding(.vertical, 5)
                if touch != .off {
                    divider
                    VStack(alignment: .leading, spacing: 6) {
                        HStack { Text("Opacity"); Spacer(); Text("\(Int(opacity * 100))%").foregroundStyle(Ink.dim) }
                        Slider(value: $opacity, in: 0.15...1)
                    }
                    .padding(.vertical, 10)
                    divider
                    Toggle("Haptic feedback", isOn: $haptics).padding(.vertical, 8)
                }
            }
            .font(.system(size: 16))
            .foregroundStyle(Ink.text)
            .padding(.horizontal, 16)
            .padding(.vertical, 4)
            .inkCard()
            Text(touch == .auto ? "The touch controller steps aside while a physical controller is connected."
                 : touch == .always ? "The touch controller is drawn even with a controller connected." : "The touch controller is never drawn.")
                .font(.footnote).foregroundStyle(Ink.dim).padding(.horizontal, 6)
        }
        .padding(.horizontal, 16)
    }

    private var detailsSection: some View {
        VStack(alignment: .leading, spacing: 12) {
            SectionHeading(title: "Details").padding(.horizontal, 4)
            VStack(spacing: 0) {
                detail("Publisher", game.publisher)
                if !game.version.isEmpty { divider; detail("Version", game.version) }
                if let id = game.titleId { divider; detail("Title ID", id) }
                if !game.executable.isEmpty { divider; detail("Executable", game.executable) }
                divider
                detail("Size", gameSize > 0 ? ByteCountFormatter.string(fromByteCount: gameSize, countStyle: .file) : "…")
                if let last = game.lastPlayedAgo { divider; detail("Last played", last.prefix(1).uppercased() + last.dropFirst()) }
            }
            .padding(.horizontal, 16)
            .inkCard()
        }
        .padding(.horizontal, 16)
    }

    private var manageSection: some View {
        VStack(spacing: 0) {
            Button(role: .destructive) { confirmSaves = true } label: {
                Label("Delete Saves", systemImage: "trash").frame(maxWidth: .infinity, alignment: .leading).padding(.vertical, 14)
            }
            .disabled(saveSize == 0)
            divider
            Button(role: .destructive) { confirmRemove = true } label: {
                Label("Remove Game", systemImage: "xmark.bin").frame(maxWidth: .infinity, alignment: .leading).padding(.vertical, 14)
            }
        }
        .font(.system(size: 16))
        .foregroundStyle(.red)
        .padding(.horizontal, 16)
        .inkCard()
        .padding(.horizontal, 16)
    }

    private var divider: some View { Rectangle().fill(Ink.hairline).frame(height: 0.5) }

    private func detail(_ label: String, _ value: String) -> some View {
        HStack(alignment: .firstTextBaseline) {
            Text(label).foregroundStyle(Ink.dim)
            Spacer(minLength: 16)
            Text(value).foregroundStyle(Ink.text).multilineTextAlignment(.trailing).lineLimit(2)
        }
        .font(.system(size: 15))
        .padding(.vertical, 12)
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
