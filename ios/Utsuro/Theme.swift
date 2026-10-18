import SwiftUI
import UIKit

/// The app is drawn with the system's own colours: grouped lists on the grouped background, label colours, one accent.
/// No gradients and no glass; the pad over the game is flat.
enum Theme {
    static let bg = Color(uiColor: .systemGroupedBackground)
    static let surface = Color(uiColor: .secondarySystemGroupedBackground)
    static let surfaceHigh = Color(uiColor: .tertiarySystemFill)
    static let hairline = Color(uiColor: .separator)
    static let text = Color(uiColor: .label)
    static let textDim = Color(uiColor: .secondaryLabel)
    static let accent = Color(red: 0.36, green: 0.52, blue: 0.95)
    static var accentSoft: Color { accent.opacity(0.16) }
    static let good = Color(uiColor: .systemGreen)
    static let cardCorner: CGFloat = 14

    static var backdrop: some View { bg.ignoresSafeArea() }
}

extension View {
    /// A container for content that is not a list row: a surface on the grouped page, as the system draws one.
    func utsuroCard(high: Bool = false) -> some View {
        background(high ? Theme.surfaceHigh : Theme.surface, in: RoundedRectangle(cornerRadius: Theme.cardCorner, style: .continuous))
    }

    func utsuroDestination<D: Hashable, Screen: View>(for type: D.Type, @ViewBuilder destination: @escaping (D) -> Screen) -> some View {
        navigationDestination(for: type, destination: destination)
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
            .font(.system(size: 16, weight: .semibold))
            .foregroundStyle(enabled ? .white : Theme.textDim)
            .frame(maxWidth: .infinity)
            .padding(.vertical, 15)
            .background(enabled ? Theme.accent : Theme.surfaceHigh,
                        in: RoundedRectangle(cornerRadius: 15, style: .continuous))
            .opacity(configuration.isPressed ? 0.85 : 1)
            .scaleEffect(configuration.isPressed ? 0.985 : 1)
            .animation(.easeOut(duration: 0.12), value: configuration.isPressed)
    }
}
