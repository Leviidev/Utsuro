import SwiftUI
import UIKit

/// What the app is doing at the top level: browsing, or inside a game. Launching goes through here so the JIT check lives in one place.
@MainActor
final class Session: ObservableObject {
    static let shared = Session()

    @Published private(set) var playing: InstalledGame?
    @Published var launchError: String?

    private var startedAt: Date?

    func play(_ game: InstalledGame) {
        launchError = nil
        guard !GameRunner.isRunning else {
            launchError = "A game has already run in this session. Close Utsuro and open it again to start another."
            return
        }
        let jit = JITCoordinator.shared
        if !JITBootstrap.prewarmed { JITBootstrap.prewarm() }
        guard JITBootstrap.prewarmed else {
            launchError = JITBootstrap.lastFailure ?? "JIT is not on yet. Turn it on in Settings, then press Play."
            jit.enable()
            return
        }
        UtsuroLog.log("ui", "play \(game.id)")
        GameSettings(gameID: game.id).lastPlayed = Date()
        startedAt = Date()
        AppDelegate.lock(.landscape)
        playing = game
    }

    func leave() {
        if let game = playing, let startedAt {
            let settings = GameSettings(gameID: game.id)
            settings.playSeconds += Date().timeIntervalSince(startedAt)
        }
        playing = nil
        AppDelegate.lock(.portrait)
    }
}
