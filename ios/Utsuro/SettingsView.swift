import SwiftUI
import UIKit
import GameController

/// Settings, laid out the way the iOS Settings app lays them out: a card for the app up top, a JIT group, then groups of
/// rows that push a page of their own. Fluid materials, continuous corners, and customizable theme accents.
struct SettingsView: View {
    @ObservedObject private var jit = JITCoordinator.shared
    @ObservedObject private var library = GameLibrary.shared
    @AppStorage(JITCoordinator.autoEnableKey) private var jitAtLaunch = false
    @AppStorage(JITCoordinator.trollStoreAutoKey) private var trollStoreAuto = true

    var body: some View {
        NavigationStack {
            List {
                Section {
                    NavigationLink { AboutSettings() } label: { header }
                        .listRowBackground(Color.clear)
                }

                Section {
                    NavigationLink { JITSettings() } label: {
                        Self.row(icon: AnyView(JITRing(size: 18)), JITControl.isOn ? "JIT is on" : "JIT is off", jitLine)
                    }
                    Toggle(isOn: $jitAtLaunch) {
                        row("bolt.badge.automatic.fill", .yellow, "Turn on at launch", "As soon as Utsuro opens")
                    }
                    .disabled(!UtsuroBuiltInJIT.isAvailable)
                    if JITBootstrap.isInstalledWithTrollStore {
                        Toggle(isOn: $trollStoreAuto) {
                            row("shippingbox.fill", .orange, "Ask TrollStore at launch", "This Utsuro was installed with TrollStore")
                        }
                    }
                } header: {
                    Text("JIT")
                } footer: {
                    Text(UtsuroBuiltInJIT.isAvailable
                         ? "Games need JIT. StikJIT, built into Utsuro, turns it on without a computer; with the switch on it happens by itself every time Utsuro opens, once it is paired."
                         : "Games need JIT. Built-in StikJIT needs iOS 26; on this iPhone Utsuro uses StikDebug, TrollStore or a jailbreak instead.")
                }
                .listRowBackground(Ink.card)

                Section("General") {
                    NavigationLink { AppearanceSettings() } label: { row("paintbrush.fill", .pink, "Appearance", "Light or dark, accent colour") }
                    NavigationLink { ControllerSettings() } label: { row("gamecontroller.fill", .green, "Controllers", "Connected pads and the touch controls") }
                    NavigationLink { PerformanceSettings() } label: { row("speedometer", .orange, "Performance", "Frame pacing and rendering") }
                    NavigationLink { StorageSettings() } label: { row("internaldrive.fill", .blue, "Storage", "\(library.games.count) game\(library.games.count == 1 ? "" : "s")") }
                    NavigationLink { HelpSettings() } label: { row("questionmark", .teal, "Help", "What to do when something does not run") }
                }
                .listRowBackground(Ink.card)

                Section("Support") {
                    NavigationLink { LogsSettings() } label: { row("doc.text.fill", .gray, "Logs", "Share Utsuro's log with a bug report") }
                }
                .listRowBackground(Ink.card)
            }
            .listStyle(.insetGrouped)
            .scrollContentBackground(.hidden)
            .background(Ink.page.ignoresSafeArea())
            .toolbar(.hidden, for: .navigationBar)
        }
    }

    /// What JIT is doing, in a line under its row.
    private var jitLine: String {
        if JITControl.isOn { return "With \(JITControl.via(jit))" }
        if jit.busy { return jit.status ?? "Turning on…" }
        return "Games need it: tap to turn it on"
    }

    /// The app at the top, centred: its icon, its name as the wordmark sets it, and the build.
    private var header: some View {
        VStack(spacing: 0) {
            UtsuroAppIcon(size: 78)
                .shadow(color: Theme.accent.opacity(0.35), radius: 18, y: 6)
            Wordmark(size: 26).padding(.top, 12)
            Text("Version \(SettingsView.versionLine)").font(.system(size: 13)).foregroundStyle(Ink.dim).padding(.top, 3)
        }
        .frame(maxWidth: .infinity)
        .padding(.top, 8)
        .padding(.bottom, 4)
    }

