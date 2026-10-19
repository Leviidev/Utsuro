import SwiftUI

/// What the player can change about how one game is played on this phone. Kept per game, in UserDefaults.
struct GameSettings {
    enum TouchControls: String, CaseIterable, Identifiable {
        case auto, always, off
        var id: String { rawValue }
        var title: String {
            switch self { case .auto: return "Automatic"; case .always: return "Always on"; case .off: return "Off" }
        }
    }

    let gameID: String

    private func key(_ name: String) -> String { "utsuro.game.\(gameID).\(name)" }

    /// When the touch controller is drawn: automatic hides it while a physical controller is connected.
    var touchControls: TouchControls {
        get { TouchControls(rawValue: UserDefaults.standard.string(forKey: key("touch")) ?? "") ?? .auto }
        nonmutating set { UserDefaults.standard.set(newValue.rawValue, forKey: key("touch")) }
    }

    var padOpacity: Double {
        get { UserDefaults.standard.object(forKey: key("padOpacity")) as? Double ?? UserDefaults.standard.object(forKey: "utsuro.padOpacity") as? Double ?? 0.6 }
        nonmutating set { UserDefaults.standard.set(newValue, forKey: key("padOpacity")) }
    }

    var haptics: Bool {
        get { UserDefaults.standard.object(forKey: key("haptics")) as? Bool ?? true }
        nonmutating set { UserDefaults.standard.set(newValue, forKey: key("haptics")) }
    }

    var lastPlayed: Date? {
        get { UserDefaults.standard.object(forKey: key("lastPlayed")) as? Date }
        nonmutating set { UserDefaults.standard.set(newValue, forKey: key("lastPlayed")) }
    }

    /// Seconds spent in the game, added up when it is left.
    var playSeconds: Double {
        get { UserDefaults.standard.double(forKey: key("playSeconds")) }
        nonmutating set { UserDefaults.standard.set(newValue, forKey: key("playSeconds")) }
    }
}
