import SwiftUI
import UIKit
import ImageIO

/// The app's look: the Xbox dashboard's. Near-black surfaces, one green, square-ish tiles, large type.
/// Flat colour only -- no gradients and no glass; the pad over the game is flat too.
enum Theme {
    static let bg = Color(red: 0.055, green: 0.055, blue: 0.059)
    static let surface = Color(red: 0.105, green: 0.108, blue: 0.113)
    static let surfaceHigh = Color(red: 0.155, green: 0.160, blue: 0.168)
    static let hairline = Color.white.opacity(0.08)
    static let text = Color.white
    static let textDim = Color.white.opacity(0.62)
    /// Xbox green, for fills with white on them.
    static let accent = Color(red: 0.063, green: 0.486, blue: 0.063)
    /// The brighter green, for text and icons on the dark surfaces.
    static let accentBright = Color(red: 0.36, green: 0.76, blue: 0.20)
    static var accentSoft: Color { accent.opacity(0.22) }
    static let good = accentBright
    static let danger = Color(red: 0.90, green: 0.30, blue: 0.27)
    static let cardCorner: CGFloat = 12

    static var backdrop: some View { bg.ignoresSafeArea() }
}

extension View {
    /// A container for content that is not a list row: a flat surface on the page.
    func utsuroCard(high: Bool = false) -> some View {
        background(high ? Theme.surfaceHigh : Theme.surface, in: RoundedRectangle(cornerRadius: Theme.cardCorner, style: .continuous))
    }

    func utsuroDestination<D: Hashable, Screen: View>(for type: D.Type, @ViewBuilder destination: @escaping (D) -> Screen) -> some View {
        navigationDestination(for: type, destination: destination)
    }
}

/// A section's heading, in the dashboard's small-caps-less bold.
struct SectionTitle: View {
    let text: String
    init(_ text: String) { self.text = text }
    var body: some View {
        Text(text).font(.system(size: 20, weight: .bold)).foregroundStyle(Theme.text)
            .frame(maxWidth: .infinity, alignment: .leading)
    }
}

struct CardButtonStyle: ButtonStyle {
    func makeBody(configuration: Configuration) -> some View {
        configuration.label
            .scaleEffect(configuration.isPressed ? 0.96 : 1)
            .opacity(configuration.isPressed ? 0.9 : 1)
            .animation(.easeOut(duration: 0.14), value: configuration.isPressed)
    }
}

/// The one action a screen is for.
struct PrimaryButtonStyle: ButtonStyle {
    var enabled = true

    func makeBody(configuration: Configuration) -> some View {
        configuration.label
            .font(.system(size: 17, weight: .bold))
            .foregroundStyle(enabled ? .white : Theme.textDim)
            .frame(maxWidth: .infinity)
            .padding(.vertical, 15)
            .background(enabled ? Theme.accent : Theme.surfaceHigh, in: RoundedRectangle(cornerRadius: 6, style: .continuous))
            .opacity(configuration.isPressed ? 0.85 : 1)
            .scaleEffect(configuration.isPressed ? 0.985 : 1)
            .animation(.easeOut(duration: 0.12), value: configuration.isPressed)
    }
}

/// A secondary action: outlined, the dashboard's other button.
struct SecondaryButtonStyle: ButtonStyle {
    var tint: Color = Theme.text
    func makeBody(configuration: Configuration) -> some View {
        configuration.label
            .font(.system(size: 15, weight: .semibold))
            .foregroundStyle(tint)
            .frame(maxWidth: .infinity)
            .padding(.vertical, 12)
            .background(Theme.surfaceHigh, in: RoundedRectangle(cornerRadius: 6, style: .continuous))
            .opacity(configuration.isPressed ? 0.8 : 1)
    }
}

/// A downsampled image from disk: the splash art is 1920x1080 and a grid of them should not hold the full bitmaps.
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
