// SPDX-License-Identifier: GPL-2.0-or-later
import Foundation
import UIKit
import os

/// `csops(2)`. Reading CS_DEBUGGED is how we ask "is a debugger attached right
/// now" without executing anything — unlike the brk probe, which costs a trap.
@_silgen_name("csops")
private func csops(_ pid: Int32, _ ops: Int32,
                   _ useraddr: UnsafeMutableRawPointer?, _ usersize: Int) -> Int32

private let CS_OPS_STATUS = Int32(0)
private let CS_DEBUGGED = UInt32(0x10000000)

/// Gets Utsuro from "launched normally, no executable memory" to "JIT is live".
///
/// On iOS 27 every supported device enforces TXM, so the app cannot grant itself
/// executable memory — only an attached debugger can. That debugger is either
/// StikDebug, which Utsuro hands the JIT script inline over its URL scheme, or
/// Utsuro's own built-in StikJIT helper (JITSetup.swift), which runs the same
/// script from an app extension.
enum JITBootstrap {
    private static let log = Logger(subsystem: "com.utsuro.app", category: "jit")

    enum State: Equatable {
        case unknown
        case waitingForDebugger
        case live
        case failed(String)
    }

    /// Install the trap guard. Must run before anything can execute a `brk`.
    ///
    /// Without this, a `brk` that StikDebug is not there to service is a fatal
    /// SIGTRAP rather than a failed call — the process simply dies.
    static func installTrapGuard() {
        UtsuroLog.log("jit", "installing brk trap guard")
        husk_ios_jit_install_trap_handler()
        UtsuroLog.log("jit", "trap guard installed -- an unserviced brk will now "
                         + "return 0 instead of killing the process")
    }

    /// 
    /// a smaller region here means QEMU allocates a second one, at a point where
    /// StikDebug may be long gone.
    // Back to 256 MiB. Raising this to 512 was one of three changes made at
    // once in v15, and v15 was the first build to die inside qemu_init(). The
    // guest RAM -- the other suspect -- has since been shown to map and write
    // cleanly at 6144 MiB, which leaves this. The region itself allocates and
    // passes its selftest at 512; whatever objects is further in, where TCG
    // carves the buffer into per-vCPU regions.
    //
    // 512 MiB since the native runtime runs Minecraft: its main library alone is a 354 MiB image that has to sit in this
    // region, with the stubs the loader places beside it. A larger prewarm is safe for QEMU, which is handed the
    // prewarmed region whenever it is at least what tb-size asks for.
    static let jitBytes = 256 * 1024 * 1024

    /// True once the region is held. The memory budget needs this: after a
    /// prewarm the JIT is already counted in the footprint, so subtracting it
    /// again charges for it twice and cost the guest 256 MiB.
    nonisolated(unsafe) static var prewarmed = false

    /// Take the JIT region now, while StikDebug is definitely still attached.
    ///
    /// StikDebug lets go after a while, and a first run spends a minute
    /// downloading 1.1 GB of guest image before QEMU starts. By the time
    /// qemu_init() asked for memory the debugger had detached, and there is no
    /// recovering from that in-process -- without a debugger there is no
    /// executable memory at all, and asking again later is precisely what does
    /// not work. So claim it first and hold it.
    @discardableResult
    static func prewarm() -> Bool {
        if prewarmed { return true }
        // Before iOS 26 Utsuro can make the region itself where the device allows it (TrollStore's dynamic-codesigning, a
        // jailbreak, or a debugger that attached and let go), so it does not wait for CS_DEBUGGED there: Dopamine hides that
        // flag from apps unless "Allow JIT in Apps" is on, while still letting them run code they wrote.
        guard isDebuggerAttached || (canGrantOwnJIT && !selfGrantTried) else {
            UtsuroLog.log("jit", "no debugger attached yet; not prewarming")
            return false
        }
        if !isDebuggerAttached { selfGrantTried = true }
        // Before iOS 26, unless StikDebug's script is what attached, no brk: TrollStore's enable-jit (and its Open with JIT)
        // attaches for 100 ms and services nothing, and a brk in that window stops Utsuro for good (TrollStore then says
        // "trollstorehelper returned 3"). A debugged process there makes its own executable memory anyway.
        if canGrantOwnJIT && !stikDebugMayServiceTraps {
            selfGrantTried = true
            let ok = utsuro_ios_prewarm_jit(true)
            UtsuroLog.log("jit", "made the JIT region without a trap servicer: \(ok ? "OK (\(husk_ios_jit_self_route().map { String(cString: $0) } ?? "?"))" : "no")")
            if ok { prewarmed = true; lastFailure = nil }
            return ok
        }
        UtsuroLog.log("jit", "claiming \(jitBytes / (1024 * 1024)) MiB of JIT memory now, "
                         + "before the guest download -- StikDebug does not stay attached")
        let ok = utsuro_ios_prewarm_jit(false)
        if ok {
            prewarmed = true
            lastFailure = nil
            detachIfDone()
        }
        else {
            lastFailure = "The debugger is attached but is not answering trap "
                        + "requests, and this device will not execute a MAP_JIT "
                        + "mapping either, so no executable memory could be "
                        + "claimed. This is what happens when Utsuro runs inside "
                        + "another container app rather than sideloaded on its own."
        }
        UtsuroLog.log("jit", ok ? "JIT region secured; held for the game"
                              : "JIT prewarm FAILED -- StikDebug is not servicing traps")
        return ok
    }

