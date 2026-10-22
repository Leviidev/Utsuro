import SwiftUI
import UIKit

/// Every game, grouped by how far along it is: what plays, what is getting there, what does not run yet.
struct LibraryView: View {
    @ObservedObject private var library = GameLibrary.shared
    @ObservedObject private var router = AppRouter.shared
    @State private var query = ""

    private var columns: [GridItem] {
        Array(repeating: GridItem(.flexible(), spacing: 12, alignment: .top),
              count: UIDevice.current.userInterfaceIdiom == .pad ? 4 : 2)
    }

    /// The groups, in the order they are shown.
    private struct Shelf: Identifiable {
        let id: String
        let title: String
        let color: Color
        let includes: (GameCompatibility) -> Bool
    }

    private let shelves: [Shelf] = [
        Shelf(id: "ready", title: "Ready to play", color: GameCompatibility.plays.color) { $0 == .plays || $0 == .ingame },
        Shelf(id: "close", title: "Getting there", color: GameCompatibility.opens.color) { $0 == .opens },
        Shelf(id: "not", title: "Not yet", color: GameCompatibility.broken.color) { $0 == .broken },
        Shelf(id: "new", title: "Not tried", color: GameCompatibility.untested.color) { $0 == .untested },
    ]

    var body: some View {
        NavigationStack(path: $router.library) {
            ScrollView {
                VStack(alignment: .leading, spacing: 0) {
                    HStack(alignment: .center) {
                        Text("Library").font(.title(36)).foregroundStyle(Ink.text)
                        Spacer()
                        RoundGlyphButton(systemImage: "plus") { router.addGame() }
                            .disabled(library.importing)
                            .accessibilityLabel("Add a Game")
                    }
                    .padding(.horizontal, 2)
                    .padding(.bottom, 12)

                    SearchField(text: $query, prompt: "Search your games")
                    if library.importing {
                        BusyStrip(text: library.importProgress ?? "Copying the game…").padding(.top, 12)
                    }
                    if let failure = library.importError {
                        Text(failure).font(.footnote).foregroundStyle(Theme.danger).padding(.top, 10)
                    }

                    let games = library.games.filter { matches(query, $0.title) }
                    ForEach(shelves) { shelf in
                        let members = games.filter { shelf.includes($0.compatibility) }
                        if !members.isEmpty { section(shelf, members) }
                    }
                    if games.isEmpty {
                        if query.isEmpty { if !library.importing { emptyCard.padding(.top, 22) } } else { noResults }
                    }
                }
                .padding(.horizontal, 16)
                .padding(.top, 4)
                .padding(.bottom, 28)
            }
            .scrollIndicators(.hidden)
            .scrollDismissesKeyboard(.immediately)
            .background(Ink.page.ignoresSafeArea())
            .toolbar(.hidden, for: .navigationBar)
            .navigationDestination(for: InstalledGame.self) { GamePage(game: $0) }
        }
    }

    private func section(_ shelf: Shelf, _ games: [InstalledGame]) -> some View {
        VStack(alignment: .leading, spacing: 10) {
            HStack(spacing: 8) {
                Circle().fill(shelf.color).frame(width: 7, height: 7)
                Caps(shelf.title, color: Ink.text)
                Caps("\(games.count)")
            }
            .padding(.horizontal, 4)
            LazyVGrid(columns: columns, spacing: 12) {
                ForEach(games) { game in
                    NavigationLink(value: game) { GameCard(game: game) }
                        .buttonStyle(PressScale())
                        .accessibilityLabel(game.title)
                        .gameMenu(game) { router.library.append($0) }
                }
            }
        }
        .padding(.top, 24)
    }

    private var emptyCard: some View {
        VStack(alignment: .leading, spacing: 12) {
            Text("No games yet").font(.title(22)).foregroundStyle(Ink.text)
            Text("Utsuro runs a decrypted Xbox One dump: the folder that holds Mount and EmbeddedXvd. Add it with the + button, or put "
               + "it in On My iPhone › Utsuro › Games with the Files app.")
                .font(.subheadline)
                .foregroundStyle(Ink.dim)
                .fixedSize(horizontal: false, vertical: true)
            Button { router.addGame() } label: { Label("Add a Game", systemImage: "plus") }
                .buttonStyle(PillButtonStyle())
                .padding(.top, 4)
        }
        .frame(maxWidth: .infinity, alignment: .leading)
        .padding(18)
        .inkCard(24)
    }

    private var noResults: some View {
        VStack(spacing: 8) {
            Image(systemName: "magnifyingglass").font(.system(size: 28, weight: .light)).foregroundStyle(Ink.faint)
            Text("Nothing called “\(query)”").font(.title(18)).foregroundStyle(Ink.text)
        }
        .frame(maxWidth: .infinity)
        .padding(.vertical, 48)
    }

    private func matches(_ query: String, _ title: String) -> Bool {
        let q = query.trimmingCharacters(in: .whitespaces)
        return q.isEmpty || title.localizedCaseInsensitiveContains(q)
    }
}

/// One game in the grid: its box art in a card, its name, and how much it has been played (or how it runs).
private struct GameCard: View {
    let game: InstalledGame

    private var caption: String {
        if let played = game.timePlayed { return played }
        switch game.compatibility {
        case .opens: return "Opens, not playable yet"
        case .broken: return "Does not start yet"
        case .untested: return "Not tried yet"
        default: return "Not played yet"
        }
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 0) {
            Color.clear
                .aspectRatio(1, contentMode: .fit)
                .overlay { GeometryReader { geo in GameIcon(game: game, size: geo.size.width, corner: 18) } }
            Text(game.title)
                .font(.system(size: 14.5, weight: .semibold))
                .foregroundStyle(Ink.text)
                .lineLimit(1)
                .padding(.top, 9)
                .padding(.horizontal, 4)
            Text(caption)
                .font(.system(size: 12))
                .foregroundStyle(Ink.dim)
                .lineLimit(1)
                .padding(.top, 2)
                .padding(.horizontal, 4)
                .padding(.bottom, 4)
        }
        .padding(8)
        .inkCard(24)
        .contentShape(RoundedRectangle(cornerRadius: 24, style: .continuous))
    }
}