    /// A row with a round icon in the accent, the title, and a line under it.
    static func row(_ symbol: String, _ color: Color, _ title: String, _ detail: String) -> some View {
        row(icon: AnyView(Image(systemName: symbol).font(.system(size: 14, weight: .semibold)).foregroundStyle(Theme.accent)), title, detail)
    }

    static func row(icon: AnyView, _ title: String, _ detail: String) -> some View {
        Label {
            VStack(alignment: .leading, spacing: 1) {
                Text(title)
                Text(detail).font(.caption).foregroundStyle(.secondary).lineLimit(1)
            }
        } icon: {
            Circle()
                .fill(Theme.accent.opacity(0.18))
                .frame(width: 32, height: 32)
                .overlay { icon }
        }
    }

    private func row(_ symbol: String, _ color: Color, _ title: String, _ detail: String) -> some View { Self.row(symbol, color, title, detail) }

    static var versionLine: String {
        let info = Bundle.main.infoDictionary
        let version = info?["CFBundleShortVersionString"] as? String ?? "?"
        let commit = info?["UtsuroBuildCommit"] as? String ?? "dev"
        return "\(version) · \(commit)"
    }
}

// MARK: - pages

/// Appearance settings: light/dark theme selection and accent color palette.
struct AppearanceSettings: View {
    @AppStorage(Theme.Appearance.key) private var appearance = Theme.Appearance.system
    @ObservedObject private var theme = AppTheme.shared
    @Environment(\.colorScheme) private var scheme
    @State private var custom: Color = AppTheme.shared.accentColor

    var body: some View {
        Form {
            Section {
                Picker("Appearance", selection: $appearance) {
                    ForEach(Theme.Appearance.allCases) { Text($0.title).tag($0) }
                }
                .pickerStyle(.segmented)
                .onChange(of: appearance) { v in
                    Theme.apply(v)
                }
            } header: {
                Text("Theme")
            } footer: {
                Text("System follows your iPhone: light by day, dark by night. The game screen stays dark either way.")
            }

            Section {
                LazyVGrid(columns: Array(repeating: GridItem(.flexible()), count: 5), spacing: 14) {
                    ForEach(AppTheme.presets) { preset in
                        Button {
                            theme.accentColor = preset.color
                            custom = preset.color
                        } label: {
                            swatch(preset.color, selected: preset.color.themeHex == theme.accentColor.themeHex)
                        }
                        .buttonStyle(.plain)
                        .accessibilityLabel(preset.name)
                    }
                }
                .padding(.vertical, 6)

                ColorPicker("Custom Colour", selection: $custom, supportsOpacity: false)
                    .onChange(of: custom) { theme.accentColor = $0 }
            } header: {
                Text("Accent Colour")
            } footer: {
                Text("Tints buttons, switches, and highlighted cards across the app.")
            }
        }
        .scrollContentBackground(.hidden)
        .background(Theme.backdrop)
        .navigationTitle("Appearance")
    }

    private func swatch(_ color: Color, selected: Bool) -> some View {
        ZStack {
            Circle()
                .fill(color)
                .frame(width: 44, height: 44)
                .shadow(color: color.opacity(0.35), radius: selected ? 6 : 2, y: 2)
            if selected {
                Circle()
                    .strokeBorder(Color.white, lineWidth: 3)
                    .frame(width: 44, height: 44)
                Image(systemName: "checkmark")
                    .font(.system(size: 16, weight: .bold))
                    .foregroundStyle(.white)
            }
        }
    }
}

/// JIT in detail: the method, what this iPhone has, and what is attached right now.
struct JITSettings: View {
    @ObservedObject private var jit = JITCoordinator.shared

