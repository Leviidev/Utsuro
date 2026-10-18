// SPDX-License-Identifier: GPL-2.0-or-later
import SwiftUI
import UIKit

/// A controller on the glass. Minecraft: Xbox One Edition is played with a gamepad (its menus answer nothing else), so this is the
/// whole controller: a D-pad, two analog sticks and their clicks, A/B/X/Y, both bumpers and triggers, Menu and View. It reports
/// what a physical controller would, and a real one (GameController) is merged with it by the runtime.
@MainActor
final class VirtualPad: ObservableObject {
    /// Windows.Xbox.Input.GamepadButtons bit values, which is what the runtime reports to the game.
    enum Button: UInt32 {
        case a, b, x, y, l1, r1, l3, r3, start, select, up, down, left, right
        var mask: UInt32 {
            switch self {
            case .start: return 0x4
            case .select: return 0x8
            case .a: return 0x10
            case .b: return 0x20
            case .x: return 0x40
            case .y: return 0x80
            case .up: return 0x100
            case .down: return 0x200
            case .left: return 0x400
            case .right: return 0x800
            case .l1: return 0x1000
            case .r1: return 0x2000
            case .l3: return 0x4000
            case .r3: return 0x8000
            }
        }
    }

    /// Everything a controller reports at one moment. Sticks run -1...1 with y up, as GameController gives them.
    struct State: Equatable {
        var buttons: UInt32 = 0
        var lx: Float = 0, ly: Float = 0, rx: Float = 0, ry: Float = 0
        var lt: Float = 0, rt: Float = 0
    }

    private var last = State()

    func connect() {}

    func disconnect() {
        last = State()
        utsuro_ios_set_pad(0, 0, 0, 0, 0, 0, 0)
    }

    func send(_ state: State) {
        guard state != last else { return }
        last = state
        utsuro_ios_set_pad(state.buttons, state.lx, state.ly, state.rx, state.ry, state.lt, state.rt)
    }
}

/// Where a player has moved, resized or hidden the pad's controls, per game. The default layout is worked out from the
/// screen; this is applied over it, so a layout made in one orientation or on one phone still fits another.
struct PadLayout: Codable, Equatable {
    struct Adjust: Codable, Equatable {
        var dx: CGFloat = 0, dy: CGFloat = 0     // the move, as a fraction of the screen's width and height
        var scale: CGFloat = 1
        var hidden = false
    }
    var controls: [String: Adjust] = [:]

    subscript(_ id: String) -> Adjust {
        get { controls[id] ?? Adjust() }
        set { controls[id] = newValue == Adjust() ? nil : newValue }
    }

    private static func file(_ appID: String) -> URL {
        FileManager.default.urls(for: .documentDirectory, in: .userDomainMask)[0].appendingPathComponent("pad-layout-\(appID).json")
    }
    static func load(_ appID: String) -> PadLayout {
        guard let data = try? Data(contentsOf: file(appID)), let l = try? JSONDecoder().decode(PadLayout.self, from: data) else { return PadLayout() }
        return l
    }
    func save(_ appID: String) {
        if controls.isEmpty { try? FileManager.default.removeItem(at: Self.file(appID)); return }
        if let data = try? JSONEncoder().encode(self) { try? data.write(to: Self.file(appID), options: .atomic) }
    }

    /// The names the editor shows.
    static func name(_ id: String) -> String {
        ["lstick": "Left stick", "rstick": "Right stick", "dpad": "D-pad", "back": "View", "start": "Menu"][id] ?? id.uppercased()
    }
}

