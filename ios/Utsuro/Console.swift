// SPDX-License-Identifier: GPL-2.0-or-later
// The console look, as Husk draws it: a black (or near-white) page, flat cards, white-filled selections and a white Play button,
// the game's own artwork wherever there is room for it. No glass on content and no coloured washes; the accent is for small things.
import SwiftUI
import UIKit

// MARK: - palette

enum Console {
    private static func adaptive(dark: UIColor, light: UIColor) -> Color {
        Color(uiColor: UIColor { $0.userInterfaceStyle == .dark ? dark : light })
    }

    /// The page: black, or near-white.
    static let page = adaptive(dark: .black, light: UIColor(white: 0.965, alpha: 1))
    /// Cards and wells on the page.
    static let card = adaptive(dark: UIColor(white: 0.11, alpha: 1), light: UIColor(white: 0.905, alpha: 1))
    /// One step up from a card: the button beside Play.
    static let raised = adaptive(dark: UIColor(white: 0.16, alpha: 1), light: UIColor(white: 0.865, alpha: 1))
    static let hairline = adaptive(dark: UIColor(white: 1, alpha: 0.08), light: UIColor(white: 0, alpha: 0.08))
    /// Text, and the fill of what is selected: white in dark, black in light.
    static let text = adaptive(dark: .white, light: .black)
    /// What sits on `text` as a fill: Play's label.
    static let onText = adaptive(dark: .black, light: .white)
    static let textDim = adaptive(dark: UIColor(white: 1, alpha: 0.6), light: UIColor(white: 0, alpha: 0.55))
}

// MARK: - where things are

/// Which tab is up, the stacks inside Home and the Library, and the folder picker for adding a game: shared so Home's menu can
/// add a game or open the Library, and a game's page can be pushed from anywhere.
@MainActor
final class AppRouter: ObservableObject {
    static let shared = AppRouter()

    enum Tab: Hashable { case home, library, settings }

    @Published var tab = Tab.home
    @Published var home: [InstalledGame] = []
    @Published var library: [InstalledGame] = []
    @Published var addingGame = false

    func addGame() { addingGame = true }
}

// MARK: - JIT, in one line

@MainActor
enum JITControl {
    static var isOn: Bool { JITBootstrap.ready }

    /// How it was (or will be) turned on, in a word.
    static func via(_ jit: JITCoordinator) -> String {
        let m = jit.resolvedMethod
        return m == .builtIn ? "StikJIT" : m.title
    }

    /// Turns it on the way the JIT card would: StikJIT where Utsuro has it, else what this iPhone has.
    static func turnOn(_ jit: JITCoordinator) {
        if UtsuroBuiltInJIT.isAvailable { jit.method = .builtIn }
        jit.enable()
    }
}

// MARK: - a game's pictures

/// A game's tile: its square box art, edge to edge, with continuous corners -- the console's tile colour behind it in case the
/// picture has holes.
struct GameIcon: View {
    let game: InstalledGame
    let size: CGFloat

    var body: some View {
        ZStack {
            game.tileColor
            if let url = game.icon, let image = ThumbnailCache.image(at: url, maxPixel: max(160, size * 3)) {
                Image(uiImage: image).resizable().aspectRatio(contentMode: .fill)
            } else {
                Image(systemName: "gamecontroller.fill")
                    .font(.system(size: size * 0.36, weight: .semibold))
                    .foregroundStyle(.white.opacity(0.85))
            }
        }
        .frame(width: size, height: size)
        .clipShape(RoundedRectangle(cornerRadius: size * 0.225, style: .continuous))
        .overlay {
            RoundedRectangle(cornerRadius: size * 0.225, style: .continuous)
                .strokeBorder(Color.primary.opacity(0.08), lineWidth: 0.5)
        }
    }
}

/// What sits behind a game's name: its splash screen, else its tile art blown up and blurred.
struct GameBackdrop: View {
    let game: InstalledGame
    var pixels: CGFloat = 1200
    var fallback: Color = Console.card

    var body: some View {
        ZStack {
            fallback
            if let url = game.artwork, let image = ThumbnailCache.image(at: url, maxPixel: pixels) {
                Color.clear.overlay { Image(uiImage: image).resizable().aspectRatio(contentMode: .fill) }
            } else if let url = game.icon, let image = ThumbnailCache.image(at: url, maxPixel: 240) {
                Color.clear.overlay {
                    Image(uiImage: image).resizable().aspectRatio(contentMode: .fill)
                        .scaleEffect(1.6).blur(radius: 28).saturation(1.2)
                }
            }
        }
        .clipped()
        // Scaled up, a picture reaches past its frame; clipping hides that but does not stop it taking a neighbour's touches.
        .allowsHitTesting(false)
    }
}

/// Utsuro's mark: the app icon.
struct UtsuroMark: View {
    var size: CGFloat = 32

    var body: some View {
        Image("UtsuroIcon")
            .resizable()
            .scaledToFit()
            .frame(width: size, height: size)
            .clipShape(RoundedRectangle(cornerRadius: size * 0.225, style: .continuous))
    }
}

// MARK: - small pieces

struct StatusDot: View {
    let status: GameCompatibility
    var size: CGFloat = 7
    var body: some View { Circle().fill(status.color).frame(width: size, height: size) }
}

