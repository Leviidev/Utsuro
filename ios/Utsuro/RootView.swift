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
                        .tabItem { Label("Home", image: "TabEnso") }
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

/// Home: the game to go back to, whole, with Play in its ring; then the rest as a list, each one a tap from playing.
struct HomeView: View {
    @ObservedObject private var router = AppRouter.shared
    @ObservedObject private var library = GameLibrary.shared
    @ObservedObject private var session = Session.shared
    @ObservedObject private var jit = JITCoordinator.shared

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
            ScrollView {
                let games = self.games
                VStack(alignment: .leading, spacing: 0) {
                    header
                        .padding(.horizontal, 2)
                        .padding(.bottom, 14)

                    if library.importing { BusyStrip(text: library.importProgress ?? "Copying the game…").padding(.bottom, 12) }
                    if !JITControl.isOn { JITStrip().padding(.bottom, 16) }

                    if let first = games.first {
                        featured(first)
                        if let error = session.launchError {
                            Text(error)
                                .font(.footnote)
                                .foregroundStyle(.orange)
                                .fixedSize(horizontal: false, vertical: true)
                                .padding(.top, 10)
                                .padding(.horizontal, 4)
                        }
                        if games.count > 1 { others(Array(games.dropFirst())) }
                    } else {
                        WelcomeCard()
                    }
                }
                .padding(.horizontal, 16)
                .padding(.top, 4)
                .padding(.bottom, 28)
                .id(jit.attachGeneration)
            }
            .scrollIndicators(.hidden)
            .background(Ink.page.ignoresSafeArea())
            .toolbar(.hidden, for: .navigationBar)
            .navigationDestination(for: InstalledGame.self) { GamePage(game: $0) }
        }
    }

    // MARK: header

    private var header: some View {
        HStack(spacing: 9) {
            UtsuroMark(size: 30)
            Wordmark()
            Spacer(minLength: 8)
            HomeMenu()
        }
        .frame(height: 44)
    }

    // MARK: the game to go back to

    private func featured(_ game: InstalledGame) -> some View {
        VStack(alignment: .leading, spacing: 0) {
            Caps(GameSettings(gameID: game.id).lastPlayed == nil ? "Start playing" : "Continue playing")
                .padding(.horizontal, 4)
                .padding(.bottom, 10)
            Button { router.home.append(game) } label: { SplashCard(game: game) }
                .buttonStyle(PressScale())
                .gameMenu(game) { router.home.append($0) }
                .accessibilityLabel(game.title)
            HStack(alignment: .center, spacing: 12) {
                VStack(alignment: .leading, spacing: 6) {
                    Text(game.title)
                        .font(.title(30))
                        .foregroundStyle(Ink.text)
                        .lineLimit(2)
                        .minimumScaleFactor(0.75)
                    statusLine(game, detail: game.timePlayed.map { "\($0) played" } ?? game.publisher)
                }
                Spacer(minLength: 8)
                RingPlayButton { session.play(game) }
            }
            .padding(.top, 14)
            .padding(.horizontal, 4)
        }
    }

    // MARK: the rest

    private func others(_ games: [InstalledGame]) -> some View {
        VStack(alignment: .leading, spacing: 12) {
            SectionHeading(title: "Your games", count: library.games.count, link: "See all") { router.tab = .library }
                .padding(.horizontal, 4)
            VStack(spacing: 0) {
                ForEach(Array(games.prefix(6).enumerated()), id: \.element.id) { index, game in
                    if index > 0 { Rectangle().fill(Ink.hairline).frame(height: 0.5).padding(.leading, 83).padding(.trailing, 14) }
                    GameRow(game: game) { router.home.append(game) } play: { session.play(game) }
                }
            }
            .padding(.vertical, 4)
            .inkCard(24)
        }
        .padding(.top, 28)
    }

    private func statusLine(_ game: InstalledGame, detail: String?) -> some View {
        HStack(spacing: 5) {
            if game.compatibility != .untested {
                StatusDot(status: game.compatibility)
                Text(game.compatibility.label)
            }
            if let detail {
                Text((game.compatibility != .untested ? "· " : "") + detail)
            }
        }
        .font(.system(size: 13))
        .foregroundStyle(Ink.dim)
        .lineLimit(1)
    }
}

/// A game in Home's list: its box art, name and how it runs, and a Play of its own.
private struct GameRow: View {
    let game: InstalledGame
    let open: () -> Void
    let play: () -> Void
    @ObservedObject private var router = AppRouter.shared