/// The pad as an overlay on the game. It is one UIKit view that does its own multi-touch hit-testing and drawing rather than a
/// SwiftUI view per key: with a gesture on every key, SwiftUI has to settle which of several overlapping recognizers owns a touch
/// before any of them fires (a key only registered once the finger had moved), a key's hit area was just its drawn size so a
/// thumb landing between two keys pressed neither, and a finger could not roll from one key to the next. Here every touch is
/// followed for itself, a key takes any touch within a generous margin of it (the nearest key wins), and the hit areas and the
/// drawing come from the same layout so they cannot drift apart. A touch that lands on no control falls through to the game.
struct VirtualPadView: UIViewRepresentable {
    let pad: VirtualPad
    var opacity: Double = 1
    var haptics = true
    var layout = PadLayout()
    /// Editing: touches move controls instead of pressing them, and the one touched last is `selected`.
    var editing = false
    var selected: Binding<String?>? = nil
    var onChange: ((PadLayout) -> Void)? = nil

    func makeUIView(context: Context) -> PadView {
        pad.connect()
        let view = PadView(pad: pad)
        update(view)
        return view
    }
    func updateUIView(_ view: PadView, context: Context) { update(view) }

    private func update(_ view: PadView) {
        view.alpha = editing ? max(opacity, 0.85) : opacity
        view.hapticsEnabled = haptics
        view.onChange = onChange
        view.onSelect = { id in selected?.wrappedValue = id }
        view.configure(layout: layout, editing: editing, selected: selected?.wrappedValue)
    }
    static func dismantleUIView(_ view: PadView, coordinator: ()) { view.pad.disconnect() }
}

@MainActor
final class PadView: UIView {
    let pad: VirtualPad

    private enum Kind {
        case button(VirtualPad.Button, String)       // a round key
        case bumper(VirtualPad.Button, String)       // a long key
        case trigger(left: Bool, String)             // a long key that reports a trigger value
        case stick(left: Bool)
        case dpad
    }
    private struct Control {
        let kind: Kind
        var center: CGPoint
        var size: CGSize                              // a circle's diameter in width, or the key's width and height
        var hidden = false
        var radius: CGFloat { min(size.width, size.height) / 2 }

        /// Its name in a saved layout.
        var id: String {
            switch kind {
            case .button(_, let l), .bumper(_, let l), .trigger(_, let l): return l.lowercased()
            case .stick(let left): return left ? "lstick" : "rstick"
            case .dpad: return "dpad"
            }
        }
    }
    /// What one finger is doing: holding a stick, or pressing whatever is under it (and following it as it slides).
    private struct Finger {
        var stick: Int?
        var vector = CGPoint.zero                     // a held stick's push, -1...1, y down as the screen has it
        var buttons: UInt32 = 0
        var lt = false, rt = false
    }

    private var controls: [Control] = []
    private var fingers: [UITouch: Finger] = [:]
    private var shown = VirtualPad.State()
    private let haptic = UIImpactFeedbackGenerator(style: .light)
    var hapticsEnabled = true

    // Editing.
    private var adjustments = PadLayout()
    private var editing = false
    private var selectedID: String?
    private var drag: (id: String, start: CGPoint, from: PadLayout.Adjust)?
    var onChange: ((PadLayout) -> Void)?
    var onSelect: ((String?) -> Void)?

    func configure(layout: PadLayout, editing: Bool, selected: String?) {
        let changed = layout != adjustments || editing != self.editing || selected != selectedID
        adjustments = layout
        if editing != self.editing { fingers.removeAll(); drag = nil; if editing { pad.send(VirtualPad.State()) } }
        self.editing = editing
        selectedID = selected
        if changed { setNeedsLayout(); setNeedsDisplay() }
    }

    init(pad: VirtualPad) {
        self.pad = pad
        super.init(frame: .zero)
        isMultipleTouchEnabled = true
        isOpaque = false
        backgroundColor = .clear
        contentMode = .redraw
    }
    required init?(coder: NSCoder) { fatalError("not used") }

    // MARK: layout

    override func layoutSubviews() {
        super.layoutSubviews()
        controls = layout(in: bounds).map { c in
            let a = adjustments[c.id]
            var c = c
            c.center = CGPoint(x: c.center.x + a.dx * bounds.width, y: c.center.y + a.dy * bounds.height)
            c.size = CGSize(width: c.size.width * a.scale, height: c.size.height * a.scale)
            c.hidden = a.hidden
            return c
        }
        setNeedsDisplay()
    }

