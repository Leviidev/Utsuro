import SwiftUI
import UIKit

/// Portrait for the app's own screens, landscape once the game is on.
final class AppDelegate: NSObject, UIApplicationDelegate {
    nonisolated(unsafe) static var orientations: UIInterfaceOrientationMask = .portrait

    func application(_ application: UIApplication, supportedInterfaceOrientationsFor window: UIWindow?) -> UIInterfaceOrientationMask {
        Self.orientations
    }

    @MainActor static func lock(_ mask: UIInterfaceOrientationMask) {
        orientations = mask
        for case let scene as UIWindowScene in UIApplication.shared.connectedScenes {
            scene.windows.first?.rootViewController?.setNeedsUpdateOfSupportedInterfaceOrientations()
            scene.requestGeometryUpdate(.iOS(interfaceOrientations: mask)) { error in
                UtsuroLog.log("ui", "rotation refused: \(error.localizedDescription)")
            }
        }
    }
}

@main
struct UtsuroApp: App {
    @UIApplicationDelegateAdaptor(AppDelegate.self) private var delegate
    @Environment(\.scenePhase) private var scenePhase

    init() {
        // Before anything can execute a brk: an unserviced one is a fatal SIGTRAP, not a failed call.
        UtsuroLog.start()
        JITBootstrap.noteLaunchState()
        JITBootstrap.installTrapGuard()
    }

    var body: some Scene {
        WindowGroup {
            HomeView()
                .preferredColorScheme(.dark)
                .onChange(of: scenePhase) { phase in
                    // StikDebug, TrollStore and the built-in helper all attach to a running Utsuro and leave it marked as debugged.
                    // The region is claimed the moment that is seen, while the debugger is still there to grant it.
                    if phase == .active, JITBootstrap.debuggedFlag, !JITBootstrap.prewarmed { JITBootstrap.prewarm() }
                }
        }
    }
}