    var body: some View {
        HStack(spacing: 13) {
            Button(action: open) {
                HStack(spacing: 13) {
                    GameIcon(game: game, size: 56)
                    VStack(alignment: .leading, spacing: 3) {
                        Text(game.title).font(.system(size: 16, weight: .semibold)).foregroundStyle(Ink.text).lineLimit(1)
                        HStack(spacing: 5) {
                            if game.compatibility != .untested { StatusDot(status: game.compatibility) }
                            Text([game.compatibility != .untested ? game.compatibility.label : nil, game.lastPlayedAgo ?? "never played"]
                                .compactMap { $0 }.joined(separator: " · "))
                        }
                        .font(.system(size: 12.5))
                        .foregroundStyle(Ink.dim)
                        .lineLimit(1)
                    }
                    Spacer(minLength: 0)
                }
                .contentShape(Rectangle())
            }
            .buttonStyle(PressScale())
            .gameMenu(game) { router.home.append($0) }
            MiniPlayButton(action: play)
        }
        .padding(.horizontal, 14)
        .padding(.vertical, 10)
    }
}

/// The JIT ring in Home's corner, and what it opens: turning JIT on, adding a game, Settings.
private struct HomeMenu: View {
    @ObservedObject private var router = AppRouter.shared
    @ObservedObject private var jit = JITCoordinator.shared

    var body: some View {
        Menu {
            Button {
                if JITControl.isOn { jit.showSetup = true } else { JITControl.turnOn(jit) }
            } label: {
                Label(JITControl.isOn ? "JIT is on (\(JITControl.via(jit)))" : "Turn On JIT", systemImage: "bolt.fill")
            }
            .disabled(jit.busy)
            Button { router.addGame() } label: { Label("Add a Game", systemImage: "plus") }
            Button { router.tab = .settings } label: { Label("Settings", systemImage: "gearshape") }
        } label: {
            JITRing(size: 24)
                .frame(width: 38, height: 38)
                .background(Ink.raised, in: Circle())
        }
        .accessibilityLabel(JITControl.isOn ? "JIT is on. Menu" : "JIT is off. Menu")
    }
}

/// JIT is off: the open ring, what that means, and the way to turn it on.
private struct JITStrip: View {
    @ObservedObject private var jit = JITCoordinator.shared

    var body: some View {
        VStack(alignment: .leading, spacing: 6) {
            HStack(spacing: 12) {
                JITRing(size: 26)
                VStack(alignment: .leading, spacing: 1) {
                    Text("JIT is off").font(.system(size: 15, weight: .semibold)).foregroundStyle(Ink.text)
                    Text(jit.busy ? (jit.status ?? "Turning on…") : "Games need it to run.")
                        .font(.system(size: 12.5)).foregroundStyle(Ink.dim).lineLimit(1)
                }
                Spacer(minLength: 0)
                if jit.busy {
                    ProgressView()
                } else {
                    Button { JITControl.turnOn(jit) } label: {
                        Text("Turn On")
                            .font(.system(size: 14, weight: .bold))
                            .foregroundStyle(.white)
                            .padding(.horizontal, 14).frame(height: 32)
                            .background(Theme.accent, in: Capsule())
                    }
                    .buttonStyle(PressScale())
                }
            }
            if let error = jit.error {
                Text(error).font(.caption).foregroundStyle(.orange).fixedSize(horizontal: false, vertical: true)
            }
        }
        .padding(.horizontal, 14).padding(.vertical, 12)
        .inkCard(20)
    }
}

/// The first time: nothing added yet, and how to start.
private struct WelcomeCard: View {
    @ObservedObject private var router = AppRouter.shared

    var body: some View {
        VStack(alignment: .leading, spacing: 16) {
            UtsuroMark(size: 52)
            VStack(alignment: .leading, spacing: 8) {
                Text("Add your first game").font(.title(28)).foregroundStyle(Ink.text)
                Text("Utsuro runs a decrypted Xbox One dump: the folder that holds Mount and EmbeddedXvd. Pick it and Utsuro copies "
                   + "it in, or put it in On My iPhone › Utsuro › Games with the Files app.")
                    .font(.subheadline)
                    .foregroundStyle(Ink.dim)
                    .fixedSize(horizontal: false, vertical: true)
            }
            Button { router.addGame() } label: { Label("Add a Game", systemImage: "plus") }
                .buttonStyle(PillButtonStyle())
        }
        .padding(20)
        .inkCard(26)
        .padding(.top, 20)
    }
}