    /// Where every control sits, from the size of the screen and the space the notch and the rounded corners take.
    private func layout(in bounds: CGRect) -> [Control] {
        let w = bounds.width, h = bounds.height
        guard w > 0, h > 0 else { return [] }
        let isPad = UIDevice.current.userInterfaceIdiom == .pad
        // On iPad, clamp k to a comfortable hand reach size rather than scaling indefinitely with height
        let k = isPad ? 1.15 : max(0.7, min(1.15, h / 372))
        let insets = window?.safeAreaInsets ?? safeAreaInsets
        let left = max(insets.left, isPad ? 20 : 8), right = max(insets.right, isPad ? 20 : 8), bottom = max(insets.bottom, isPad ? 20 : 8)
        let m = 16 * k
        var list: [Control] = []

        let stickR = 62 * k
        list.append(Control(kind: .stick(left: true), center: CGPoint(x: left + m + stickR, y: h - bottom - m - stickR), size: CGSize(width: stickR * 2, height: stickR * 2)))
        let padR = 46 * k
        list.append(Control(kind: .dpad, center: CGPoint(x: left + m + padR, y: 106 * k), size: CGSize(width: padR * 2, height: padR * 2)))

        let face = 28 * k, spread = 58 * k
        let fc = CGPoint(x: w - right - m - spread - face, y: h - bottom - m - spread - face)
        list.append(Control(kind: .button(.y, "Y"), center: CGPoint(x: fc.x, y: fc.y - spread), size: CGSize(width: face * 2, height: face * 2)))
        list.append(Control(kind: .button(.a, "A"), center: CGPoint(x: fc.x, y: fc.y + spread), size: CGSize(width: face * 2, height: face * 2)))
        list.append(Control(kind: .button(.x, "X"), center: CGPoint(x: fc.x - spread, y: fc.y), size: CGSize(width: face * 2, height: face * 2)))
        list.append(Control(kind: .button(.b, "B"), center: CGPoint(x: fc.x + spread, y: fc.y), size: CGSize(width: face * 2, height: face * 2)))

        let rStickR = 46 * k
        list.append(Control(kind: .stick(left: false), center: CGPoint(x: w - right - m - rStickR - 10 * k, y: 106 * k), size: CGSize(width: rStickR * 2, height: rStickR * 2)))

        let bw = 64 * k, bh = 34 * k
        list.append(Control(kind: .trigger(left: true, "L2"), center: CGPoint(x: left + m + bw / 2, y: 10 * k + bh / 2), size: CGSize(width: bw, height: bh)))
        list.append(Control(kind: .bumper(.l1, "L1"), center: CGPoint(x: left + m + bw * 1.5 + 8 * k, y: 10 * k + bh / 2), size: CGSize(width: bw, height: bh)))
        list.append(Control(kind: .trigger(left: false, "R2"), center: CGPoint(x: w - right - m - bw / 2, y: 10 * k + bh / 2), size: CGSize(width: bw, height: bh)))
        list.append(Control(kind: .bumper(.r1, "R1"), center: CGPoint(x: w - right - m - bw * 1.5 - 8 * k, y: 10 * k + bh / 2), size: CGSize(width: bw, height: bh)))

        let clickR = 20 * k
        list.append(Control(kind: .button(.l3, "L3"), center: CGPoint(x: left + m + stickR * 2 + clickR + 6 * k, y: h - bottom - m - stickR * 2 + clickR), size: CGSize(width: clickR * 2, height: clickR * 2)))
        list.append(Control(kind: .button(.r3, "R3"), center: CGPoint(x: w - right - m - rStickR - 10 * k, y: 106 * k + rStickR + clickR + 8 * k), size: CGSize(width: clickR * 2, height: clickR * 2)))

        let sw = 60 * k, sh = 26 * k
        list.append(Control(kind: .bumper(.select, "View"), center: CGPoint(x: w / 2 - sw / 2 - 12 * k, y: 10 * k + sh / 2 + 4 * k), size: CGSize(width: sw, height: sh)))
        list.append(Control(kind: .bumper(.start, "Menu"), center: CGPoint(x: w / 2 + sw / 2 + 12 * k, y: 10 * k + sh / 2 + 4 * k), size: CGSize(width: sw, height: sh)))
        return list
    }