    /// Whether to leave StikDebug attached after the region is held.
    ///
    /// While a debugger is attached, every stop event (a `brk`, a signal, a
    /// Mach exception) halts the whole process until StikDebug answers. iOS
    /// suspends StikDebug soon after it hands the foreground back, so the next
    /// stop freezes Utsuro with nothing in the log. On iOS 26 that looks like
    /// Android hanging on its boot screen. The RX pages stay valid after detach
    /// (AetherPS4 runs this way on TXM devices), so detaching once the region is
    /// held removes the debugger from the picture.
    static var keepDebuggerAttached: Bool {
        get { UserDefaults.standard.bool(forKey: "utsuro.keepDebuggerAttached") }
        set { UserDefaults.standard.set(newValue, forKey: "utsuro.keepDebuggerAttached") }
    }

    /// Whether StikDebug's script may be the debugger: the only one that answers a brk.
    private static var stikDebugMayServiceTraps: Bool {
        let method = UserDefaults.standard.string(forKey: "utsuro.jitMethod") ?? JITMethod.automatic.rawValue
        return method == JITMethod.stikDebug.rawValue || (method == JITMethod.automatic.rawValue && isStikDebugInstalled)
    }

    /// The one try at making the region without a debugger has been made (it is not repeated: the answer will not change).
    nonisolated(unsafe) static private var selfGrantTried = false

    /// Whether Utsuro has JIT now: a debugger marked it, or the region is already held. Before iOS 26 this tries, once, to make
    /// the region itself, which is all TrollStore and jailbroken devices need. Cheap after the first call, and logs nothing.
    static var ready: Bool {
        if debuggedFlag || prewarmed || isLive { return true }
        guard canGrantOwnJIT, !selfGrantTried else { return false }
        return prewarm()
    }

    /// True once this process has told StikDebug to let go.
    nonisolated(unsafe) static private(set) var detached = false

    /// Release the debugger once the JIT region is held. It is no longer
    /// needed for anything, and leaving it attached is what froze the app.
    private static func detachIfDone() {
        guard prewarmed, !detached, !keepDebuggerAttached else { return }
        UtsuroLog.log("jit", "JIT region held; detaching StikDebug so a suspended "
                         + "debugger cannot stall the process later")
        husk_ios_jit_detach()
        detached = true
    }

    /// Whether this device enforces TXM, so that only a trap servicer can grant
    /// executable memory. Uses the same rule as the "TXM expected" line in the
    /// log banner, so the two can never disagree.
    static let deviceEnforcesTXM: Bool = UtsuroLog.expectsTXM(model: UtsuroLog.deviceModel)

    /// Why the last prewarm failed, for the UI to show.
    ///
    /// CS_DEBUGGED being set is not the same as the debugger servicing traps.
    /// A build running inside LiveContainer reports itself debugged, answers no
    /// brk, and gets no executable memory -- and Utsuro started QEMU anyway,
    /// which segfaulted inside qemu_init() with a perfectly healthy 4 GB of
    /// headroom. The crash looked like memory pressure and was nothing of the
    /// kind.
    nonisolated(unsafe) static var lastFailure: String?

    /// True only after a JIT region has been allocated AND passed the execute
    /// self-test — which happens inside `qemu_init`. It is therefore always false
    /// before the guest starts, and must NOT be used to decide whether to start it.
    /// Use `isDebuggerAttached` for that, and this afterwards to confirm it worked.
    static var isLive: Bool { husk_ios_jit_is_available() }

