import Foundation
import SwiftUI
import UIKit

/// The games Utsuro can run. A game is a dump: a folder holding `Mount/` (the title) and `EmbeddedXvd/` (the Xbox Era userland).
/// They are looked for in Utsuro's Documents folder -- On My iPhone › Utsuro in the Files app -- in `Games/<name>`, in a folder of their
/// own directly in Documents, or in Documents itself. Importing one copies it into `Games/`.
@MainActor
final class GameLibrary: ObservableObject {
    static let shared = GameLibrary()

    @Published private(set) var games: [InstalledGame] = []
    @Published private(set) var importing = false
    @Published private(set) var importProgress: String?
    @Published var importError: String?

    nonisolated static var documents: URL { FileManager.default.urls(for: .documentDirectory, in: .userDomainMask)[0] }
    nonisolated static var gamesRoot: URL { documents.appendingPathComponent("Games", isDirectory: true) }
    /// Where the Minecraft that ships inside some builds is unpacked, and where earlier versions put every import.
    nonisolated static var legacyTarget: URL { documents.appendingPathComponent("Minecraft", isDirectory: true) }

    private init() { refresh() }

    nonisolated static func isDump(_ url: URL) -> Bool {
        var dir: ObjCBool = false
        let fm = FileManager.default
        return fm.fileExists(atPath: url.appendingPathComponent("Mount/AppxManifest.xml").path)
            && fm.fileExists(atPath: url.appendingPathComponent("EmbeddedXvd/Windows", isDirectory: true).path, isDirectory: &dir) && dir.boolValue
    }

    // MARK: - Per-game places

    /// Saves live next to the game, outside the dump so removing or re-importing the game keeps them. The Minecraft that earlier
    /// versions imported keeps its old `Saves/` folder.
    nonisolated static func saves(for game: InstalledGame) -> URL {
        if game.dump.standardizedFileURL == legacyTarget.standardizedFileURL { return documents.appendingPathComponent("Saves", isDirectory: true) }
        return documents.appendingPathComponent("GameData/\(game.id)/Saves", isDirectory: true)
    }

    nonisolated static func screenshotsFolder(for game: InstalledGame) -> URL {
        documents.appendingPathComponent("Screenshots/\(game.id)", isDirectory: true)
    }

    func screenshots(for game: InstalledGame) -> [URL] {
        let folder = Self.screenshotsFolder(for: game)
        let files = (try? FileManager.default.contentsOfDirectory(at: folder, includingPropertiesForKeys: [.contentModificationDateKey])) ?? []
        return files.filter { $0.pathExtension.lowercased() == "png" }
            .sorted { (a, b) in
                let da = (try? a.resourceValues(forKeys: [.contentModificationDateKey]).contentModificationDate) ?? .distantPast
                let db = (try? b.resourceValues(forKeys: [.contentModificationDateKey]).contentModificationDate) ?? .distantPast
                return da > db
            }
    }

    /// Some dumps ship without the console's registry hives; another game's (same console OS family) stand in for them.
    func hiveFallback(for game: InstalledGame) -> URL? {
        func config(_ g: InstalledGame) -> URL { g.dump.appendingPathComponent("EmbeddedXvd/Windows/System32/config", isDirectory: true) }
        let fm = FileManager.default
        if fm.fileExists(atPath: config(game).appendingPathComponent("SOFTWARE").path) { return nil }
        return games.first { $0.id != game.id && fm.fileExists(atPath: config($0).appendingPathComponent("SOFTWARE").path) }.map(config)
    }

    // MARK: - Finding games

    func refresh() {
        let fm = FileManager.default
        var candidates: [URL] = [Self.documents, Self.legacyTarget]
        for root in [Self.gamesRoot, Self.documents] {
            if let sub = try? fm.contentsOfDirectory(at: root, includingPropertiesForKeys: nil, options: [.skipsHiddenFiles]) { candidates += sub }
        }
        var seen = Set<String>()
        var found: [InstalledGame] = []
        for url in candidates where Self.isDump(url) {
            let key = url.standardizedFileURL.path
            guard seen.insert(key).inserted else { continue }
            // A dump directly in Documents has no folder name of its own to file things under.
            if url.standardizedFileURL == Self.documents.standardizedFileURL { continue }
            found.append(InstalledGame(dump: url))
        }
        games = found.sorted { $0.title.localizedCaseInsensitiveCompare($1.title) == .orderedAscending }
        UtsuroLog.log("library", "games: " + (games.isEmpty ? "none in \(Self.documents.path)" : games.map { "\($0.title) (\($0.dump.lastPathComponent))" }.joined(separator: ", ")))
        // A build that carries the game inside the app (package_ipa.sh --bundle-game) unpacks it once, so the title gets a writable copy.
        if games.isEmpty, !importing, let bundled = Bundle.main.resourceURL?.appendingPathComponent("Game"), Self.isDump(bundled) {
            UtsuroLog.log("library", "unpacking the game bundled in the app")
            importDump(from: bundled, into: Self.legacyTarget)
        }
    }

