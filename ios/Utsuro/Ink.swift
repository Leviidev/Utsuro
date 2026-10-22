// SPDX-License-Identifier: GPL-2.0-or-later
// Utsuro's look. Ink: a near-black page with a breath of indigo, cards one step up, off-white text. Titles are set in the
// system serif (New York); labels in small spaced capitals. The motif is the logo's: an ensō, the open circle, which turns up
// as the Play button's ring, the JIT ring, and the rings on a game's page. Art is shown whole, never under a coloured wash.
import SwiftUI
import UIKit

// MARK: - palette

enum Ink {
    private static func adaptive(dark: UIColor, light: UIColor) -> Color {
        Color(uiColor: UIColor { $0.userInterfaceStyle == .dark ? dark : light })
    }
    private static func rgb(_ r: Int, _ g: Int, _ b: Int, _ a: CGFloat = 1) -> UIColor {
        UIColor(red: CGFloat(r) / 255, green: CGFloat(g) / 255, blue: CGFloat(b) / 255, alpha: a)
    }

    static let pageUI = UIColor { $0.userInterfaceStyle == .dark ? rgb(8, 8, 12) : rgb(244, 244, 247) }
    static let page = Color(uiColor: pageUI)
    static let card = adaptive(dark: rgb(19, 19, 27), light: .white)
    static let raised = adaptive(dark: rgb(29, 29, 40), light: rgb(233, 233, 240))
    static let hairline = adaptive(dark: UIColor(white: 1, alpha: 0.07), light: UIColor(white: 0, alpha: 0.08))
    static let text = adaptive(dark: rgb(244, 243, 255), light: rgb(16, 16, 24))
    /// What sits on `text` as a fill: the Play disc's glyph.
    static let onText = adaptive(dark: rgb(8, 8, 12), light: .white)
    static let dim = adaptive(dark: rgb(244, 243, 255, 0.55), light: rgb(16, 16, 24, 0.55))
    static let faint = adaptive(dark: rgb(244, 243, 255, 0.32), light: rgb(16, 16, 24, 0.32))
    /// The empty part of a ring.
    static let track = adaptive(dark: UIColor(white: 1, alpha: 0.08), light: UIColor(white: 0, alpha: 0.07))
}

extension Font {
    /// Titles: the system serif.
    static func title(_ size: CGFloat, weight: Font.Weight = .semibold) -> Font { .system(size: size, weight: weight, design: .serif) }
}

/// A label in small spaced capitals.
struct Caps: View {
    let text: String
    var color: Color = Ink.dim
    init(_ text: String, color: Color = Ink.dim) { self.text = text; self.color = color }
    var body: some View {
        Text(text.uppercased()).font(.system(size: 11, weight: .semibold)).tracking(1.4).foregroundStyle(color)
    }
}

/// A section's heading: its name in serif, a count, and an optional link on the right.
struct SectionHeading: View {
    let title: String
    var count: Int? = nil
    var link: String? = nil
    var action: (() -> Void)? = nil

    var body: some View {
        HStack(alignment: .firstTextBaseline, spacing: 8) {
            Text(title).font(.title(22)).foregroundStyle(Ink.text)
            if let count { Text("\(count)").font(.system(size: 15)).foregroundStyle(Ink.faint) }
            Spacer(minLength: 0)
            if let link, let action {
                Button(link, action: action).font(.system(size: 14, weight: .semibold)).foregroundStyle(Theme.accent)
            }
        }
    }
}

extension View {
    /// A card on the page.
    func inkCard(_ corner: CGFloat = 22) -> some View {
        background(Ink.card, in: RoundedRectangle(cornerRadius: corner, style: .continuous))
    }
}

// MARK: - the ensō

/// An open circle with round ends: the logo's stroke, as a ring.
struct EnsoRing: View {
    var color: Color
    var lineWidth: CGFloat = 3
    /// How much of the circle is drawn; the rest is the gap.
    var amount: CGFloat = 0.86
    /// Where the stroke starts, in degrees from three o'clock.
    var start: Double = -60

    var body: some View {
        Circle()
            .trim(from: 0, to: amount)
            .stroke(color, style: StrokeStyle(lineWidth: lineWidth, lineCap: .round))
            .rotationEffect(.degrees(start))
            .padding(lineWidth / 2)
    }
}

/// Play: a disc in an ensō ring.
struct RingPlayButton: View {
    var size: CGFloat = 66
    let action: () -> Void

    var body: some View {
        Button(action: action) {
            ZStack {
                EnsoRing(color: Theme.accent, lineWidth: 2.5)
                Circle().fill(Ink.text).padding(size * 0.11)
                Image(systemName: "play.fill")
                    .font(.system(size: size * 0.28, weight: .bold))
                    .foregroundStyle(Ink.onText)
                    .offset(x: size * 0.025)
            }
            .frame(width: size, height: size)
            .contentShape(Circle())
        }
        .buttonStyle(PressScale())
        .accessibilityLabel("Play")
    }
}

