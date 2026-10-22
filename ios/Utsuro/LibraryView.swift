import SwiftUI
import UIKit

/// Every game: search, and a grid of box art; a tile opens the game's page.
struct LibraryView: View {
    @ObservedObject private var library = GameLibrary.shared
    @ObservedObject private var router = AppRouter.shared
    @State private var query = ""

    private var columns: [GridItem] {
        Array(repeating: GridItem(.flexible(), spacing: 14, alignment: .top),
              count: UIDevice.current.userInterfaceIdiom == .pad ? 6 : 3)
    }

    var body: some View {
        NavigationStack(path: $router.library) {
            ScrollView {
                VStack(alignment: .leading, spacing: 16) {
                    SearchField(text: $query, prompt: "Search games")
                    if library.importing { BusyStrip(text: library.importProgress ?? "Copying the game…") }
                    if let failure = library.importError {
                        Text(failure).font(.footnote).foregroundStyle(Theme.danger).frame(maxWidth: .infinity, alignment: .leading)
                    }

                    let games = library.games.filter { matches(query, $0.title) }
                    section("Xbox One", games)
                    if !query.isEmpty, games.isEmpty { NoResults(query: query) }
                }
                .padding(.horizontal, 20)
                .padding(.top, 8)
                .padding(.bottom, 24)
            }
            .scrollIndicators(.hidden)
            .scrollDismissesKeyboard(.immediately)
            .background(Console.page.ignoresSafeArea())
            .navigationTitle("Library")
            .navigationBarTitleDisplayMode(.large)
            .toolbar {
                ToolbarItem(placement: .topBarTrailing) {
                    Button { router.addGame() } label: { Image(systemName: "plus") }
                        .disabled(library.importing)
                        .accessibilityLabel("Add a Game")
                }
            }
            .navigationDestination(for: InstalledGame.self) { GamePage(game: $0) }
        }
    }

    @ViewBuilder
    private func section(_ title: String, _ games: [InstalledGame]) -> some View {
        VStack(alignment: .leading, spacing: 14) {
            HStack(alignment: .firstTextBaseline, spacing: 8) {
                Text(title).font(.system(size: 21, weight: .bold)).foregroundStyle(Console.text)
                Text("\(games.count)").font(.system(size: 15, weight: .medium)).foregroundStyle(Console.textDim)
            }
            if games.isEmpty {
                if query.isEmpty && !library.importing { emptyCard }
            } else {
                LazyVGrid(columns: columns, spacing: 20) {
                    ForEach(games) { game in
                        NavigationLink(value: game) { LibraryCell(game: game) }
                            .buttonStyle(CardButtonStyle())
                            .accessibilityLabel(game.title)
                            .gameMenu(game) { router.library.append($0) }
                    }
                }
            }
        }
        .padding(.top, 8)
    }

    private var emptyCard: some View {
        VStack(alignment: .leading, spacing: 12) {
            Text("No games yet").font(.system(size: 17, weight: .semibold)).foregroundStyle(Console.text)
            Text("Utsuro runs a decrypted Xbox One dump: the folder that holds Mount and EmbeddedXvd. Add it with the + button, or put "
               + "it in On My iPhone › Utsuro › Games with the Files app.")
                .font(.subheadline)
                .foregroundStyle(Console.textDim)
                .fixedSize(horizontal: false, vertical: true)
            Button { router.addGame() } label: { Label("Add a Game", systemImage: "plus") }
                .buttonStyle(ConsolePlayButtonStyle())
                .padding(.top, 4)
        }
        .frame(maxWidth: .infinity, alignment: .leading)
        .padding(16)
        .background(Console.card, in: RoundedRectangle(cornerRadius: 16, style: .continuous))
    }

    private func matches(_ query: String, _ title: String) -> Bool {
        let q = query.trimmingCharacters(in: .whitespaces)
        return q.isEmpty || title.localizedCaseInsensitiveContains(q)
    }
}

/// One game in the grid: its box art, and its name with the status dot.
private struct LibraryCell: View {
    let game: InstalledGame

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            Color.clear
                .aspectRatio(1, contentMode: .fit)
                .overlay { GeometryReader { geo in GameIcon(game: game, size: geo.size.width) } }
                .shadow(color: .black.opacity(0.25), radius: 6, y: 3)
            HStack(spacing: 5) {
                if game.compatibility != .untested { StatusDot(status: game.compatibility) }
                Text(game.title)
                    .font(.system(size: 13, weight: .semibold))
                    .foregroundStyle(Console.text)
                    .lineLimit(1)
            }
        }
        .contentShape(Rectangle())
    }
}

/// Nothing matches the search.
private struct NoResults: View {
    let query: String
    var body: some View {
        VStack(spacing: 8) {
            Image(systemName: "magnifyingglass").font(.system(size: 28, weight: .light)).foregroundStyle(.secondary)
            Text("No Results").font(.headline)
            Text("No game here is called “\(query)”.").font(.subheadline).foregroundStyle(.secondary)
        }
        .frame(maxWidth: .infinity)
        .padding(.vertical, 48)
    }
}
