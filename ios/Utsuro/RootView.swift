import SwiftUI
import UIKit

/// The app: Home, Library and Settings behind a tab bar, and the game itself over all of it when one is playing.
struct RootView: View {
    @ObservedObject private var session = Session.shared
    @ObservedObject private var jit = JITCoordinator.shared
    @ObservedObject private var library = GameLibrary.shared
    @Environment(\.scenePhase) private var scenePhase
    @State private var tab = Tab.home

    enum Tab: Hashable { case home, library, settings }

    init() {
        let appearance = UITabBarAppearance()
        appearance.configureWithDefaultBackground()
        UITabBar.appearance().standardAppearance = appearance
        UITabBar.appearance().scrollEdgeAppearance = appearance

        let nav = UINavigationBarAppearance()
        nav.configureWithDefaultBackground()
        UINavigationBar.appearance().standardAppearance = nav
        UINavigationBar.appearance().scrollEdgeAppearance = nav
    }

    var body: some View {
        Group {
            if let game = session.playing {
                GameScreen(game: game) { session.leave() }
            } else {
                TabView(selection: $tab) {
                    HomeView(openLibrary: { tab = .library })
                        .tabItem { Label("Home", systemImage: "house.fill") }
                        .tag(Tab.home)
                    LibraryView()
                        .tabItem { Label("Library", systemImage: "square.grid.2x2.fill") }
                        .tag(Tab.library)
                    SettingsView()
                        .tabItem { Label("Settings", systemImage: "gearshape.fill") }
                        .tag(Tab.settings)
                }
                .tint(Theme.accentBright)
            }
        }
        .background(Theme.bg.ignoresSafeArea())
        .sheet(isPresented: $jit.showSetup) { JITSetupFlow() }
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

/// What the player comes back to: the last game played, big, with Play; then the rest of the library as a row, and the
/// JIT line when it is off. Fluid materials, continuous corners, and liquid glass styling.
struct HomeView: View {
    @ObservedObject private var library = GameLibrary.shared
    @ObservedObject private var session = Session.shared
    @ObservedObject private var jit = JITCoordinator.shared
    var openLibrary: () -> Void
    @State private var path: [InstalledGame] = []

    /// Games with the most recently played first; never-played ones after, in library order.
    private var ordered: [InstalledGame] {
        library.games.sorted { a, b in
            let da = GameSettings(gameID: a.id).lastPlayed ?? .distantPast
            let db = GameSettings(gameID: b.id).lastPlayed ?? .distantPast
            return da > db
        }
    }

    var body: some View {
        NavigationStack(path: $path) {
            ScrollView {
                VStack(alignment: .leading, spacing: 22) {
                    JITCard(compact: true)
                    if let first = ordered.first {
                        hero(first)
                        if ordered.count > 1 {
                            SectionTitle("Your games")
                            ScrollView(.horizontal, showsIndicators: false) {
                                HStack(spacing: 12) {
                                    ForEach(ordered.dropFirst()) { game in
                                        NavigationLink(value: game) { GameTile(game: game).frame(width: 140) }
                                            .buttonStyle(CardButtonStyle())
                                    }
                                }
                                .padding(.horizontal, 2)
                            }
                        }
                        Button(action: openLibrary) {
                            HStack { Label("All games", systemImage: "square.grid.2x2"); Spacer(); Image(systemName: "chevron.right").foregroundStyle(Theme.textDim) }
                                .padding(16).contentShape(Rectangle())
                        }
                        .buttonStyle(.plain)
                        .utsuroCard()
                    } else {
                        empty
                    }
                }
                .padding(16)
                .frame(maxWidth: 900)
                .frame(maxWidth: .infinity)
            }
            .background(Theme.backdrop)
            .navigationTitle("Utsuro")
            .navigationBarTitleDisplayMode(.large)
            .navigationDestination(for: InstalledGame.self) { GamePage(game: $0) }
        }
    }

    /// The featured game: its wide art, name, how it runs, and Play.
    private func hero(_ game: InstalledGame) -> some View {
        VStack(alignment: .leading, spacing: 0) {
            ZStack(alignment: .bottomLeading) {
                game.tileColor
                FileImage(url: game.wideLogo ?? game.logo, maxPixel: 1200).padding(game.wideLogo == nil ? 40 : 0)
            }
            .frame(maxWidth: .infinity).aspectRatio(16.0 / 7.0, contentMode: .fit)
            .clipped()
            VStack(alignment: .leading, spacing: 10) {
                HStack(alignment: .firstTextBaseline) {
                    VStack(alignment: .leading, spacing: 3) {
                        Text(game.title).font(.system(size: 20, weight: .bold)).foregroundStyle(Theme.text).lineLimit(2)
                        HStack(spacing: 6) {
                            let status = game.compatibility
                            Circle().fill(status.color).frame(width: 8, height: 8)
                            Text(status.label).font(.footnote.weight(.semibold)).foregroundStyle(Theme.textDim)
                            if let played = GameSettings(gameID: game.id).lastPlayed {
                                Text("· " + played.formatted(.relative(presentation: .named))).font(.footnote).foregroundStyle(Theme.textDim)
                            }
                        }
                    }
                    Spacer(minLength: 12)
                }
                HStack(spacing: 10) {
                    Button { session.play(game) } label: { Label("Play", systemImage: "play.fill").frame(maxWidth: .infinity) }
                        .buttonStyle(PrimaryButtonStyle())
                    NavigationLink(value: game) {
                        Image(systemName: "ellipsis").font(.system(size: 16, weight: .bold)).foregroundStyle(Theme.text)
                            .frame(width: 50, height: 50)
                            .background(Theme.surfaceHigh, in: RoundedRectangle(cornerRadius: Theme.cardCorner, style: .continuous))
                    }
                    .buttonStyle(CardButtonStyle())
                }
            }
            .padding(16)
        }
        .utsuroGlassCard(cornerRadius: Theme.cardCorner)
    }

    private var empty: some View {
        VStack(spacing: 16) {
            Image(systemName: "gamecontroller").font(.system(size: 46)).foregroundStyle(Theme.textDim)
            Text("Nothing to play yet").font(.system(size: 22, weight: .bold))
            Text("Add a decrypted Xbox One dump in the Library.").font(.subheadline).foregroundStyle(Theme.textDim).multilineTextAlignment(.center)
            Button(action: openLibrary) { Label("Open Library", systemImage: "square.grid.2x2") }
                .buttonStyle(PrimaryButtonStyle())
                .frame(maxWidth: 280)
        }
        .padding(.top, 50)
        .padding(.horizontal, 12)
    }
}