    var body: some View {
        Form {
            Section { JITCard() }.listRowInsets(EdgeInsets()).listRowBackground(Color.clear)
            Section {
                Picker("Method", selection: $jit.method) { ForEach(JITMethod.allCases) { Text($0.title).tag($0) } }
                detail("StikDebug", JITBootstrap.isStikDebugInstalled ? "installed" : "not found")
                detail("Installed with TrollStore", JITBootstrap.isInstalledWithTrollStore ? "yes" : "no")
                detail("Built-in pairing", jit.hasPairing ? "set up" : "not set up")
                Button { jit.showSetup = true } label: { Label("Set up JIT", systemImage: "wand.and.stars") }
            } header: { Text("Other ways to turn on JIT") } footer: {
                Text(jit.method == .automatic ? jit.automaticDescription : (UtsuroBuiltInJIT.unavailableReason ?? "Built-in StikJIT needs a pairing file, which Utsuro can make itself."))
            }
            Section("Now") {
                detail("Debugger", JITBootstrap.isDebuggerAttached ? "attached" : "not attached")
                detail("Executable memory", JITBootstrap.ready ? "granted" : "not claimed")
            }
        }
        .scrollContentBackground(.hidden).background(Theme.backdrop)
        .navigationTitle("JIT")
    }

    private func detail(_ label: String, _ value: String) -> some View {
        HStack { Text(label); Spacer(); Text(value).foregroundStyle(.secondary) }
    }
}

/// Connected controllers, and the touch controls' defaults (each game can override them on its page).
struct ControllerSettings: View {
    @State private var controllers: [String] = GCController.controllers().map { $0.vendorName ?? "Controller" }
    @AppStorage("utsuro.padOpacity") private var padOpacity = 0.7

    var body: some View {
        Form {
            Section("Connected") {
                if controllers.isEmpty {
                    Label("No controller connected", systemImage: "gamecontroller").foregroundStyle(.secondary)
                } else {
                    ForEach(controllers, id: \.self) { Label($0, systemImage: "gamecontroller.fill") }
                }
            }
            Section {
                VStack(alignment: .leading) {
                    HStack { Text("Touch control opacity"); Spacer(); Text("\(Int(padOpacity * 100))%").foregroundStyle(.secondary) }
                    Slider(value: $padOpacity, in: 0.2...1.0).tint(Theme.accent)
                }
            } header: { Text("Touch controls") } footer: {
                Text("Connect an Xbox, PlayStation or MFi controller in Settings › Bluetooth and the touch controls step aside. Each game's page can override the defaults.")
            }
        }
        .scrollContentBackground(.hidden).background(Theme.backdrop)
        .navigationTitle("Controllers")
        .onReceive(NotificationCenter.default.publisher(for: .GCControllerDidConnect)) { _ in controllers = GCController.controllers().map { $0.vendorName ?? "Controller" } }
        .onReceive(NotificationCenter.default.publisher(for: .GCControllerDidDisconnect)) { _ in controllers = GCController.controllers().map { $0.vendorName ?? "Controller" } }
    }
}

/// Rendering knobs that apply to every game.
struct PerformanceSettings: View {
    @AppStorage("utsuro.frameCap") private var frameCap = 0
    @AppStorage("utsuro.showFps") private var showFps = false

    var body: some View {
        Form {
            Section {
                Picker("Frame rate cap", selection: $frameCap) {
                    Text("Off (as fast as it runs)").tag(0)
                    Text("30 fps").tag(30)
                    Text("60 fps").tag(60)
                }
                Toggle("Show frame rate", isOn: $showFps).tint(Theme.accent)
            } header: { Text("Frames") } footer: {
                Text("A cap keeps the phone cooler on games that run faster than they need to. The counter sits in a corner over the game.")
            }
        }
        .scrollContentBackground(.hidden).background(Theme.backdrop)
        .navigationTitle("Performance")
    }
}

/// How much is on the phone, and where.
struct StorageSettings: View {
    @ObservedObject private var library = GameLibrary.shared
    @State private var usedBytes: Int64 = 0

