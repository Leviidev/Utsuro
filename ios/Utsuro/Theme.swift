// SPDX-License-Identifier: GPL-2.0-or-later
import SwiftUI
import UIKit
import ImageIO

/// Utsuro's visual vocabulary, rendered with fluid materials and system colors.
///
/// Drawn with system adaptive materials: grouped lists on the system's grouped background,
/// liquid glass surfaces with continuous corner radiuses and subtle specular hairlines,
/// and an accent color the user can change (`AppTheme`).
enum Theme {
    /// The page behind grouped content.
    static let bgUI = UIColor.systemGroupedBackground
    static let bg = Color(uiColor: bgUI)
    /// Cards, rows, content containers.
    static let surfaceUI = UIColor.secondarySystemGroupedBackground
    static let surface = Color(uiColor: surfaceUI)
    /// Wells, chips, elevated elements on a card.
    static let surfaceHighUI = UIColor.tertiarySystemFill
    static let surfaceHigh = Color(uiColor: surfaceHighUI)
    /// The hairline edge that separates a surface from the page.
    static let hairlineUI = UIColor.separator
    static let hairline = Color(uiColor: hairlineUI)

    static let textUI = UIColor.label
    static let text = Color(uiColor: textUI)
    static let textDim = Color(uiColor: .secondaryLabel)

    /// The user's accent: what is pressed, selected or switched on.
    static var accent: Color { AppTheme.shared.accentColor }
    static var accentBright: Color { AppTheme.shared.accentColor }
    static var accentSoft: Color { AppTheme.shared.accentColor.opacity(0.18) }
    static let good = Color(uiColor: .systemGreen)
    static let danger = Color(uiColor: .systemRed)
    static let shadow = Color.black.opacity(0.12)

    /// The appearance the app is drawn in.
    enum Appearance: String, CaseIterable, Identifiable {
        case system, light, dark

        static let key = "utsuro.appearance"

        var id: String { rawValue }

        var title: String {
            switch self {
            case .system: return "System"
            case .light: return "Light"
            case .dark: return "Dark"
            }
        }

        var style: UIUserInterfaceStyle {
            switch self {
            case .system: return .unspecified
            case .light: return .light
            case .dark: return .dark
            }
        }

        static var current: Appearance {
            Appearance(rawValue: UserDefaults.standard.string(forKey: key) ?? "") ?? .system
        }
    }

    /// Applies an appearance override to every window in the app.
    static func apply(_ appearance: Appearance) {
        for case let scene as UIWindowScene in UIApplication.shared.connectedScenes {
            for window in scene.windows {
                window.overrideUserInterfaceStyle = appearance.style
            }
        }
    }

    static let cardCorner: CGFloat = 16
    static let rowCorner: CGFloat = 12

    static var backdrop: some View { bg.ignoresSafeArea() }
}

extension View {
    /// A liquid glass container for content on the page, with continuous corners and subtle depth.
    @ViewBuilder
    func utsuroCard<S: Shape>(_ shape: S, high: Bool = false) -> some View {
        self.background(.ultraThinMaterial, in: shape)
            .overlay(shape.stroke(Theme.hairline.opacity(0.35), lineWidth: 0.5))
            .shadow(color: Color.black.opacity(0.06), radius: 8, x: 0, y: 3)
    }

    func utsuroCard(high: Bool = false) -> some View {
        utsuroCard(RoundedRectangle(cornerRadius: Theme.cardCorner, style: .continuous), high: high)
    }

    /// Prominent liquid glass card with a specular hairline border and deeper refraction shadow.
    func utsuroGlassCard(cornerRadius: CGFloat = Theme.cardCorner) -> some View {
        let shape = RoundedRectangle(cornerRadius: cornerRadius, style: .continuous)
        return self.background(.thinMaterial, in: shape)
            .overlay(shape.stroke(Color.white.opacity(0.18), lineWidth: 0.5))
            .shadow(color: Color.black.opacity(0.12), radius: 12, x: 0, y: 5)
    }

    /// Dark floating HUD panel for the in-game overlay.
    func utsuroPanel<S: Shape>(_ shape: S) -> some View {
        self.background(Color(red: 0.082, green: 0.094, blue: 0.129).opacity(0.92), in: shape)
            .overlay(shape.stroke(Color.white.opacity(0.12), lineWidth: 0.5))
            .environment(\.colorScheme, .dark)
    }