    // MARK: hit-testing

    /// How far a touch at `p` is from a control, as a fraction of how far it may be and still count: under 1 is a hit.
    private func reach(_ c: Control, _ p: CGPoint) -> CGFloat {
        let dx = p.x - c.center.x, dy = p.y - c.center.y
        switch c.kind {
        case .stick: return hypot(dx, dy) / (c.radius * 1.3)
        case .dpad: return hypot(dx, dy) / (c.radius * 1.25)
        case .button: return hypot(dx, dy) / (c.radius * 1.4)
        case .bumper, .trigger:
            let sx = abs(dx) / (c.size.width / 2 + 8), sy = abs(dy) / (c.size.height / 2 + 8)
            return max(sx, sy)
        }
    }

    private func control(at p: CGPoint) -> Int? {
        var best: (Int, CGFloat)?
        for (i, c) in controls.enumerated() {
            if c.hidden && !editing { continue }
            let r = reach(c, p)
            if r <= 1, best == nil || r < best!.1 { best = (i, r) }
        }
        return best?.0
    }

    /// Only the controls take touches; everywhere else the game underneath gets them.
    override func hitTest(_ point: CGPoint, with event: UIEvent?) -> UIView? {
        if editing { return self }      // while editing, the game gets nothing
        return control(at: point) != nil ? self : nil
    }

    /// What a finger at `p` presses, for one that is not holding a stick.
    private func press(at p: CGPoint) -> Finger {
        var f = Finger()
        guard let i = control(at: p) else { return f }
        let c = controls[i]
        switch c.kind {
        case .button(let b, _), .bumper(let b, _): f.buttons = b.mask
        case .trigger(let left, _): if left { f.lt = true } else { f.rt = true }
        case .dpad: f.buttons = dpadBits(p, c)
        case .stick: break
        }
        return f
    }

    /// The D-pad as eight directions: a touch toward a corner is both of its directions, as on a real one, and the middle is dead.
    private func dpadBits(_ p: CGPoint, _ c: Control) -> UInt32 {
        let dx = p.x - c.center.x, dy = p.y - c.center.y
        if hypot(dx, dy) < c.radius * 0.22 { return 0 }
        var deg = atan2(-dy, dx) * 180 / .pi
        if deg < 0 { deg += 360 }
        typealias B = VirtualPad.Button
        switch deg {
        case 22.5..<67.5: return B.up.mask | B.right.mask
        case 67.5..<112.5: return B.up.mask
        case 112.5..<157.5: return B.up.mask | B.left.mask
        case 157.5..<202.5: return B.left.mask
        case 202.5..<247.5: return B.down.mask | B.left.mask
        case 247.5..<292.5: return B.down.mask
        case 292.5..<337.5: return B.down.mask | B.right.mask
        default: return B.right.mask
        }
    }

    // MARK: touches

    override func touchesBegan(_ touches: Set<UITouch>, with event: UIEvent?) {
        if editing {
            guard let t = touches.first else { return }
            let p = t.location(in: self)
            if let i = control(at: p) {
                let id = controls[i].id
                drag = (id, p, adjustments[id])
                selectedID = id
            } else {
                selectedID = nil
            }
            onSelect?(selectedID)
            setNeedsDisplay()
            return
        }
        for t in touches {
            let p = t.location(in: self)
            if let i = control(at: p), case .stick = controls[i].kind {
                var f = Finger(); f.stick = i
                f.vector = stickVector(p, controls[i])
                fingers[t] = f
            } else {
                fingers[t] = press(at: p)
            }
        }
        apply()
    }