    // MARK: - Importing and removing

    /// Copies a dump the user picked (a folder with Mount/ and EmbeddedXvd/) into the app, so the game reads local storage.
    func importDump(from picked: URL, into fixedTarget: URL? = nil) {
        guard !importing else { return }
        importing = true
        importError = nil
        importProgress = "Copying…"
        DispatchQueue.global(qos: .userInitiated).async {
            let scoped = picked.startAccessingSecurityScopedResource()
            defer { if scoped { picked.stopAccessingSecurityScopedResource() } }
            let fm = FileManager.default
            var failure: String?
            do {
                // The picked folder is the dump, or holds it one level down.
                var source = picked
                if !Self.isDump(source) {
                    let inside = (try? fm.contentsOfDirectory(at: picked, includingPropertiesForKeys: nil)) ?? []
                    guard let found = inside.first(where: Self.isDump) else {
                        throw NSError(domain: "Utsuro", code: 1, userInfo: [NSLocalizedDescriptionKey:
                            "That folder does not hold Mount and EmbeddedXvd. Pick the folder that contains both."])
                    }
                    source = found
                }
                var target = fixedTarget
                if target == nil {
                    // Filed under the name the game gives itself, so a second import of it replaces the first.
                    let name = ManifestReader.read(source.appendingPathComponent("Mount/AppxManifest.xml")).identityName ?? source.lastPathComponent
                    let safe = name.components(separatedBy: CharacterSet.alphanumerics.union(CharacterSet(charactersIn: "-_.")).inverted).joined()
                    target = Self.gamesRoot.appendingPathComponent(safe.isEmpty ? "Game" : safe, isDirectory: true)
                }
                let destination = target!
                try? fm.removeItem(at: destination)
                try fm.createDirectory(at: destination, withIntermediateDirectories: true)
                for name in ["Mount", "EmbeddedXvd"] {
                    DispatchQueue.main.async { self.importProgress = "Copying \(name)…" }
                    try Self.copyTree(from: source.appendingPathComponent(name), to: destination.appendingPathComponent(name))
                }
            } catch {
                failure = error.localizedDescription
            }
            DispatchQueue.main.async {
                self.importing = false
                self.importProgress = nil
                self.importError = failure
                self.refresh()
            }
        }
    }

    /// Deletes the game's files. Its saves and screenshots stay, unless asked for.
    func remove(_ game: InstalledGame, saves: Bool) {
        let fm = FileManager.default
        try? fm.removeItem(at: game.dump)
        if saves {
            try? fm.removeItem(at: Self.saves(for: game))
            try? fm.removeItem(at: Self.screenshotsFolder(for: game))
        }
        refresh()
    }

    func deleteSaves(of game: InstalledGame) {
        try? FileManager.default.removeItem(at: Self.saves(for: game))
    }

    func deleteScreenshot(_ url: URL) {
        try? FileManager.default.removeItem(at: url)
        objectWillChange.send()
    }

    /// What the game's saves and screenshots take, for the page that offers to delete them.
    nonisolated static func size(of url: URL) -> Int64 {
        let fm = FileManager.default
        guard let walker = fm.enumerator(at: url, includingPropertiesForKeys: [.totalFileAllocatedSizeKey, .isRegularFileKey]) else { return 0 }
        var total: Int64 = 0
        for case let file as URL in walker {
            let v = try? file.resourceValues(forKeys: [.totalFileAllocatedSizeKey, .isRegularFileKey])
            if v?.isRegularFile == true { total += Int64(v?.totalFileAllocatedSize ?? 0) }
        }
        return total
    }

    /// A copy that goes file by file, so one unreadable file names itself rather than failing the whole folder silently.
    nonisolated private static func copyTree(from: URL, to: URL) throws {
        let fm = FileManager.default
        try fm.createDirectory(at: to, withIntermediateDirectories: true)
        let entries = try fm.contentsOfDirectory(at: from, includingPropertiesForKeys: [.isDirectoryKey])
        for entry in entries {
            let dest = to.appendingPathComponent(entry.lastPathComponent)
            if (try? entry.resourceValues(forKeys: [.isDirectoryKey]).isDirectory) == true {
                try copyTree(from: entry, to: dest)
            } else {
                try fm.copyItem(at: entry, to: dest)
            }
        }
    }
}