    func utsuroDestination<D: Hashable, Screen: View>(for type: D.Type, @ViewBuilder destination: @escaping (D) -> Screen) -> some View {
        navigationDestination(for: type, destination: destination)
    }
}

/// A section's heading.
struct SectionTitle: View {
    let text: String
    init(_ text: String) { self.text = text }
    var body: some View {
        Text(text)
            .font(.system(size: 20, weight: .bold))
            .foregroundStyle(Theme.text)
            .frame(maxWidth: .infinity, alignment: .leading)
    }
}

/// A card button with smooth interactive depth scaling.
struct CardButtonStyle: ButtonStyle {
    func makeBody(configuration: Configuration) -> some View {
        configuration.label
            .scaleEffect(configuration.isPressed ? 0.96 : 1)
            .opacity(configuration.isPressed ? 0.9 : 1)
            .animation(.easeOut(duration: 0.14), value: configuration.isPressed)
    }
}

/// The primary call to action button.
struct PrimaryButtonStyle: ButtonStyle {
    var enabled = true

    func makeBody(configuration: Configuration) -> some View {
        configuration.label
            .font(.system(size: 16, weight: .semibold))
            .foregroundStyle(enabled ? .white : Theme.textDim)
            .frame(maxWidth: .infinity)
            .padding(.vertical, 15)
            .background(enabled ? Theme.accent : Theme.surfaceHigh,
                        in: RoundedRectangle(cornerRadius: 14, style: .continuous))
            .opacity(configuration.isPressed ? 0.85 : 1)
            .scaleEffect(configuration.isPressed ? 0.985 : 1)
            .animation(.easeOut(duration: 0.12), value: configuration.isPressed)
    }
}

/// A secondary action button with glass styling.
struct SecondaryButtonStyle: ButtonStyle {
    var tint: Color = Theme.text

    func makeBody(configuration: Configuration) -> some View {
        configuration.label
            .font(.system(size: 15, weight: .semibold))
            .foregroundStyle(tint)
            .frame(maxWidth: .infinity)
            .padding(.vertical, 12)
            .background(.ultraThinMaterial, in: RoundedRectangle(cornerRadius: 12, style: .continuous))
            .overlay(RoundedRectangle(cornerRadius: 12, style: .continuous).stroke(Theme.hairline.opacity(0.3), lineWidth: 0.5))
            .opacity(configuration.isPressed ? 0.8 : 1)
            .scaleEffect(configuration.isPressed ? 0.98 : 1)
            .animation(.easeOut(duration: 0.12), value: configuration.isPressed)
    }
}

/// Round glyph button for navigation bars or floating toolbars.
struct CircleButton: View {
    let systemImage: String
    var active = false
    let action: () -> Void

    var body: some View {
        Button(action: action) {
            Image(systemName: systemImage)
                .font(.system(size: 15, weight: .semibold))
                .foregroundStyle(active ? .white : Theme.text)
                .frame(width: 36, height: 36)
                .background(active ? Theme.accent : Theme.surfaceHigh, in: Circle())
        }
        .buttonStyle(.plain)
    }
}

/// A downsampled image from disk.
enum ThumbnailCache {
    nonisolated(unsafe) private static let cache = NSCache<NSString, UIImage>()

    static func image(at url: URL, maxPixel: CGFloat) -> UIImage? {
        let key = "\(url.path)@\(Int(maxPixel))" as NSString
        if let hit = cache.object(forKey: key) { return hit }
        guard let source = CGImageSourceCreateWithURL(url as CFURL, nil) else { return nil }
        let options: [CFString: Any] = [
            kCGImageSourceCreateThumbnailFromImageAlways: true,
            kCGImageSourceCreateThumbnailWithTransform: true,
            kCGImageSourceThumbnailMaxPixelSize: maxPixel,
        ]
        guard let cg = CGImageSourceCreateThumbnailAtIndex(source, 0, options as CFDictionary) else { return nil }
        let image = UIImage(cgImage: cg)
        cache.setObject(image, forKey: key)
        return image
    }
}

/// An image file, or a stand-in while it is not there.
struct FileImage: View {
    let url: URL?
    var maxPixel: CGFloat = 600
    var contentMode: ContentMode = .fit

    var body: some View {
        if let url, let image = ThumbnailCache.image(at: url, maxPixel: maxPixel) {
            Image(uiImage: image).resizable().aspectRatio(contentMode: contentMode)
        } else {
            Image(systemName: "gamecontroller.fill").resizable().scaledToFit().padding(24).foregroundStyle(Theme.textDim)
        }
    }
}