    override func touchesMoved(_ touches: Set<UITouch>, with event: UIEvent?) {
        if editing {
            guard let d = drag, let t = touches.first, bounds.width > 0, bounds.height > 0 else { return }
            let p = t.location(in: self)
            var a = d.from
            a.dx += (p.x - d.start.x) / bounds.width
            a.dy += (p.y - d.start.y) / bounds.height
            adjustments[d.id] = a
            setNeedsLayout()
            return
        }
        for t in touches {
            guard var f = fingers[t] else { continue }
            let p = t.location(in: self)
            if let i = f.stick { f.vector = stickVector(p, controls[i]) } else { f = press(at: p) }
            fingers[t] = f
        }
        apply()
    }

    override func touchesEnded(_ touches: Set<UITouch>, with event: UIEvent?) {
        if editing {
            if drag != nil { drag = nil; onChange?(adjustments) }
            return
        }
        for t in touches { fingers[t] = nil }
        apply()
    }

    override func touchesCancelled(_ touches: Set<UITouch>, with event: UIEvent?) {
        if editing { drag = nil; return }
        for t in touches { fingers[t] = nil }
        apply()
    }

    /// A stick's push: how far the finger is from its centre, as a fraction of the stick's radius, capped at one.
    private func stickVector(_ p: CGPoint, _ c: Control) -> CGPoint {
        let dx = p.x - c.center.x, dy = p.y - c.center.y
        let d = hypot(dx, dy)
        guard d > 0 else { return .zero }
        let s = min(d, c.radius) / c.radius
        return CGPoint(x: dx / d * s, y: dy / d * s)
    }

    /// Fold every finger into one controller state and report it.
    private func apply() {
        var s = VirtualPad.State()
        for (_, f) in fingers {
            s.buttons |= f.buttons
            if f.lt { s.lt = 1 }
            if f.rt { s.rt = 1 }
            if let i = f.stick, case .stick(let left) = controls[i].kind {
                if left { s.lx = Float(f.vector.x); s.ly = Float(-f.vector.y) } else { s.rx = Float(f.vector.x); s.ry = Float(-f.vector.y) }
            }
        }
        if hapticsEnabled, s.buttons & ~shown.buttons != 0 || (s.lt > shown.lt) || (s.rt > shown.rt) { haptic.impactOccurred() }
        shown = s
        pad.send(s)
        setNeedsDisplay()
    }

    // MARK: drawing

    override func draw(_ rect: CGRect) {
        for c in controls {
            if c.hidden && !editing { continue }
            let ctx = UIGraphicsGetCurrentContext()
            ctx?.saveGState()
            if c.hidden { ctx?.setAlpha(0.3) }
            defer {
                ctx?.restoreGState()
                if editing, c.id == selectedID {
                    let pad: CGFloat = 6
                    let box = CGRect(x: c.center.x - c.size.width / 2 - pad, y: c.center.y - c.size.height / 2 - pad,
                                     width: c.size.width + pad * 2, height: c.size.height + pad * 2)
                    let sel = UIBezierPath(roundedRect: box, cornerRadius: 10)
                    sel.lineWidth = 2
                    sel.setLineDash([6, 4], count: 2, phase: 0)
                    UIColor.systemYellow.setStroke(); sel.stroke()
                }
            }
            switch c.kind {
            case .stick(let left):
                drawStick(c, push: stickPush(left: left))
            case .dpad:
                drawDPad(c)
            case .button(let b, let label):
                drawRound(c, label: label, down: shown.buttons & b.mask != 0)
            case .bumper(let b, let label):
                drawKey(c, label: label, down: shown.buttons & b.mask != 0)
            case .trigger(let left, let label):
                drawKey(c, label: label, down: (left ? shown.lt : shown.rt) > 0)
            }
        }
    }

    /// How far the finger holding one of the sticks has pushed it, if one is.
    private func stickPush(left: Bool) -> CGPoint? {
        for f in fingers.values {
            guard let i = f.stick, case .stick(let l) = controls[i].kind, l == left else { continue }
            return f.vector
        }
        return nil
    }