    /// The actual precondition for starting the guest: StikDebug has attached.
    ///
    /// Checked with csops/CS_DEBUGGED rather than the `brk #0x69` probe, because
    /// this costs nothing and is safe to call repeatedly — the brk probe traps
    /// every time, and every trap is a chance to hit a moment when StikDebug is
    /// not listening. The brk probe still runs once, inside the allocator, where
    /// its answer is immediately acted on.
    static var isDebuggerAttached: Bool {
        guard let flags = csStatus() else {
            UtsuroLog.log("jit", "csops failed (errno \(errno)); assuming no debugger")
            return false
        }
        let attached = (flags & CS_DEBUGGED) != 0
        UtsuroLog.log("jit", String(format: "csops status = 0x%08x, CS_DEBUGGED = %@",
                                  flags, attached ? "set" : "clear"))
        return attached
    }

    /// `isDebuggerAttached` without the log line, for polling while the
    /// built-in helper attaches.
    static var debuggedFlag: Bool {
        (csStatus() ?? 0) & CS_DEBUGGED != 0
    }

    private static func csStatus() -> UInt32? {
        var flags: UInt32 = 0
        let rc = withUnsafeMutableBytes(of: &flags) { buf in
            csops(getpid(), CS_OPS_STATUS, buf.baseAddress, buf.count)
        }
        return rc == 0 ? flags : nil
    }

    static var isStikDebugInstalled: Bool {
        URL(string: "stikjit://").map(UIApplication.shared.canOpenURL) ?? false
    }

    /// Whether TrollStore is on this device. It answers enable-jit on the `apple-magnifier` scheme, but that scheme is also the
    /// Magnifier app's, on every iPhone, so whether it opens says nothing. A Utsuro TrollStore installed knows from its marker file;
    /// otherwise SpringBoard is asked to open TrollStore, suspended, which an ordinary app is refused (9) when it exists and told
    /// it does not (7) when it does not. TrollStore installs on iOS 14.0 to 17.0 only.
    static let isTrollStoreInstalled: Bool = {
        if isInstalledWithTrollStore { return true }
        if #available(iOS 17.0.1, *) { return false }
        typealias Launch = @convention(c) (NSString, NSURL?, NSDictionary?, NSDictionary?, Bool) -> Int32
        guard let lib = dlopen("/System/Library/PrivateFrameworks/SpringBoardServices.framework/SpringBoardServices", RTLD_NOW),
              let sym = dlsym(lib, "SBSLaunchApplicationWithIdentifierAndURLAndLaunchOptions") else { return false }
        let launch = unsafeBitCast(sym, to: Launch.self)
        for id in ["com.opa334.TrollStore", "com.opa334.TrollStoreLite"] {
            let r = launch(id as NSString, nil, nil, nil, true)
            UtsuroLog.log("jit", "SpringBoard on opening \(id): \(r)")
            if r == 9 { return true }
        }
        return false
    }()

    /// Whether this copy of Utsuro was installed by TrollStore (or TrollStore Lite): it leaves a marker file next to the app in
    /// its bundle container (TrollStore's `TS_MARKER`). Only then is it a TrollStore app, which keeps the entitlements it was
    /// built with -- including the memory ones.
    static var isInstalledWithTrollStore: Bool {
        let container = Bundle.main.bundleURL.deletingLastPathComponent()
        return ["_TrollStore", "_TrollStoreLite"].contains {
            FileManager.default.fileExists(atPath: container.appendingPathComponent($0).path)
        }
    }

    /// A rootless jailbreak (Dopamine and its kin) puts its files in /var/jb and lets apps see it.
    static var isJailbroken: Bool { FileManager.default.fileExists(atPath: "/var/jb") }

    /// Whether Utsuro was already marked as debugged when it was opened, before it asked anything of anyone. That is what a
    /// jailbreak that allows JIT in apps does to every app it launches (Dopamine's "Allow JIT in Apps" setting), and it needs
    /// no hand-off at all. Read once, early (UtsuroApp.init), so a later attach is not mistaken for it.
    nonisolated(unsafe) static var debuggedAtLaunch = false
    static func noteLaunchState() { debuggedAtLaunch = (csStatus() ?? 0) & CS_DEBUGGED != 0 }

