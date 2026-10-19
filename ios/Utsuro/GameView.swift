import SwiftUI
import UIKit
import QuartzCore
import GameController

/// Runs the title on a thread of its own. There is one title per process, and it ends the process's JIT session when it returns.
enum GameRunner {
    nonisolated(unsafe) private static var started = false
    static var isRunning: Bool { started }

    static func start(game: InstalledGame, hives: URL?, layer: CAMetalLayer, finished: @escaping (Int32) -> Void) {
        guard !started else { return }
        started = true
        let dump = game.dump
        layer.drawableSize = CGSize(width: 1920, height: 1080)
        let layerPointer = Unmanaged.passUnretained(layer).toOpaque()
        let documents = GameLibrary.documents
        let saves = GameLibrary.saves(for: game)
        try? FileManager.default.createDirectory(at: saves, withIntermediateDirectories: true)
        setenv("UTSURO_SAVES", saves.path, 1)
        if let hives { setenv("UTSURO_HIVES", hives.path, 1) }
        if getenv("UTSURO_LOG") == nil { setenv("UTSURO_LOG", "info", 1) }
        let data = (Bundle.main.resourcePath ?? "") + "/data"
        let crashLog = documents.appendingPathComponent("utsuro-crash.log").path
        let thread = Thread {
            UtsuroLog.log("game", "booting \(dump.path)")
            husk_ios_jit_log_footprint("before-boot")
            let status = utsuro_ios_run(dump.path, data, crashLog, layerPointer)
            UtsuroLog.log("game", "the title returned \(status)")
            UtsuroLog.flushNow()
            DispatchQueue.main.async { finished(status) }
        }
        thread.name = "utsuro-title"
        thread.stackSize = 16 << 20
        thread.qualityOfService = .userInteractive
        thread.start()
    }
}

/// A view whose backing layer is the CAMetalLayer the renderer presents to.
final class MetalHostView: UIView {
    override class var layerClass: AnyClass { CAMetalLayer.self }
    var onLayer: ((CAMetalLayer) -> Void)?
    private var delivered = false

    override func layoutSubviews() {
        super.layoutSubviews()
        guard bounds.width > 0, let layer = layer as? CAMetalLayer else { return }
        // The renderer draws 1080p whatever the screen is; a bounds change must not resize the drawable under it.
        if delivered { layer.drawableSize = CGSize(width: 1920, height: 1080); return }
        delivered = true
        layer.contentsScale = window?.screen.scale ?? UIScreen.main.scale
        layer.contentsGravity = .resizeAspect
        layer.isOpaque = true
        onLayer?(layer)
    }
}

struct MetalHost: UIViewRepresentable {
    let onLayer: (CAMetalLayer) -> Void
    func makeUIView(context: Context) -> MetalHostView {
        let view = MetalHostView()
        view.backgroundColor = .black
        view.onLayer = onLayer
        return view
    }
    func updateUIView(_ view: MetalHostView, context: Context) {}
}

/// The game full screen: the picture at 16:9 in the middle, the controller over it.
struct GameScreen: View {
    let game: InstalledGame
    let onExit: () -> Void
    @StateObject private var pad = VirtualPad()
    @State private var layout: PadLayout
    @State private var confirmQuit = false
    @State private var flash = false
    /// A physical controller is the player's input when there is one; the touch controls only exist for when there is not.
    @State private var hasController = !GCController.controllers().isEmpty
    private let settings: GameSettings

    init(game: InstalledGame, onExit: @escaping () -> Void) {
        self.game = game
        self.onExit = onExit
        settings = GameSettings(gameID: game.id)
        _layout = State(initialValue: PadLayout.load("minecraft"))
    }

    private var showPad: Bool {
        switch settings.touchControls {
        case .auto: return !hasController
        case .always: return true
        case .off: return false
        }
    }

    private func takeScreenshot() {
        let folder = GameLibrary.screenshotsFolder(for: game)
        try? FileManager.default.createDirectory(at: folder, withIntermediateDirectories: true)
        let name = "Screenshot \(Self.stamp.string(from: Date())).png"
        utsuro_ios_screenshot(folder.appendingPathComponent(name).path)
        UtsuroLog.log("ui", "screenshot \(name)")
        withAnimation(.easeOut(duration: 0.12)) { flash = true }
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.18) { withAnimation(.easeIn(duration: 0.25)) { flash = false } }
    }

    private static let stamp: DateFormatter = {
        let f = DateFormatter()
        f.dateFormat = "yyyy-MM-dd HH.mm.ss"
        return f
    }()

    var body: some View {
        ZStack {
            Color.black.ignoresSafeArea()
            MetalHost { layer in
                let hives = MainActor.assumeIsolated { GameLibrary.shared.hiveFallback(for: game) }
                GameRunner.start(game: game, hives: hives, layer: layer) { _ in onExit() }
            }
            .aspectRatio(16.0 / 9.0, contentMode: .fit)
            if showPad {
                VirtualPadView(pad: pad, opacity: settings.padOpacity, haptics: settings.haptics, layout: layout)
                    .ignoresSafeArea()
            }
            VStack {
                HStack(spacing: 14) {
                    Spacer()
                    barButton("camera.fill") { takeScreenshot() }
                    barButton("xmark") { confirmQuit = true }
                    Spacer()
                }
                .padding(.top, 4)
                Spacer()
            }
            Color.white.opacity(flash ? 0.35 : 0).ignoresSafeArea().allowsHitTesting(false)
        }
        .statusBarHidden(true)
        .persistentSystemOverlays(.hidden)
        .onReceive(NotificationCenter.default.publisher(for: .GCControllerDidConnect)) { _ in
            hasController = !GCController.controllers().isEmpty
            UtsuroLog.log("input", "controller connected: \(GCController.controllers().compactMap { $0.vendorName }.joined(separator: ", "))")
        }
        .onReceive(NotificationCenter.default.publisher(for: .GCControllerDidDisconnect)) { _ in
            hasController = !GCController.controllers().isEmpty
            UtsuroLog.log("input", "controller disconnected; touch controls \(hasController ? "stay hidden" : "are back")")
        }
        .onAppear {
            UtsuroLog.log("input", "controllers at start: \(GCController.controllers().count)")
            UIApplication.shared.isIdleTimerDisabled = true
        }
        .onDisappear { UIApplication.shared.isIdleTimerDisabled = false }
        .onReceive(NotificationCenter.default.publisher(for: UIApplication.didReceiveMemoryWarningNotification)) { _ in
            UtsuroLog.log("memory", "iOS sent a memory warning")
            husk_ios_jit_log_footprint("memory-warning")
        }
        .alert("Quit \(game.title)?", isPresented: $confirmQuit) {
            Button("Quit", role: .destructive) {
                // The guest has no clean shutdown from outside; saves are written as the game makes them.
                UtsuroLog.log("game", "quit from the on-screen button")
                UtsuroLog.flushNow()
                exit(0)
            }
            Button("Keep playing", role: .cancel) {}
        } message: {
            Text("Your progress is saved as the game saves it.")
        }
    }

    private func barButton(_ symbol: String, action: @escaping () -> Void) -> some View {
        Button(action: action) {
            Image(systemName: symbol)
                .font(.system(size: 12, weight: .bold))
                .foregroundStyle(.white.opacity(0.6))
                .frame(width: 28, height: 28)
                .background(Color.white.opacity(0.12), in: Circle())
        }
    }
}
