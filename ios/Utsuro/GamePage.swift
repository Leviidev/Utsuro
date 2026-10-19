import SwiftUI
import UIKit

/// One game: its art, a Play button, the screenshots taken in it, how it is set up, and how to remove it.
struct GamePage: View {
    let game: InstalledGame
    @ObservedObject private var library = GameLibrary.shared
    @ObservedObject private var session = Session.shared
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
            VStack(alignment: .leading, spacing: 26) {
                hero
                header
                playBlock
                screenshotsSection
                if !game.summary.isEmpty && game.summary != game.title { aboutSection }
                settingsSection
                detailsSection
                manageSection
            }
            .padding(.horizontal, 16)
            .padding(.bottom, 30)
            .frame(maxWidth: 760)
            .frame(maxWidth: .infinity)
        }
        .background(Theme.backdrop)
        .navigationBarTitleDisplayMode(.inline)
        .toolbarBackground(Theme.bg, for: .navigationBar)
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
        .onAppear { refreshScreenshots(); loadSizes() }
        .onChange(of: touch) { settings.touchControls = $0 }
        .onChange(of: opacity) { settings.padOpacity = $0 }
        .onChange(of: haptics) { settings.haptics = $0 }
    }

    // MARK: - Pieces

    private var hero: some View {
        ZStack {
            game.tileColor
            FileImage(url: game.splash ?? game.wideLogo ?? game.logo, maxPixel: 1200, contentMode: .fit)
        }
        .frame(maxWidth: .infinity).aspectRatio(16.0 / 9.0, contentMode: .fit)
        .clipShape(RoundedRectangle(cornerRadius: Theme.cardCorner, style: .continuous))
    }

    private var header: some View {
        VStack(alignment: .leading, spacing: 8) {
            HStack(alignment: .center, spacing: 14) {
                ZStack {
                    game.tileColor
                    FileImage(url: game.logo, maxPixel: 300).padding(6)
                }
                .frame(width: 64, height: 64)
                .clipShape(RoundedRectangle(cornerRadius: 10, style: .continuous))
                VStack(alignment: .leading, spacing: 2) {
                    Text(game.title).font(.system(size: 26, weight: .bold)).foregroundStyle(Theme.text)
                    Text(game.publisher).font(.system(size: 14)).foregroundStyle(Theme.textDim)
                }
            }
            HStack(spacing: 8) {
                chip("Xbox One")
                if !game.version.isEmpty { chip("v\(game.version)") }
                ForEach(game.ratings, id: \.self) { chip($0) }
            }
        }
    }

    private func chip(_ text: String) -> some View {
        Text(text).font(.system(size: 12, weight: .semibold)).foregroundStyle(Theme.textDim)
            .padding(.horizontal, 9).padding(.vertical, 4)
            .background(Theme.surfaceHigh, in: RoundedRectangle(cornerRadius: 5, style: .continuous))
    }

    private var playBlock: some View {
        VStack(alignment: .leading, spacing: 10) {
            Button { session.play(game) } label: { Label("Play", systemImage: "play.fill") }
                .buttonStyle(PrimaryButtonStyle())
            if let error = session.launchError {
                Text(error).font(.footnote).foregroundStyle(.orange)
            }
            let played = playedLine
            if !played.isEmpty { Text(played).font(.footnote).foregroundStyle(Theme.textDim) }
        }
    }

    private var playedLine: String {
        var parts: [String] = []
        if let last = settings.lastPlayed {
            parts.append("Last played " + last.formatted(.relative(presentation: .named)))
        }
        let seconds = settings.playSeconds
        if seconds >= 60 {
            let hours = Int(seconds) / 3600, minutes = (Int(seconds) % 3600) / 60
            parts.append(hours > 0 ? "\(hours) h \(minutes) min played" : "\(minutes) min played")
        }
        return parts.joined(separator: " · ")
    }

    private var screenshotsSection: some View {
        VStack(alignment: .leading, spacing: 12) {
            SectionTitle("Screenshots")
            if screenshots.isEmpty {
                VStack(spacing: 8) {
                    Image(systemName: "camera").font(.system(size: 26)).foregroundStyle(Theme.textDim)
                    Text("Nothing here yet. In the game, tap the camera at the top of the screen to take one.")
                        .font(.footnote).foregroundStyle(Theme.textDim).multilineTextAlignment(.center)
                }
                .frame(maxWidth: .infinity).padding(22).utsuroCard()
            } else {
                ScrollView(.horizontal, showsIndicators: false) {
                    HStack(spacing: 12) {
                        ForEach(screenshots, id: \.self) { url in
                            Button { viewing = url } label: {
                                FileImage(url: url, maxPixel: 700, contentMode: .fill)
                                    .frame(width: 250, height: 140)
                                    .background(Theme.surface)
                                    .clipShape(RoundedRectangle(cornerRadius: 8, style: .continuous))
                            }
                            .buttonStyle(CardButtonStyle())
                        }
                    }
                }
            }
        }
    }

    private var aboutSection: some View {
        VStack(alignment: .leading, spacing: 10) {
            SectionTitle("About")
            Text(game.summary).font(.system(size: 15)).foregroundStyle(Theme.text.opacity(0.9))
                .frame(maxWidth: .infinity, alignment: .leading)
        }
    }

    private var settingsSection: some View {
        VStack(alignment: .leading, spacing: 12) {
            SectionTitle("Game settings")
            VStack(alignment: .leading, spacing: 16) {
                VStack(alignment: .leading, spacing: 8) {
                    Text("Touch controls").font(.system(size: 15, weight: .semibold))
                    Picker("Touch controls", selection: $touch) {
                        ForEach(GameSettings.TouchControls.allCases) { Text($0.title).tag($0) }
                    }
                    .pickerStyle(.segmented)
                    Text(touch == .auto ? "Hidden while a controller is connected." : touch == .always ? "Drawn even with a controller connected." : "Never drawn.")
                        .font(.caption).foregroundStyle(Theme.textDim)
                }
                if touch != .off {
                    VStack(alignment: .leading, spacing: 6) {
                        HStack { Text("Control opacity").font(.system(size: 15, weight: .semibold)); Spacer(); Text("\(Int(opacity * 100))%").foregroundStyle(Theme.textDim) }
                        Slider(value: $opacity, in: 0.15...1).tint(Theme.accentBright)
                    }
                    Toggle("Haptic feedback", isOn: $haptics).tint(Theme.accent).font(.system(size: 15, weight: .semibold))
                }
            }
            .padding(16).frame(maxWidth: .infinity, alignment: .leading).utsuroCard()
        }
    }

    private var detailsSection: some View {
        VStack(alignment: .leading, spacing: 12) {
            SectionTitle("Details")
            VStack(spacing: 0) {
                row("Publisher", game.publisher)
                if !game.version.isEmpty { row("Version", game.version) }
                if let id = game.titleId { row("Title ID", id) }
                if !game.executable.isEmpty { row("Executable", game.executable) }
                row("Installed size", gameSize > 0 ? ByteCountFormatter.string(fromByteCount: gameSize, countStyle: .file) : "…")
                row("Saves", saveSize > 0 ? ByteCountFormatter.string(fromByteCount: saveSize, countStyle: .file) : "None", last: true)
            }
            .utsuroCard()
        }
    }

    private func row(_ label: String, _ value: String, last: Bool = false) -> some View {
        VStack(spacing: 0) {
            HStack {
                Text(label).foregroundStyle(Theme.textDim)
                Spacer(minLength: 16)
                Text(value).foregroundStyle(Theme.text).multilineTextAlignment(.trailing).lineLimit(2)
            }
            .font(.system(size: 14))
            .padding(.horizontal, 16).padding(.vertical, 12)
            if !last { Divider().overlay(Theme.hairline).padding(.leading, 16) }
        }
    }

    private var manageSection: some View {
        VStack(spacing: 10) {
            Button { confirmSaves = true } label: { Label("Delete saves", systemImage: "trash") }
                .buttonStyle(SecondaryButtonStyle(tint: Theme.danger))
                .disabled(saveSize == 0)
            Button { confirmRemove = true } label: { Label("Remove game", systemImage: "xmark.bin") }
                .buttonStyle(SecondaryButtonStyle(tint: Theme.danger))
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