/// A small round Play for a row.
struct MiniPlayButton: View {
    let action: () -> Void

    var body: some View {
        Button(action: action) {
            Image(systemName: "play.fill")
                .font(.system(size: 12, weight: .bold))
                .foregroundStyle(Theme.accent)
                .offset(x: 1)
                .frame(width: 36, height: 36)
                .background(Ink.raised, in: Circle())
        }
        .buttonStyle(PressScale())
        .accessibilityLabel("Play")
    }
}

/// A value in a ring, and what it is under it.
struct StatRing: View {
    let value: String
    let label: String
    let progress: Double
    var color: Color = Theme.accent

    var body: some View {
        VStack(spacing: 8) {
            ZStack {
                Circle().stroke(Ink.track, lineWidth: 5)
                Circle()
                    .trim(from: 0, to: max(0.02, min(progress, 1)))
                    .stroke(color, style: StrokeStyle(lineWidth: 5, lineCap: .round))
                    .rotationEffect(.degrees(-90))
                Text(value)
                    .font(.system(size: 13, weight: .bold))
                    .foregroundStyle(color == Theme.accent || color == Ink.dim ? Ink.text : color)
                    .minimumScaleFactor(0.7)
                    .lineLimit(1)
                    .padding(.horizontal, 8)
            }
            .frame(width: 70, height: 70)
            Caps(label)
        }
        .frame(maxWidth: .infinity)
    }
}

/// The JIT ring: closed and green when it is on, open and orange when it is off.
struct JITRing: View {
    var size: CGFloat = 26
    var body: some View {
        EnsoRing(color: JITControl.isOn ? .green : .orange, lineWidth: size * 0.115, amount: JITControl.isOn ? 0.88 : 0.55, start: -70)
            .frame(width: size, height: size)
    }
}

/// The main action of a page: a full-width capsule in the accent.
struct PillButtonStyle: ButtonStyle {
    func makeBody(configuration: Configuration) -> some View {
        configuration.label
            .font(.system(size: 17, weight: .bold))
            .foregroundStyle(.white)
            .frame(maxWidth: .infinity)
            .frame(height: 54)
            .background(Theme.accent, in: Capsule())
            .opacity(configuration.isPressed ? 0.85 : 1)
            .scaleEffect(configuration.isPressed ? 0.98 : 1)
            .animation(.easeOut(duration: 0.12), value: configuration.isPressed)
    }
}

/// Anything tappable that should give a little under the finger.
struct PressScale: ButtonStyle {
    func makeBody(configuration: Configuration) -> some View {
        configuration.label
            .scaleEffect(configuration.isPressed ? 0.95 : 1)
            .opacity(configuration.isPressed ? 0.9 : 1)
            .animation(.easeOut(duration: 0.14), value: configuration.isPressed)
    }
}

/// A round glyph button on the page: add, more.
struct RoundGlyphButton: View {
    let systemImage: String
    let action: () -> Void

    var body: some View {
        Button(action: action) {
            Image(systemName: systemImage)
                .font(.system(size: 16, weight: .semibold))
                .foregroundStyle(Theme.accent)
                .frame(width: 38, height: 38)
                .background(Ink.raised, in: Circle())
        }
        .buttonStyle(PressScale())
    }
}

// MARK: - where things are

/// Which tab is up, the stacks inside Home and the Library, and the folder picker for adding a game.
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

/// A game's box art, edge to edge in a rounded square, with the console's tile colour behind it in case the picture has holes.
struct GameIcon: View {
    let game: InstalledGame
    let size: CGFloat
    var corner: CGFloat? = nil

    var body: some View {
        let radius = corner ?? size * 0.27
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
        .clipShape(RoundedRectangle(cornerRadius: radius, style: .continuous))
        .overlay {
            RoundedRectangle(cornerRadius: radius, style: .continuous).strokeBorder(Ink.hairline, lineWidth: 0.5)
        }
    }
}

/// A game's splash screen, filling whatever frame it is given; its box art blown up and blurred when it has none.
struct GameBackdrop: View {
    let game: InstalledGame
    var pixels: CGFloat = 1200
    var fallback: Color = Ink.card

    var body: some View {
        ZStack {
            fallback
            if let url = game.artwork, let image = ThumbnailCache.image(at: url, maxPixel: pixels) {
                Color.clear.overlay { Image(uiImage: image).resizable().aspectRatio(contentMode: .fill) }
            } else if let url = game.icon, let image = ThumbnailCache.image(at: url, maxPixel: 240) {
                Color.clear.overlay {
                    Image(uiImage: image).resizable().aspectRatio(contentMode: .fill).scaleEffect(1.6).blur(radius: 28)
                }
            }
        }
        .clipped()
        // Scaled up, a picture reaches past its frame; clipping hides that but does not stop it taking a neighbour's touches.
        .allowsHitTesting(false)
    }
}

/// The splash, whole: 16:9 with rounded corners.
struct SplashCard: View {
    let game: InstalledGame

