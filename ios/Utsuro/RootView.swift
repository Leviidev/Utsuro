import SwiftUI
import UIKit

/// The app: a Library and Settings behind a tab bar, and the game itself over all of it when one is playing.
struct RootView: View {
    @ObservedObject private var session = Session.shared
    @ObservedObject private var jit = JITCoordinator.shared
    @ObservedObject private var library = GameLibrary.shared
    @Environment(\.scenePhase) private var scenePhase
    @State private var tab = Tab.library

    enum Tab: Hashable { case library, settings }

    init() {
        // The tab bar in the dashboard's colours: flat, dark, green when selected.
        let appearance = UITabBarAppearance()
        appearance.configureWithOpaqueBackground()
        appearance.backgroundColor = UIColor(Theme.surface)
        appearance.shadowColor = UIColor(Theme.hairline)
        UITabBar.appearance().standardAppearance = appearance
        UITabBar.appearance().scrollEdgeAppearance = appearance
        let nav = UINavigationBarAppearance()
        nav.configureWithOpaqueBackground()
        nav.backgroundColor = UIColor(Theme.bg)
        nav.shadowColor = .clear
        nav.titleTextAttributes = [.foregroundColor: UIColor.white]
        nav.largeTitleTextAttributes = [.foregroundColor: UIColor.white]
        UINavigationBar.appearance().standardAppearance = nav
        UINavigationBar.appearance().scrollEdgeAppearance = nav
    }

    var body: some View {
        Group {
            if let game = session.playing {
                GameScreen(game: game) { session.leave() }
            } else {
                TabView(selection: $tab) {
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
