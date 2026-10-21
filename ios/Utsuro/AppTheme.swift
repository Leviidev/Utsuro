// SPDX-License-Identifier: GPL-2.0-or-later
import SwiftUI
import UIKit

/// The app's accent colour, chosen by the user and remembered.
///
/// Light and dark follow the system or Settings > Appearance (`Theme.Appearance`).
/// This is the colour that buttons, toggles, selected states and links are tinted with.
final class AppTheme: ObservableObject {
    static let shared = AppTheme()

    struct Preset: Identifiable {
        let id: String
        let name: String
        let color: Color
    }

    /// Xbox Green - the default Utsuro accent.
    static let xbox = Color(red: 0.063, green: 0.486, blue: 0.063)
    /// Husk's signature indigo.
    static let husk = Color(red: 0.353, green: 0.322, blue: 0.945)

    static let presets: [Preset] = [
        Preset(id: "xbox", name: "Xbox Green", color: xbox),
        Preset(id: "husk", name: "Husk Indigo", color: husk),
        Preset(id: "blue", name: "Electric Blue", color: Color(red: 0.0, green: 0.48, blue: 1.0)),
        Preset(id: "cyan", name: "Neon Cyan", color: Color(red: 0.0, green: 0.75, blue: 0.95)),
        Preset(id: "purple", name: "Deep Purple", color: Color(red: 0.61, green: 0.31, blue: 0.87)),
        Preset(id: "pink", name: "Sunset Pink", color: Color(red: 1.0, green: 0.18, blue: 0.42)),
        Preset(id: "red", name: "Racing Red", color: Color(red: 0.95, green: 0.22, blue: 0.20)),
        Preset(id: "orange", name: "Ember Orange", color: Color(red: 1.0, green: 0.58, blue: 0.0)),
        Preset(id: "yellow", name: "Sun Gold", color: Color(red: 1.0, green: 0.80, blue: 0.0)),
        Preset(id: "mint", name: "Mint Frost", color: Color(red: 0.0, green: 0.78, blue: 0.65)),
    ]

    @Published var accentColor: Color {
        didSet { persist() }
    }

    private static let key = "utsuro.accentColorHex"

    private init() {
        if let hex = UserDefaults.standard.string(forKey: Self.key), let color = Color(themeHex: hex) {
            accentColor = color
        } else {
            accentColor = Self.xbox
        }
    }

    func reset() { accentColor = Self.xbox }

    private func persist() {
        UserDefaults.standard.set(accentColor.themeHex, forKey: Self.key)
    }
}

extension Color {
    init?(themeHex hex: String) {
        var s = hex.trimmingCharacters(in: .whitespacesAndNewlines)
        if s.hasPrefix("#") { s.removeFirst() }
        guard s.count == 6, let value = UInt32(s, radix: 16) else { return nil }
        self = Color(red: Double((value >> 16) & 0xFF) / 255.0,
                     green: Double((value >> 8) & 0xFF) / 255.0,
                     blue: Double(value & 0xFF) / 255.0)
    }

    var themeHex: String {
        var r: CGFloat = 0, g: CGFloat = 0, b: CGFloat = 0, a: CGFloat = 0
        UIColor(self).getRed(&r, green: &g, blue: &b, alpha: &a)
        return String(format: "#%02X%02X%02X", Int((r * 255.0).rounded()), Int((g * 255.0).rounded()), Int((b * 255.0).rounded()))
    }

    /// Whether text on this colour should be dark.
    var isLight: Bool {
        var r: CGFloat = 0, g: CGFloat = 0, b: CGFloat = 0, a: CGFloat = 0
        UIColor(self).getRed(&r, green: &g, blue: &b, alpha: &a)
        return 0.299 * r + 0.587 * g + 0.114 * b > 0.6
    }
}
