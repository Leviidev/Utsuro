import Foundation
import SwiftUI

/// A game Utsuro has found: a dump folder (Mount/ the title, EmbeddedXvd/ the console's userland) and what its manifest says about it.
struct InstalledGame: Identifiable, Hashable {
    /// The dump folder's name; stable between launches, and what per-game settings and saves are filed under.
    let id: String
    let dump: URL
    var mount: URL { dump.appendingPathComponent("Mount", isDirectory: true) }

    let title: String
    let publisher: String
    let summary: String
    let version: String
    let executable: String
    let titleId: String?
    let ratings: [String]
    let tileColor: Color
    let logo: URL?
    let wideLogo: URL?
    let splash: URL?

    /// The square picture for tiles: the console's 480-pixel tile art (full-bleed box art), else the smaller logos.
    var icon: URL? { wideLogo ?? logo }
    /// The wide picture behind the title on Home and the game's page: the 1920x1080 splash screen.
    var artwork: URL? { splash }

    /// When the dump was added: what "Recently added" sorts by.
    var addedDate: Date? {
        let attrs = try? FileManager.default.attributesOfItem(atPath: dump.path)
        return (attrs?[.creationDate] as? Date) ?? (attrs?[.modificationDate] as? Date)
    }

    /// How this game runs, as a dot and a word: this iPhone's own record first (it was played here), then what the
    /// project knows about the title. Nothing fancier: the list lives in this file and moves with each release.
    var compatibility: GameCompatibility {
        if GameSettings(gameID: id).playSeconds > 120 { return .plays }
        return GameCompatibility.known(title: title, executable: executable)
    }

    static func == (a: InstalledGame, b: InstalledGame) -> Bool { a.id == b.id && a.dump == b.dump }
    func hash(into hasher: inout Hasher) { hasher.combine(id); hasher.combine(dump) }

    /// Reads `dump/Mount/AppxManifest.xml`. A dump whose manifest cannot be read still gets a card, named after its folder.
    init(dump: URL) {
        self.dump = dump
        let folder = dump.lastPathComponent
        id = folder
        let mount = dump.appendingPathComponent("Mount", isDirectory: true)
        let m = ManifestReader.read(mount.appendingPathComponent("AppxManifest.xml"))
        func clean(_ s: String?) -> String? {
            guard let s = s?.trimmingCharacters(in: .whitespacesAndNewlines), !s.isEmpty, !s.hasPrefix("ms-resource:") else { return nil }
            return s.replacingOccurrences(of: "®", with: "").replacingOccurrences(of: "™", with: "")
        }
        title = clean(m.visualDisplayName) ?? clean(m.displayName) ?? clean(m.identityName) ?? folder
        publisher = clean(m.publisher) ?? "Unknown publisher"
        summary = clean(m.about) ?? ""
        version = m.version ?? ""
        executable = m.executable ?? ""
        titleId = m.titleId
        ratings = m.ratings
        tileColor = Color(hex: m.backgroundColor) ?? Theme.surfaceHigh
        func asset(_ names: [String?]) -> URL? {
            for case let name? in names {
                if let hit = Self.resolve(name, in: mount) { return hit }
            }
            return nil
        }
        logo = asset([m.logo, "Logo.png", m.storeLogo, "StoreLogo.png"])
        wideLogo = asset([m.wideLogo, "WideLogo.png"])
        splash = asset([m.splash, "SplashScreen.png"])
    }

    /// A manifest path (`Media\Logos\Logo.png`) as a file under `root`. Windows paths: either slash, and any case, since the
    /// manifest's case need not match the files' (widelogo.png / WideLogo.png).
    static func resolve(_ path: String, in root: URL) -> URL? {
        var at = root
        for part in path.split(whereSeparator: { $0 == "\\" || $0 == "/" }) {
            let names = (try? FileManager.default.contentsOfDirectory(atPath: at.path)) ?? []
            guard let hit = names.first(where: { $0.lowercased() == part.lowercased() }) else { return nil }
            at = at.appendingPathComponent(hit)
        }
        return at == root ? nil : at
    }
}