    var body: some View {
        Form {
            Section {
                HStack { Text("Games"); Spacer(); Text("\(library.games.count)").foregroundStyle(.secondary) }
                HStack { Text("Used by Utsuro"); Spacer(); Text(usedBytes > 0 ? ByteCountFormatter.string(fromByteCount: usedBytes, countStyle: .file) : "…").foregroundStyle(.secondary) }
                HStack { Text("Files app folder"); Spacer(); Text("On My iPhone › Utsuro").foregroundStyle(.secondary) }
            }
            Section {
                ForEach(library.games) { game in
                    HStack(spacing: 8) {
                        Circle().fill(game.compatibility.color).frame(width: 8, height: 8)
                        Text(game.title)
                        Spacer()
                        Text(game.compatibility.label).foregroundStyle(.secondary)
                    }
                }
            } header: { Text("Compatibility") } footer: {
                Text("Green plays, yellow-green reaches gameplay with problems, orange opens, red does not start. A game you have played on this iPhone for a while counts as playing.")
            }
        }
        .scrollContentBackground(.hidden).background(Theme.backdrop)
        .navigationTitle("Storage")
        .onAppear {
            DispatchQueue.global(qos: .utility).async {
                let total = GameLibrary.size(of: GameLibrary.documents)
                DispatchQueue.main.async { usedBytes = total }
            }
        }
    }
}

/// Short answers to the questions that come up.
struct HelpSettings: View {
    var body: some View {
        Form {
            Section("Adding a game") {
                Text("Utsuro runs a decrypted Xbox One dump: a folder that holds Mount and EmbeddedXvd. Add it from the Library, or put it in On My iPhone › Utsuro › Games with the Files app.")
            }
            Section("The game will not start") {
                Text("Turn on JIT first: the Settings page says whether it is on. Then check the game's compatibility line; a game marked Opens or Broken is not expected to play yet.")
            }
            Section("It runs slowly") {
                Text("Close other apps and give the phone a minute to cool. The first minutes of a game compile its shaders and are slower than the rest.")
            }
        }
        .scrollContentBackground(.hidden).background(Theme.backdrop)
        .navigationTitle("Help")
    }
}

/// The log files, shared as they are.
struct LogsSettings: View {
    @State private var share = false

    var body: some View {
        Form {
            Section {
                Button { share = true } label: { Label("Share logs", systemImage: "square.and.arrow.up") }
            } footer: {
                Text("utsuro.log is this run, utsuro-prev.log the one before, and utsuro-crash.log the last crash. Attach them to a bug report.")
            }
        }
        .scrollContentBackground(.hidden).background(Theme.backdrop)
        .navigationTitle("Logs")
        .sheet(isPresented: $share) { ShareSheet(items: logFiles) }
    }

    private var logFiles: [URL] {
        let docs = GameLibrary.documents
        return ["utsuro.log", "utsuro-prev.log", "utsuro-crash.log"].map { docs.appendingPathComponent($0) }
            .filter { FileManager.default.fileExists(atPath: $0.path) }
    }
}

/// What this build is.
struct AboutSettings: View {
    var body: some View {
        Form {
            Section {
                HStack { Text("Version"); Spacer(); Text(SettingsView.versionLine).foregroundStyle(.secondary) }
                HStack { Text("Runs"); Spacer(); Text("Xbox One games on arm64").foregroundStyle(.secondary) }
                HStack { Text("Engine"); Spacer(); Text("FEXCore JIT, Metal").foregroundStyle(.secondary) }
            }
            Section {
                Link(destination: URL(string: "https://github.com/Leviidev/Utsuro")!) { Label("Utsuro on GitHub", systemImage: "link") }
            }
        }
        .scrollContentBackground(.hidden).background(Theme.backdrop)
        .navigationTitle("About")
    }
}

struct ShareSheet: UIViewControllerRepresentable {
    let items: [Any]

    func makeUIViewController(context: Context) -> UIActivityViewController {
        UIActivityViewController(activityItems: items, applicationActivities: nil)
    }

    func updateUIViewController(_ vc: UIActivityViewController, context: Context) {}
}
