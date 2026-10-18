// input.h -- the host's controller: keyboard keys and any attached game controller, merged into
// one Xbox pad state that Windows.Xbox.Input.Gamepad reports.
#pragma once

#include <cstdint>

namespace utsuro::input {

/// GamepadButtons bit values from Windows.Xbox.Input.
enum Button : uint32_t {
    kMenu = 0x4, kView = 0x8, kA = 0x10, kB = 0x20, kX = 0x40, kY = 0x80,
    kDPadUp = 0x100, kDPadDown = 0x200, kDPadLeft = 0x400, kDPadRight = 0x800,
    kLeftShoulder = 0x1000, kRightShoulder = 0x2000, kLeftThumb = 0x4000, kRightThumb = 0x8000,
};

struct PadState {
    uint32_t buttons {0};
    float left_trigger {0}, right_trigger {0};
    float left_x {0}, left_y {0}, right_x {0}, right_y {0};
};

/// The current merged state (callable from any thread).
PadState Poll();

/// Window side: a key went down or up (macOS virtual key code) / mouse motion in points.
void KeyEvent(uint16_t key_code, bool down);
void MouseMoved(float dx, float dy);
void MouseButton(int button, bool down);

} // namespace utsuro::input