    /// Whether this iOS lets a process that is marked as debugged make executable memory for itself, as opposed to needing a
    /// debugger to hand it out: true before iOS 26, which is when the Trusted Execution Monitor arrived. It is what makes
    /// TrollStore and jailbreaks enough, without StikDebug.
    static var canGrantOwnJIT: Bool {
        if #available(iOS 26, *) { return false }
        return true
    }

    /// utsuro-jit.js as standard base64, which Built-in StikJIT's custom script takes.
    static var scriptBase64: String? {
        loadScript()?.data(using: .utf8)?.base64EncodedString()
    }

    /// Ask StikDebug to attach to us and run the JIT script.
    ///
    /// This backgrounds Utsuro — iOS switches to StikDebug, which attaches over the
    /// debugserver protocol, runs the script, and relaunches us. Everything after
    /// this point happens in a *new* foreground pass of the app.
    @MainActor
    static func requestTrollStoreAttach() -> Bool {
        UtsuroLog.log("jit", "requestTrollStoreAttach() -- handing off to TrollStore")
        guard let bundleID = Bundle.main.bundleIdentifier else { return false }
        
        let url = "apple-magnifier://enable-jit?bundle-id=\(bundleID)"
        guard isTrollStoreInstalled, let launchURL = URL(string: url) else {
            UtsuroLog.log("jit", "FAIL: TrollStore is not installed")
            return false
        }
        
        UtsuroLog.log("jit", "opening apple-magnifier:// for bundle \(bundleID)")
        UIApplication.shared.open(launchURL)
        return true
    }

    @MainActor
    static func requestAttach() -> Bool {
        UtsuroLog.log("jit", "requestAttach() -- handing off to StikDebug")

        guard let bundleID = Bundle.main.bundleIdentifier else {
            UtsuroLog.log("jit", "FAIL: no bundle identifier")
            return false
        }

        var url = "stikjit://enable-jit?bundle-id=\(bundleID)"

        // Sending the script inline makes Utsuro self-contained. StikDebug accepts
        // script-data unconditionally (HomeView.handleExternalURL), so we do not
        // depend on the user having assigned a script to Utsuro in StikDebug's UI.
        if let script = loadScript(), let encoded = base64URLEncode(script) {
            UtsuroLog.log("jit", "loaded utsuro-jit.js (\(script.count) chars, "
                             + "\(encoded.count) chars encoded); sending inline")
            url += "&script-data=\(encoded)"
        } else {
            UtsuroLog.log("jit", "WARNING: could not load utsuro-jit.js from the bundle. "
                             + "Falling back to whatever script StikDebug has assigned "
                             + "to this bundle id -- if none, JIT will not be enabled.")
        }

        guard let launchURL = URL(string: url) else {
            UtsuroLog.log("jit", "FAIL: malformed StikDebug URL (\(url.count) chars)")
            return false
        }

        guard UIApplication.shared.canOpenURL(launchURL) else {
            UtsuroLog.log("jit", "FAIL: cannot open stikjit:// -- StikDebug is not "
                             + "installed, or LSApplicationQueriesSchemes is missing "
                             + "the 'stikjit' entry in Info.plist")
            return false
        }

        UtsuroLog.log("jit", "opening stikjit:// for bundle \(bundleID); "
                         + "Utsuro will be backgrounded and relaunched after attach")
        UIApplication.shared.open(launchURL)
        return true
    }

    private static func loadScript() -> String? {
        // Accept either bundle layout: the build copies utsuro-jit.js to the bundle
        // root, but a folder-reference build phase would put it under Resources/.
        let candidates = [
            Bundle.main.path(forResource: "utsuro-jit", ofType: "js"),
            Bundle.main.path(forResource: "utsuro-jit", ofType: "js", inDirectory: "Resources"),
        ]
        for case let path? in candidates {
            if let s = try? String(contentsOfFile: path, encoding: .utf8) { return s }
        }
        return nil
    }

    /// StikDebug expects base64url (`-`/`_`, no padding) and percent-encodes it
    /// into the query string.
    private static func base64URLEncode(_ s: String) -> String? {
        guard let data = s.data(using: .utf8) else { return nil }
        let b64 = data.base64EncodedString()
            .replacingOccurrences(of: "+", with: "-")
            .replacingOccurrences(of: "/", with: "_")
            .trimmingCharacters(in: CharacterSet(charactersIn: "="))
        return b64.addingPercentEncoding(withAllowedCharacters: .urlQueryAllowed)
    }
}