/// The big Play button: white on dark, black on light.
struct ConsolePlayButtonStyle: ButtonStyle {
    func makeBody(configuration: Configuration) -> some View {
        configuration.label
            .font(.system(size: 17, weight: .bold))
            .foregroundStyle(Console.onText)
            .frame(maxWidth: .infinity)
            .frame(height: 52)
            .background(Console.text, in: RoundedRectangle(cornerRadius: 14, style: .continuous))
            .opacity(configuration.isPressed ? 0.85 : 1)
            .scaleEffect(configuration.isPressed ? 0.98 : 1)
            .animation(.easeOut(duration: 0.12), value: configuration.isPressed)
    }
}

/// The square "⋯" beside Play.
struct MoreButtonLabel: View {
    var body: some View {
        Image(systemName: "ellipsis")
            .font(.system(size: 18, weight: .bold))
            .foregroundStyle(Console.text)
            .frame(width: 52, height: 52)
            .background(Console.raised, in: RoundedRectangle(cornerRadius: 14, style: .continuous))
    }
}

/// Work under way, said in one line.
struct BusyStrip: View {
    let text: String

    var body: some View {
        HStack(spacing: 12) {
            ProgressView()
            Text(text).font(.subheadline).foregroundStyle(Console.text).lineLimit(2)
            Spacer(minLength: 0)
        }
        .padding(14)
        .background(Console.card, in: RoundedRectangle(cornerRadius: 16, style: .continuous))
    }
}

/// A search field as the system draws one.
struct SearchField: View {
    @Binding var text: String
    let prompt: String
    @FocusState private var focused: Bool

    var body: some View {
        HStack(spacing: 8) {
            HStack(spacing: 6) {
                Image(systemName: "magnifyingglass")
                    .font(.system(size: 15, weight: .medium))
                    .foregroundStyle(.secondary)
                TextField(prompt, text: $text)
                    .focused($focused)
                    .submitLabel(.search)
                    .autocorrectionDisabled()
                    .textInputAutocapitalization(.never)
                if !text.isEmpty {
                    Button { text = "" } label: { Image(systemName: "xmark.circle.fill").foregroundStyle(.tertiary) }
                        .buttonStyle(.plain)
                        .accessibilityLabel("Clear")
                }
            }
            .padding(.horizontal, 10)
            .frame(height: 38)
            .background(Color(uiColor: .tertiarySystemFill), in: RoundedRectangle(cornerRadius: 12, style: .continuous))
            if focused {
                Button("Cancel") { text = ""; focused = false }
                    .transition(.move(edge: .trailing).combined(with: .opacity))
            }
        }
        .animation(.easeOut(duration: 0.2), value: focused)
    }
}

// MARK: - what can be done with a game

/// Play, its page, and removing it: the long press on a tile and Home's "⋯".
struct GameActions: View {
    let game: InstalledGame
    var showsDetails = true
    let open: (InstalledGame) -> Void
    let askRemove: () -> Void
    @ObservedObject private var session = Session.shared

    var body: some View {
        Button { session.play(game) } label: { Label("Play", systemImage: "play.fill") }
        if showsDetails {
            Button { open(game) } label: { Label("Details", systemImage: "info.circle") }
        }
        Divider()
        Button(role: .destructive) { askRemove() } label: { Label("Remove", systemImage: "trash") }
    }
}

/// Asking before a game is removed, and removing it.
private struct RemoveGameDialog: ViewModifier {
    let game: InstalledGame
    @Binding var isPresented: Bool

    func body(content: Content) -> some View {
        content.confirmationDialog("Remove \(game.title)?", isPresented: $isPresented, titleVisibility: .visible) {
            Button("Remove game, keep saves", role: .destructive) { GameLibrary.shared.remove(game, saves: false) }
            Button("Remove game and saves", role: .destructive) { GameLibrary.shared.remove(game, saves: true) }
        } message: {
            Text("The game's files are deleted from this iPhone. You can add the dump again later.")
        }
    }
}

/// Long-press on a game's tile.
private struct GameMenu: ViewModifier {
    let game: InstalledGame
    let open: (InstalledGame) -> Void
    @State private var confirmRemove = false

    func body(content: Content) -> some View {
        content
            .contextMenu { GameActions(game: game, open: open) { confirmRemove = true } }
            .removeGameDialog(game, isPresented: $confirmRemove)
    }
}

extension View {
    func gameMenu(_ game: InstalledGame, open: @escaping (InstalledGame) -> Void) -> some View {
        modifier(GameMenu(game: game, open: open))
    }

    func removeGameDialog(_ game: InstalledGame, isPresented: Binding<Bool>) -> some View {
        modifier(RemoveGameDialog(game: game, isPresented: isPresented))
    }
}

extension InstalledGame {
    /// "Last played 2 hours ago", or nothing for a game never started here.
    var lastPlayedText: String? {
        guard let date = GameSettings(gameID: id).lastPlayed else { return nil }
        if date.timeIntervalSinceNow > -60 { return "Last played just now" }
        let rel = RelativeDateTimeFormatter()
        rel.unitsStyle = .full
        return "Last played " + rel.localizedString(for: date, relativeTo: Date())
    }
}
