import SwiftUI
import UniformTypeIdentifiers

/// Every game found, as tiles; a tile opens the game's page.
struct LibraryView: View {
    @ObservedObject private var library = GameLibrary.shared
    @State private var picking = false
    @State private var path: [InstalledGame] = []

    private let columns = [GridItem(.adaptive(minimum: 150, maximum: 220), spacing: 14)]

    var body: some View {
        NavigationStack(path: $path) {
            ScrollView {
                VStack(spacing: 18) {
                    JITCard(compact: true)
                    if library.games.isEmpty && !library.importing {
                        emptyState
                    } else {
                        LazyVGrid(columns: columns, spacing: 14) {
                            ForEach(library.games) { game in
                                NavigationLink(value: game) { GameTile(game: game) }
                                    .buttonStyle(CardButtonStyle())
                            }
                            addTile
                        }
                    }
                    if library.importing {
                        HStack(spacing: 10) { ProgressView().tint(Theme.accentBright); Text(library.importProgress ?? "Copying…").font(.subheadline).foregroundStyle(Theme.textDim) }
                            .frame(maxWidth: .infinity, alignment: .leading)
                    }
                    if let failure = library.importError { Text(failure).font(.footnote).foregroundStyle(Theme.danger).frame(maxWidth: .infinity, alignment: .leading) }
                }
                .padding(16)
                .frame(maxWidth: 900)
                .frame(maxWidth: .infinity)
            }
            .background(Theme.backdrop)
            .navigationTitle("Library")
            .navigationBarTitleDisplayMode(.large)
            .navigationDestination(for: InstalledGame.self) { GamePage(game: $0) }
        }
        .fileImporter(isPresented: $picking, allowedContentTypes: [.folder]) { result in
            switch result {
            case .success(let url): library.importDump(from: url)
            case .failure(let failure): library.importError = failure.localizedDescription
            }
        }
    }

    private var addTile: some View {
        Button { picking = true } label: {
            VStack(spacing: 10) {
                Image(systemName: "plus").font(.system(size: 34, weight: .semibold)).foregroundStyle(Theme.accentBright)
                    .frame(maxWidth: .infinity).aspectRatio(1, contentMode: .fit)
                    .background(.ultraThinMaterial, in: RoundedRectangle(cornerRadius: Theme.cardCorner, style: .continuous))
                    .overlay(RoundedRectangle(cornerRadius: Theme.cardCorner, style: .continuous).strokeBorder(Theme.hairline, style: StrokeStyle(lineWidth: 1.5, dash: [6, 5])))
                Text("Add game").font(.system(size: 14, weight: .semibold)).foregroundStyle(Theme.textDim)
            }
        }
        .buttonStyle(CardButtonStyle())
        .disabled(library.importing)
    }

    private var emptyState: some View {
        VStack(spacing: 16) {
            Image(systemName: "shippingbox").font(.system(size: 46)).foregroundStyle(Theme.textDim)
            Text("No games yet").font(.system(size: 22, weight: .bold))
            Text("Utsuro runs a decrypted Xbox One dump: the folder that holds Mount and EmbeddedXvd. Pick it here and Utsuro copies it in, "
                 + "or put it in Utsuro's folder in the Files app (On My iPhone › Utsuro › Games).")
                .font(.subheadline).foregroundStyle(Theme.textDim).multilineTextAlignment(.center)
            Button { picking = true } label: { Label("Add game", systemImage: "plus") }
                .buttonStyle(PrimaryButtonStyle())
                .frame(maxWidth: 280)
        }
        .padding(.top, 50)
        .padding(.horizontal, 12)
    }
}

/// A game's tile: its logo on the colour its manifest gives, and its name.
struct GameTile: View {
    let game: InstalledGame

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            ZStack {
                game.tileColor
                FileImage(url: game.logo, maxPixel: 400).padding(16)
            }
            .frame(maxWidth: .infinity).aspectRatio(1, contentMode: .fit)
            .clipShape(RoundedRectangle(cornerRadius: Theme.cardCorner, style: .continuous))
            .overlay(RoundedRectangle(cornerRadius: Theme.cardCorner, style: .continuous).stroke(Color.white.opacity(0.12), lineWidth: 0.5))
            .shadow(color: Color.black.opacity(0.08), radius: 6, x: 0, y: 3)
            Text(game.title).font(.system(size: 14, weight: .semibold)).foregroundStyle(Theme.text).lineLimit(2).multilineTextAlignment(.leading)
            HStack(spacing: 5) {
                Circle().fill(game.compatibility.color).frame(width: 7, height: 7)
                Text(game.compatibility.label).font(.system(size: 12, weight: .medium)).foregroundStyle(Theme.textDim)
            }
        }
    }
}