    private func fill(_ a: CGFloat) -> UIColor { UIColor.white.withAlphaComponent(a) }

    private func drawStick(_ c: Control, push: CGPoint?) {
        let r = c.radius
        let base = UIBezierPath(arcCenter: c.center, radius: r, startAngle: 0, endAngle: .pi * 2, clockwise: true)
        fill(0.12).setFill(); base.fill()
        fill(0.35).setStroke(); base.lineWidth = 1.5; base.stroke()
        let reach = r * 0.55
        let at = CGPoint(x: c.center.x + (push?.x ?? 0) * reach, y: c.center.y + (push?.y ?? 0) * reach)
        let thumb = UIBezierPath(arcCenter: at, radius: r * 0.42, startAngle: 0, endAngle: .pi * 2, clockwise: true)
        fill(push == nil ? 0.28 : 0.55).setFill(); thumb.fill()
        fill(0.5).setStroke(); thumb.lineWidth = 1.5; thumb.stroke()
    }

    private func drawDPad(_ c: Control) {
        let r = c.radius
        let base = UIBezierPath(arcCenter: c.center, radius: r, startAngle: 0, endAngle: .pi * 2, clockwise: true)
        fill(0.12).setFill(); base.fill()
        fill(0.35).setStroke(); base.lineWidth = 1.5; base.stroke()
        typealias B = VirtualPad.Button
        let arrows: [(B, CGFloat, CGFloat)] = [(.up, 0, -1), (.down, 0, 1), (.left, -1, 0), (.right, 1, 0)]
        for (b, dx, dy) in arrows {
            let on = shown.buttons & b.mask != 0
            let tip = CGPoint(x: c.center.x + dx * r * 0.78, y: c.center.y + dy * r * 0.78)
            let back = CGPoint(x: c.center.x + dx * r * 0.42, y: c.center.y + dy * r * 0.42)
            let side = CGPoint(x: -dy, y: dx)                                  // across the arrow
            let half = r * 0.24
            let tri = UIBezierPath()
            tri.move(to: tip)
            tri.addLine(to: CGPoint(x: back.x + side.x * half, y: back.y + side.y * half))
            tri.addLine(to: CGPoint(x: back.x - side.x * half, y: back.y - side.y * half))
            tri.close()
            fill(on ? 0.85 : 0.4).setFill(); tri.fill()
        }
    }

    private func label(_ text: String, in rect: CGRect, size: CGFloat, alpha: CGFloat) {
        let attrs: [NSAttributedString.Key: Any] = [.font: UIFont.systemFont(ofSize: size, weight: .bold), .foregroundColor: fill(alpha)]
        let s = (text as NSString).size(withAttributes: attrs)
        (text as NSString).draw(at: CGPoint(x: rect.midX - s.width / 2, y: rect.midY - s.height / 2), withAttributes: attrs)
    }

    private func drawRound(_ c: Control, label text: String, down: Bool) {
        let r = c.radius
        let path = UIBezierPath(arcCenter: c.center, radius: r, startAngle: 0, endAngle: .pi * 2, clockwise: true)
        fill(down ? 0.45 : 0.18).setFill(); path.fill()
        fill(0.4).setStroke(); path.lineWidth = 1.5; path.stroke()
        label(text, in: CGRect(x: c.center.x - r, y: c.center.y - r, width: r * 2, height: r * 2), size: r * 0.8, alpha: down ? 1 : 0.85)
    }

    private func drawKey(_ c: Control, label text: String, down: Bool) {
        let rect = CGRect(x: c.center.x - c.size.width / 2, y: c.center.y - c.size.height / 2, width: c.size.width, height: c.size.height)
        let path = UIBezierPath(roundedRect: rect, cornerRadius: min(c.size.height / 2, 14))
        fill(down ? 0.45 : 0.18).setFill(); path.fill()
        fill(0.4).setStroke(); path.lineWidth = 1.5; path.stroke()
        label(text, in: rect, size: min(15, c.size.height * 0.46), alpha: down ? 1 : 0.85)
    }
}