extension Color {
    /// "#RRGGBB" as a colour; nil for anything else.
    init?(hex: String?) {
        guard var h = hex?.trimmingCharacters(in: .whitespaces), !h.isEmpty else { return nil }
        if h.hasPrefix("#") { h.removeFirst() }
        guard h.count == 6, let v = UInt32(h, radix: 16) else { return nil }
        self.init(red: Double((v >> 16) & 0xFF) / 255, green: Double((v >> 8) & 0xFF) / 255, blue: Double(v & 0xFF) / 255)
    }
}

/// The few things the app wants out of an AppxManifest.xml.
final class ManifestReader: NSObject, XMLParserDelegate {
    var identityName: String?, version: String?, displayName: String?, publisher: String?, about: String?
    var storeLogo: String?, executable: String?, visualDisplayName: String?, logo: String?, wideLogo: String?, splash: String?
    var backgroundColor: String?, titleId: String?
    var ratings: [String] = []

    private var path: [String] = []
    private var text = ""
    private var seenApplication = false

    static func read(_ url: URL) -> ManifestReader {
        let reader = ManifestReader()
        if let data = try? Data(contentsOf: url) {
            let parser = XMLParser(data: data)
            parser.delegate = reader
            parser.parse()
        }
        return reader
    }

    func parser(_ parser: XMLParser, didStartElement element: String, namespaceURI: String?, qualifiedName: String?, attributes a: [String: String]) {
        path.append(element)
        text = ""
        switch element {
        case "Identity": identityName = a["Name"]; version = a["Version"]
        case "Application" where !seenApplication: seenApplication = true; executable = a["Executable"]
        case "VisualElements" where visualDisplayName == nil:
            visualDisplayName = a["DisplayName"]; logo = a["Logo"]; backgroundColor = a["BackgroundColor"]
        case "DefaultTile": wideLogo = a["WideLogo"] ?? wideLogo
        case "SplashScreen": splash = a["Image"] ?? splash
        case "mx:XboxLive" where titleId == nil: titleId = a["TitleId"]
        default: break
        }
    }

    func parser(_ parser: XMLParser, foundCharacters string: String) { text += string }

    func parser(_ parser: XMLParser, didEndElement element: String, namespaceURI: String?, qualifiedName: String?) {
        let parent = path.dropLast().last
        if parent == "Properties" {
            switch element {
            case "DisplayName": displayName = text
            case "PublisherDisplayName": publisher = text
            case "Description": about = text
            case "Logo": storeLogo = text
            default: break
            }
        } else if element == "mx:Rating", ratings.count < 3 {
            ratings.append(text.replacingOccurrences(of: ":", with: " "))
        }
        _ = path.popLast()
        text = ""
    }
}


/// A status word and its colour, as Husk shows them.
struct GameCompatibility: Equatable {
    let label: String
    let color: Color

    static let plays = GameCompatibility(label: "Plays", color: .green)
    static let ingame = GameCompatibility(label: "In-game", color: Color(red: 0.6, green: 0.8, blue: 0.2))
    static let opens = GameCompatibility(label: "Opens", color: .orange)
    static let broken = GameCompatibility(label: "Broken", color: .red)
    static let untested = GameCompatibility(label: "Untested", color: .gray)

    /// What the project has seen of the titles it ships notes for (PROJECT.md section 2).
    static func known(title: String, executable: String) -> GameCompatibility {
        let t = title.lowercased(), e = executable.lowercased()
        if t.contains("minecraft") || t.contains("sonic mania") || t.contains("rocket league") || e.contains("tagame") { return .plays }
        if t.contains("forza horizon 2") || e.contains("forza_durango") { return .opens }
        if t.contains("puyo") || t.contains("mega man") || t.contains("overcooked") || t.contains("peggle") { return .opens }
        if t.contains("trackmania") { return .broken }
        return .untested
    }
}
