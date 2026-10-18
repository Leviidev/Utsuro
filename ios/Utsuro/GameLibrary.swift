import Foundation
import SwiftUI
import UIKit

/// Where the game's files are. A dump is a folder holding `Mount/` (the title) and `EmbeddedXvd/` (the Xbox Era userland).
/// Utsuro looks for one in its Documents folder, which the Files app shows under On My iPhone › Utsuro, directly or one folder down.
@MainActor
final class GameLibrary: ObservableObject {
    static let shared = GameLibrary()

    @Published private(set) var dump: URL?
    @Published private(set) var importing = false
    @Published private(set) var importProgress: String?
    @Published var importError: String?

    nonisolated static var documents: URL { FileManager.default.urls(for: .documentDirectory, in: .userDomainMask)[0] }
    /// Where an import goes, and where saves live next to it.
    nonisolated static var importTarget: URL { documents.appendingPathComponent("Minecraft", isDirectory: true) }
    nonisolated static var saves: URL { documents.appendingPathComponent("Saves", isDirectory: true) }

    private init() { refresh() }

    nonisolated private static func isDump(_ url: URL) -> Bool {
        var dir: ObjCBool = false
        let fm = FileManager.default
        return fm.fileExists(atPath: url.appendingPathComponent("Mount/AppxManifest.xml").path)
            && fm.fileExists(atPath: url.appendingPathComponent("EmbeddedXvd/Windows", isDirectory: true).path, isDirectory: &dir) && dir.boolValue
    }

    func refresh() {
        let fm = FileManager.default
        var candidates = [Self.documents, Self.importTarget]
        if let sub = try? fm.contentsOfDirectory(at: Self.documents, includingPropertiesForKeys: nil, options: [.skipsHiddenFiles]) {
            candidates += sub
        }
        dump = candidates.first(where: Self.isDump)
        UtsuroLog.log("library", dump.map { "game found at \($0.path)" } ?? "no game in \(Self.documents.path)")
    }

    /// Copies a dump the user picked (a folder with Mount/ and EmbeddedXvd/) into the app, so the game reads local storage.
    func importDump(from picked: URL) {
        guard !importing else { return }
        importing = true
        importError = nil
        importProgress = "Copying…"
        let target = Self.importTarget
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
                try? fm.removeItem(at: target)
                try fm.createDirectory(at: target, withIntermediateDirectories: true)
                for name in ["Mount", "EmbeddedXvd"] {
                    DispatchQueue.main.async { self.importProgress = "Copying \(name)…" }
                    try Self.copyTree(from: source.appendingPathComponent(name), to: target.appendingPathComponent(name))
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