    var body: some View {
        GameBackdrop(game: game, pixels: 1400)
            .aspectRatio(16.0 / 9.0, contentMode: .fit)
            .clipShape(RoundedRectangle(cornerRadius: 26, style: .continuous))
            .overlay(RoundedRectangle(cornerRadius: 26, style: .continuous).stroke(Ink.hairline, lineWidth: 0.5))
    }
}

/// The ensō on its own, as in the wordmark.
struct UtsuroMark: View {
    var size: CGFloat = 30
    var body: some View { Image("UtsuroMark").resizable().scaledToFit().frame(width: size, height: size) }
}

/// The app icon.
struct UtsuroAppIcon: View {
    var size: CGFloat = 78
    var body: some View {
        Image("UtsuroIcon").resizable().scaledToFit().frame(width: size, height: size)
            .clipShape(RoundedRectangle(cornerRadius: size * 0.245, style: .continuous))
    }
}

/// "utsuro", as the wordmark sets it.
struct Wordmark: View {
    var size: CGFloat = 23
    var body: some View {
        Text("utsuro").font(.system(size: size, weight: .bold)).tracking(-size * 0.026).foregroundStyle(Ink.text)
    }
}

// MARK: - small pieces

struct StatusDot: View {
    let status: GameCompatibility
    var size: CGFloat = 7
    var body: some View { Circle().fill(status.color).frame(width: size, height: size) }
}

/// Work under way, said in one line.
struct BusyStrip: View {
    let text: String

    var body: some View {
        HStack(spacing: 12) {
            ProgressView()
            Text(text).font(.subheadline).foregroundStyle(Ink.text).lineLimit(2)
            Spacer(minLength: 0)
        }
        .padding(14)
        .inkCard(18)
    }
}

/// The search field: a capsule.
struct SearchField: View {
    @Binding var text: String
    let prompt: String
    @FocusState private var focused: Bool

    var body: some View {
        HStack(spacing: 8) {
            HStack(spacing: 8) {
                Image(systemName: "magnifyingglass").font(.system(size: 15, weight: .medium)).foregroundStyle(Ink.faint)
                TextField(prompt, text: $text)
                    .focused($focused)
                    .submitLabel(.search)
                    .autocorrectionDisabled()
                    .textInputAutocapitalization(.never)
                if !text.isEmpty {
                    Button { text = "" } label: { Image(systemName: "xmark.circle.fill").foregroundStyle(Ink.faint) }
                        .buttonStyle(.plain)
                        .accessibilityLabel("Clear")
                }
            }
            .padding(.horizontal, 14)
            .frame(height: 40)
            .background(Ink.card, in: Capsule())
            if focused {
                Button("Cancel") { text = ""; focused = false }
                    .transition(.move(edge: .trailing).combined(with: .opacity))
            }
        }
        .animation(.easeOut(duration: 0.2), value: focused)
    }
}

// MARK: - what can be done with a game

/// Play, its page, and removing it: the long press on a game, and a page's "⋯".
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
    var after: () -> Void = {}

    func body(content: Content) -> some View {
        content.confirmationDialog("Remove \(game.title)?", isPresented: $isPresented, titleVisibility: .visible) {
            Button("Remove game, keep saves", role: .destructive) { GameLibrary.shared.remove(game, saves: false); after() }
            Button("Remove game and saves", role: .destructive) { GameLibrary.shared.remove(game, saves: true); after() }
        } message: {
            Text("The game's files are deleted from this iPhone. You can add the dump again later.")
        }
    }
}

/// Long-press on a game.
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

    func removeGameDialog(_ game: InstalledGame, isPresented: Binding<Bool>, after: @escaping () -> Void = {}) -> some View {
        modifier(RemoveGameDialog(game: game, isPresented: isPresented, after: after))
    }
}

extension InstalledGame {
    private var settings: GameSettings { GameSettings(gameID: id) }

    /// "yesterday", "3 days ago"; nil for a game never started here.
    var lastPlayedAgo: String? {
        guard let date = settings.lastPlayed else { return nil }
        if date.timeIntervalSinceNow > -60 { return "just now" }
        let rel = RelativeDateTimeFormatter()
        rel.unitsStyle = .full
        rel.dateTimeStyle = .named
        return rel.localizedString(for: date, relativeTo: Date())
    }

    /// "2 h 14 min"; nil under a minute.
    var timePlayed: String? {
        let seconds = settings.playSeconds
        guard seconds >= 60 else { return nil }
        let hours = Int(seconds) / 3600, minutes = (Int(seconds) % 3600) / 60
        return hours > 0 ? "\(hours) h \(minutes) min" : "\(minutes) min"
    }

    /// How far along the status ring goes.
    var readiness: Double {
        switch compatibility {
        case .plays: return 0.92
        case .ingame: return 0.75
        case .opens: return 0.45
        case .broken: return 0.15
        default: return 0.04
        }
    }
}
