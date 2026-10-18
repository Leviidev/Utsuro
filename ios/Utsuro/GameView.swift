import SwiftUI
import UIKit
import QuartzCore

/// Runs the title on a thread of its own. There is one title per process, and it ends the process's JIT session when it returns.
enum GameRunner {
    nonisolated(unsafe) private static var started = false
    static var isRunning: Bool { started }

    static func start(dump: URL, layer: CAMetalLayer, finished: @escaping (Int32) -> Void) {
        guard !started else { return }
        started = true
        layer.drawableSize = CGSize(width: 1920, height: 1080)
        let layerPointer = Unmanaged.passUnretained(layer).toOpaque()
        let documents = GameLibrary.documents
        try? FileManager.default.createDirectory(at: GameLibrary.saves, withIntermediateDirectories: true)
        setenv("UTSURO_SAVES", GameLibrary.saves.path, 1)
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
        guard !delivered, bounds.width > 0, let layer = layer as? CAMetalLayer else { return }
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
    let dump: URL
    let onExit: () -> Void
    @StateObject private var pad = VirtualPad()
    @State private var layout = PadLayout.load("minecraft")
    @State private var confirmQuit = false
    @AppStorage("utsuro.padOpacity") private var padOpacity = 0.6

    var body: some View {
        ZStack {
            Color.black.ignoresSafeArea()
            MetalHost { layer in
                GameRunner.start(dump: dump, layer: layer) { _ in onExit() }
            }
            .aspectRatio(16.0 / 9.0, contentMode: .fit)
            VirtualPadView(pad: pad, opacity: padOpacity, haptics: true, layout: layout)
                .ignoresSafeArea()
            VStack {
                HStack {
                    Spacer()
                    Button { confirmQuit = true } label: {
                        Image(systemName: "xmark")
                            .font(.system(size: 12, weight: .bold))
                            .foregroundStyle(.white.opacity(0.6))
                            .frame(width: 28, height: 28)
                            .background(Color.white.opacity(0.12), in: Circle())
                    }
                    Spacer()
                }
                .padding(.top, 4)
                Spacer()
            }
        }
        .statusBarHidden(true)
        .persistentSystemOverlays(.hidden)
        .alert("Quit Minecraft?", isPresented: $confirmQuit) {
            Button("Quit", role: .destructive) {
                // The guest has no clean shutdown from outside; saves are written as the game makes them.
                UtsuroLog.log("game", "quit from the on-screen button")
                UtsuroLog.flushNow()
                exit(0)
            }
            Button("Keep playing", role: .cancel) {}
        } message: {
            Text("Your world is saved as the game saves it.")
        }
    }
}
