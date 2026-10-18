import SwiftUI
import UIKit

@main
struct UtsuroApp: App {
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
