import SwiftUI
import UIKit
import UniformTypeIdentifiers

/// The app: Home, Library and Settings behind the system's tab bar, and the game itself over all of it when one is playing.
struct RootView: View {
    @ObservedObject private var session = Session.shared
    @ObservedObject private var jit = JITCoordinator.shared
    @ObservedObject private var library = GameLibrary.shared
    @ObservedObject private var router = AppRouter.shared
    @Environment(\.scenePhase) private var scenePhase

    var body: some View {
        Group {
            if let game = session.playing {
                GameScreen(game: game) { session.leave() }
            } else {
                TabView(selection: $router.tab) {
                    HomeView()
                        .tabItem { Label("Home", systemImage: "house.fill") }
                        .tag(AppRouter.Tab.home)
                    LibraryView()
                        .tabItem { Label("Library", systemImage: "square.grid.2x2.fill") }
                        .tag(AppRouter.Tab.library)
                    SettingsView()
                        .tabItem { Label("Settings", systemImage: "gearshape.fill") }
                        .tag(AppRouter.Tab.settings)
                }
            }
        }
        .sheet(isPresented: $jit.showSetup) { JITSetupFlow() }
        .fileImporter(isPresented: $router.addingGame, allowedContentTypes: [.folder]) { result in
            switch result {
            case .success(let url):
                library.importDump(from: url)
                router.tab = .library
            case .failure(let failure):
                library.importError = failure.localizedDescription
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
}

// MARK: - Home

/// Home as a console's home screen: the selected game's artwork across the top, a shelf of every game (the one played last
/// first), what the selected one is and Play, then what was added lately.
struct HomeView: View {
    @ObservedObject private var router = AppRouter.shared
    @ObservedObject private var library = GameLibrary.shared
    @ObservedObject private var session = Session.shared
    @ObservedObject private var jit = JITCoordinator.shared
    /// The tile the shelf has selected; nil means the first (the one played last).
    @State private var selectedID: String?

    /// Most recently played first; never-played ones after, by name.
    private var games: [InstalledGame] {
        library.games.sorted { a, b in
            switch (GameSettings(gameID: a.id).lastPlayed, GameSettings(gameID: b.id).lastPlayed) {
            case let (x?, y?): return x > y
            case (_?, nil): return true
            case (nil, _?): return false
            default: return a.title.localizedCaseInsensitiveCompare(b.title) == .orderedAscending
            }
        }
    }

    var body: some View {
        NavigationStack(path: $router.home) {
            GeometryReader { geo in
                let games = self.games
                let selected = games.first { $0.id == selectedID } ?? games.first
                let heroHeight = (geo.size.height + geo.safeAreaInsets.top + geo.safeAreaInsets.bottom) * 0.55
                ZStack(alignment: .top) {
                    Console.page.ignoresSafeArea()
                    HomeHero(game: selected, width: geo.size.width, height: heroHeight)
                        .ignoresSafeArea(edges: .top)

                    ScrollView {
                        VStack(alignment: .leading, spacing: 0) {
                            topBar
                                .padding(.horizontal, 20)
                                .padding(.top, 6)
                                .environment(\.colorScheme, .dark)
                            notices
                                .padding(.horizontal, 20)
                                .padding(.top, 10)
                                // over the artwork: drawn as on dark in either appearance
                                .environment(\.colorScheme, .dark)

                            if let selected {
                                Color.clear.frame(height: max(12, heroHeight - geo.safeAreaInsets.top - 62 - 150))
                                shelf(games, selected: selected)
                                details(selected)
                                    .padding(.horizontal, 20)
                                    .padding(.top, 14)
                                recentlyAdded(games)
                                    .padding(.top, 30)
                            } else {
                                Color.clear.frame(height: max(12, heroHeight - geo.safeAreaInsets.top - 62 - 200))
                                WelcomeCard().padding(.horizontal, 20)
                            }
                        }
                        .padding(.bottom, 24)
                        .id(jit.attachGeneration)
                    }
                    .scrollIndicators(.hidden)
                }
            }
            .background(Console.page.ignoresSafeArea())
            .toolbar(.hidden, for: .navigationBar)
            .navigationDestination(for: InstalledGame.self) { GamePage(game: $0) }
        }
    }

    // MARK: top bar

    private var topBar: some View {
        HStack(spacing: 10) {
            Text("Utsuro")
                .font(.system(size: 24, weight: .heavy, design: .rounded))
                .foregroundStyle(.white)
                .fixedSize()
            Spacer(minLength: 4)
            HomeAvatar()
        }
        .frame(height: 44)
    }

    /// An import under way, and JIT when it is off.
    @ViewBuilder
    private var notices: some View {
        VStack(spacing: 8) {
            if library.importing { BusyStrip(text: library.importProgress ?? "Copying the game…") }
            if !JITControl.isOn { JITNotice() }
        }
    }

    // MARK: shelf

    private func shelf(_ games: [InstalledGame], selected: InstalledGame) -> some View {
        ScrollViewReader { proxy in
            ScrollView(.horizontal, showsIndicators: false) {
                HStack(alignment: .center, spacing: 12) {
                    ForEach(games) { game in
                        let isSelected = game == selected
                        Button {
                            if isSelected {
                                session.play(game)
                            } else {
                                withAnimation(.snappy(duration: 0.25)) { selectedID = game.id }
                            }
                        } label: {
                            ShelfTile(game: game, selected: isSelected)
                        }
                        .buttonStyle(CardButtonStyle())
                        .id(game.id)
                        .accessibilityLabel(game.title)
                        .accessibilityHint(isSelected ? "Plays it" : "Selects it")
                        .gameMenu(game) { router.home.append($0) }
                    }
                    if games.count > 1 {
                        Button { router.tab = .library } label: {
                            VStack(spacing: 6) {
                                Image(systemName: "square.grid.2x2.fill").font(.system(size: 20, weight: .semibold))
                                Text("All").font(.system(size: 12, weight: .semibold))
                            }
                            .foregroundStyle(Console.text.opacity(0.8))
                            .frame(width: 72, height: 72)
                            .background(Console.card, in: RoundedRectangle(cornerRadius: 18, style: .continuous))
                        }
                        .buttonStyle(CardButtonStyle())
                        .accessibilityLabel("Open the Library")
                    }
                }
                .padding(.horizontal, 20)
                .frame(height: 112)
            }
            .onChange(of: selectedID) { id in
                guard let id else { return }
                withAnimation(.snappy(duration: 0.3)) { proxy.scrollTo(id, anchor: .center) }
            }
        }
    }

    // MARK: details

    private func details(_ game: InstalledGame) -> some View {
        VStack(alignment: .leading, spacing: 6) {
            HStack(spacing: 8) {
                Text("XBOX ONE")
                    .font(.system(size: 12, weight: .bold))
                    .tracking(0.8)
                    .foregroundStyle(Console.textDim)
                let status = game.compatibility
                if status != .untested {
                    HStack(spacing: 5) {
                        StatusDot(status: status)
                        Text(status.label)
                            .font(.system(size: 12, weight: .semibold))
                            .foregroundStyle(status.color)
                    }
                }
            }
            Text(game.title)
                .font(.system(size: 32, weight: .heavy))
                .foregroundStyle(Console.text)
                .lineLimit(2)
                .minimumScaleFactor(0.7)
                .fixedSize(horizontal: false, vertical: true)
            if let used = game.lastPlayedText {
                Text(used)
                    .font(.system(size: 15))
                    .foregroundStyle(Console.textDim)
            }
            HStack(spacing: 10) {
                Button { session.play(game) } label: { Label("Play", systemImage: "play.fill") }
                    .buttonStyle(ConsolePlayButtonStyle())
                    .accessibilityLabel("Play " + game.title)
                HomeMoreButton(game: game)
            }
            .padding(.top, 10)
            if let error = session.launchError {
                Text(error)
                    .font(.footnote)
                    .foregroundStyle(.orange)
                    .fixedSize(horizontal: false, vertical: true)
                    .padding(.top, 4)
            }
        }
        .frame(maxWidth: .infinity, alignment: .leading)
        .animation(nil, value: game.id)
    }

    // MARK: recently added

    @ViewBuilder
    private func recentlyAdded(_ games: [InstalledGame]) -> some View {
        let added = games.compactMap { g in g.addedDate.map { (g, $0) } }
            .sorted { $0.1 > $1.1 }
            .prefix(10)
            .map(\.0)
        if !added.isEmpty {
            VStack(alignment: .leading, spacing: 12) {
                Text("Recently added")
                    .font(.system(size: 20, weight: .bold))
                    .foregroundStyle(Console.text)
                    .padding(.horizontal, 20)
                ScrollView(.horizontal, showsIndicators: false) {
                    HStack(spacing: 12) {
                        ForEach(added) { game in
                            NavigationLink(value: game) { WideCard(game: game) }
                                .buttonStyle(CardButtonStyle())
                                .accessibilityLabel(game.title)
                                .gameMenu(game) { router.home.append($0) }
                        }
                    }
                    .padding(.horizontal, 20)
                }
            }
        }
    }
}

/// What fills the top of Home: the selected game's splash art, dimmed with plain black and faded into the page at its bottom edge.
private struct HomeHero: View {
    let game: InstalledGame?
    let width: CGFloat
    let height: CGFloat

    var body: some View {
        ZStack {
            Color.black
            if let game {
                GameBackdrop(game: game, pixels: 1600, fallback: .black)
                    .id(game.id)
                    .transition(.opacity)
            }
            Color.black.opacity(0.45)
            VStack(spacing: 0) {
                Spacer(minLength: 0)
                LinearGradient(colors: [.clear, Console.page], startPoint: .top, endPoint: .bottom)
                    .frame(height: height * 0.32)
            }
        }
        .frame(width: width, height: height)
        .clipped()
        .allowsHitTesting(false)
        .animation(.easeInOut(duration: 0.35), value: game?.id)
    }
}

/// One tile on Home's shelf: the box art, larger with a white ring when selected.
private struct ShelfTile: View {
    let game: InstalledGame
    let selected: Bool

    var body: some View {
        let size: CGFloat = selected ? 92 : 72
        GameIcon(game: game, size: size)
            .padding(selected ? 4 : 0)
            .overlay {
                if selected {
                    RoundedRectangle(cornerRadius: size * 0.225 + 4, style: .continuous)
                        .strokeBorder(Console.text, lineWidth: 3)
                }
            }
            .shadow(color: .black.opacity(selected ? 0.5 : 0.3), radius: selected ? 12 : 6, y: 4)
            .opacity(selected ? 1 : 0.85)
            .contentShape(Rectangle())
    }
}

/// A wide card in "Recently added": the game's splash, its tile and its name.
private struct WideCard: View {
    let game: InstalledGame

    var body: some View {
        ZStack(alignment: .bottomLeading) {
            GameBackdrop(game: game, pixels: 800)
            Color.black.opacity(0.35)
            HStack(spacing: 10) {
                GameIcon(game: game, size: 38)
                VStack(alignment: .leading, spacing: 2) {
                    Text(game.title)
                        .font(.system(size: 15, weight: .semibold))
                        .foregroundStyle(.white)
                        .lineLimit(1)
                    Text("Xbox One")
                        .font(.system(size: 12))
                        .foregroundStyle(.white.opacity(0.7))
                        .lineLimit(1)
                }
                Spacer(minLength: 0)
                if game.compatibility != .untested { StatusDot(status: game.compatibility, size: 8) }
            }
            .padding(12)
        }
        .frame(width: 250, height: 140)
        .clipShape(RoundedRectangle(cornerRadius: 16, style: .continuous))
        .overlay(RoundedRectangle(cornerRadius: 16, style: .continuous).stroke(Console.hairline, lineWidth: 0.5))
        .contentShape(RoundedRectangle(cornerRadius: 16, style: .continuous))
    }
}

/// The square "⋯" beside Play: the same actions as a long press on the tile.
private struct HomeMoreButton: View {
    let game: InstalledGame
    @ObservedObject private var router = AppRouter.shared
    @State private var confirmRemove = false

    var body: some View {
        Menu {
            GameActions(game: game, open: { router.home.append($0) }) { confirmRemove = true }
        } label: {
            MoreButtonLabel()
        }
        .accessibilityLabel("More for \(game.title)")
        .removeGameDialog(game, isPresented: $confirmRemove)
    }
}

/// The round button in Home's corner: adding a game, JIT and Settings. Its dot is JIT: green on, orange off.
private struct HomeAvatar: View {
    @ObservedObject private var router = AppRouter.shared
    @ObservedObject private var jit = JITCoordinator.shared

    var body: some View {
        Menu {
            Button { router.addGame() } label: { Label("Add a Game", systemImage: "plus") }
            Divider()
            Button {
                if JITControl.isOn { jit.showSetup = true } else { JITControl.turnOn(jit) }
            } label: {
                Label(JITControl.isOn ? "JIT On (\(JITControl.via(jit)))" : "Turn On JIT", systemImage: "bolt.fill")
            }
            .disabled(jit.busy)
            Button { router.tab = .settings } label: { Label("Settings", systemImage: "gearshape") }
        } label: {
            ZStack(alignment: .topTrailing) {
                Image(systemName: "person.fill")
                    .font(.system(size: 15, weight: .semibold))
                    .foregroundStyle(.white)
                    .frame(width: 36, height: 36)
                    .background(Console.raised, in: Circle())
                    .overlay(Circle().stroke(Color.white.opacity(0.15), lineWidth: 0.5))
                Circle()
                    .fill(JITControl.isOn ? Color.green : Color.orange)
                    .frame(width: 10, height: 10)
                    .overlay(Circle().stroke(Color.black, lineWidth: 2))
                    .offset(x: 1, y: -1)
            }
        }
        .accessibilityLabel("Menu")
    }
}

/// JIT is off: say so, and the way to turn it on.
private struct JITNotice: View {
    @ObservedObject private var jit = JITCoordinator.shared

    var body: some View {
        VStack(alignment: .leading, spacing: 6) {
            Button { JITControl.turnOn(jit) } label: {
                HStack(spacing: 10) {
                    Image(systemName: "bolt.fill")
                        .font(.system(size: 14, weight: .bold))
                        .foregroundStyle(.orange)
                    VStack(alignment: .leading, spacing: 1) {
                        Text("JIT is off").font(.system(size: 14, weight: .semibold)).foregroundStyle(.white)
                        Text(jit.busy ? (jit.status ?? "Turning on…") : "Games need it. Tap to turn it on.")
                            .font(.system(size: 12)).foregroundStyle(Console.textDim).lineLimit(1)
                    }
                    Spacer(minLength: 0)
                    if jit.busy { ProgressView() } else {
                        Text("Turn On")
                            .font(.system(size: 13, weight: .bold))
                            .foregroundStyle(.black)
                            .padding(.horizontal, 12).frame(height: 28)
                            .background(Color.white, in: Capsule())
                    }
                }
                .padding(.horizontal, 12).padding(.vertical, 10)
                .background(Color.black.opacity(0.55), in: RoundedRectangle(cornerRadius: 14, style: .continuous))
                .overlay(RoundedRectangle(cornerRadius: 14, style: .continuous).stroke(Console.hairline, lineWidth: 0.5))
            }
            .buttonStyle(CardButtonStyle())
            .disabled(jit.busy)
            if let error = jit.error {
                Text(error).font(.caption).foregroundStyle(.orange).fixedSize(horizontal: false, vertical: true)
            }
        }
    }
}

/// The first time: nothing added yet, and how to start.
private struct WelcomeCard: View {
    @ObservedObject private var router = AppRouter.shared

    var body: some View {
        VStack(alignment: .leading, spacing: 16) {
            UtsuroMark(size: 60)
            VStack(alignment: .leading, spacing: 6) {
                Text("Add your first game")
                    .font(.system(size: 26, weight: .heavy))
                    .foregroundStyle(Console.text)
                Text("Utsuro runs a decrypted Xbox One dump: the folder that holds Mount and EmbeddedXvd. Pick it and Utsuro copies "
                   + "it in, or put it in On My iPhone › Utsuro › Games with the Files app.")
                    .font(.subheadline)
                    .foregroundStyle(Console.textDim)
                    .fixedSize(horizontal: false, vertical: true)
            }
            Button { router.addGame() } label: { Text("Add a Game") }
                .buttonStyle(ConsolePlayButtonStyle())
        }
        .padding(20)
        .background(Console.card, in: RoundedRectangle(cornerRadius: 22, style: .continuous))
    }
}
