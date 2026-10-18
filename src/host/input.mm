#include "host/input.h"

#include "gpu/renderer.h"

#import <Foundation/Foundation.h>
#import <GameController/GameController.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <sstream>
#include <thread>
#include <sys/stat.h>
#include <cstdio>
#include <string>
#include <vector>

namespace utsuro::input {

namespace {

std::mutex g_mutex;
bool g_keys[256] {};
std::atomic<float> g_mouse_dx {0}, g_mouse_dy {0};
std::atomic<bool> g_mouse_buttons[3] {};

// macOS virtual key codes.
enum Key : uint16_t {
    kKeyA = 0x00, kKeyS = 0x01, kKeyD = 0x02, kKeyQ = 0x0C, kKeyW = 0x0D, kKeyE = 0x0E, kKeyZ = 0x06, kKeyX = 0x07, kKeyC = 0x08, kKeyV = 0x09,
    kKeyY = 0x10, kKeyP = 0x23, kKeyI = 0x22, kKeyJ = 0x26, kKeyK = 0x28, kKeyL = 0x25,
    kReturn = 0x24, kTab = 0x30, kSpace = 0x31, kBackspace = 0x33, kEscape = 0x35, kShift = 0x38,
    kLeft = 0x7B, kRight = 0x7C, kDown = 0x7D, kUp = 0x7E,
};

bool Down(uint16_t k) { return k < 256 && g_keys[k]; }

// UTSURO_SCRIPT="seconds:BUTTON:hold, ..." presses buttons on a schedule (for unattended runs).
struct ScriptEvent { double at, hold; std::string name; };
std::vector<ScriptEvent> LoadScript() {
    std::vector<ScriptEvent> out;
    const char* env = std::getenv("UTSURO_SCRIPT");
    if (env == nullptr) return out;
    std::stringstream ss(env);
    std::string item;
    while (std::getline(ss, item, ',')) {
        std::stringstream is(item);
        std::string a, b, c;
        std::getline(is, a, ':'); std::getline(is, b, ':'); std::getline(is, c, ':');
        if (a.empty() || b.empty()) continue;
        out.push_back({std::atof(a.c_str()), c.empty() ? 0.3 : std::atof(c.c_str()), b});
    }
    return out;
}

void ApplyScript(PadState& s) {
    static const std::vector<ScriptEvent> script = LoadScript();
    static const auto start = std::chrono::steady_clock::now();
    if (script.empty()) return;
    const double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    for (const ScriptEvent& e : script) {
        if (t < e.at || t >= e.at + e.hold) continue;
        const std::string& n = e.name;
        if (n == "A") s.buttons |= kA; else if (n == "B") s.buttons |= kB; else if (n == "X") s.buttons |= kX; else if (n == "Y") s.buttons |= kY;
        else if (n == "START") s.buttons |= kMenu; else if (n == "BACK") s.buttons |= kView;
        else if (n == "LB") s.buttons |= kLeftShoulder; else if (n == "RB") s.buttons |= kRightShoulder;
        else if (n == "UP") s.buttons |= kDPadUp; else if (n == "DOWN") s.buttons |= kDPadDown;
        else if (n == "LEFT") s.buttons |= kDPadLeft; else if (n == "RIGHT") s.buttons |= kDPadRight;
        else if (n == "LSUP") s.left_y = 1; else if (n == "LSDOWN") s.left_y = -1; else if (n == "LSLEFT") s.left_x = -1; else if (n == "LSRIGHT") s.left_x = 1;
        else if (n == "RSUP") s.right_y = 1; else if (n == "RSDOWN") s.right_y = -1; else if (n == "RSLEFT") s.right_x = -1; else if (n == "RSRIGHT") s.right_x = 1;
        else if (n == "RT") s.right_trigger = 1; else if (n == "LT") s.left_trigger = 1;
    }
}

} // namespace

void RunCommand(const std::string& line);

void StartControl(const char* path) {
    if (path == nullptr || *path == 0) return;
    const std::string file = path;
    std::thread([file] {
        pthread_setname_np("utsuro-control");
        off_t offset = 0;
        {
            struct stat st;
            if (stat(file.c_str(), &st) == 0) offset = st.st_size;   // only commands appended from now on
        }
        for (;;) {
            std::this_thread::sleep_for(std::chrono::milliseconds(40));
            struct stat st;
            if (stat(file.c_str(), &st) != 0 || st.st_size <= offset) continue;
            FILE* f = std::fopen(file.c_str(), "r");
            if (!f) continue;
            std::fseek(f, offset, SEEK_SET);
            char line[512];
            while (std::fgets(line, sizeof line, f)) {
                offset = std::ftell(f);
                RunCommand(line);
            }
            std::fclose(f);
        }
    }).detach();
}

void KeyEvent(uint16_t key_code, bool down) {
    std::scoped_lock lock {g_mutex};
    if (key_code < 256) g_keys[key_code] = down;
}

void MouseMoved(float dx, float dy) {
    g_mouse_dx = g_mouse_dx + dx;
    g_mouse_dy = g_mouse_dy + dy;
}

void MouseButton(int button, bool down) {
    if (button >= 0 && button < 3) g_mouse_buttons[button] = down;
}

// ---- the control file ----------------------------------------------------------------------
struct Override { std::string name; std::chrono::steady_clock::time_point until; float value; };
std::mutex g_override_mutex;
std::vector<Override> g_overrides;

void ApplyOverrides(PadState& s) {
    std::scoped_lock lock {g_override_mutex};
    const auto now = std::chrono::steady_clock::now();
    g_overrides.erase(std::remove_if(g_overrides.begin(), g_overrides.end(), [&](const Override& o) { return o.until <= now; }), g_overrides.end());
    for (const Override& e : g_overrides) {
        const std::string& n = e.name;
        if (n == "A") s.buttons |= kA; else if (n == "B") s.buttons |= kB; else if (n == "X") s.buttons |= kX; else if (n == "Y") s.buttons |= kY;
        else if (n == "START") s.buttons |= kMenu; else if (n == "BACK") s.buttons |= kView;
        else if (n == "LB") s.buttons |= kLeftShoulder; else if (n == "RB") s.buttons |= kRightShoulder;
        else if (n == "UP") s.buttons |= kDPadUp; else if (n == "DOWN") s.buttons |= kDPadDown;
        else if (n == "LEFT") s.buttons |= kDPadLeft; else if (n == "RIGHT") s.buttons |= kDPadRight;
        else if (n == "LS") s.buttons |= kLeftThumb; else if (n == "RS") s.buttons |= kRightThumb;
        else if (n == "LX") s.left_x = e.value; else if (n == "LY") s.left_y = e.value;
        else if (n == "RX") s.right_x = e.value; else if (n == "RY") s.right_y = e.value;
        else if (n == "LT") s.left_trigger = e.value; else if (n == "RT") s.right_trigger = e.value;
    }
}

void RunCommand(const std::string& line) {
    std::stringstream ss(line);
    std::string cmd;
    ss >> cmd;
    const auto now = std::chrono::steady_clock::now();
    if (cmd == "press" || cmd == "stick") {
        std::string name;
        ss >> name;
        float value = 1.f;
        double ms = 90;
        if (cmd == "stick") ss >> value;
        ss >> ms;
        std::scoped_lock lock {g_override_mutex};
        g_overrides.push_back({name, now + std::chrono::microseconds((int64_t)(ms * 1000)), value});
    } else if (cmd == "hold") {
        std::string name;
        ss >> name;
        std::scoped_lock lock {g_override_mutex};
        g_overrides.push_back({name, now + std::chrono::hours(1), 1.f});
    } else if (cmd == "release") {
        std::string name;
        ss >> name;
        std::scoped_lock lock {g_override_mutex};
        g_overrides.erase(std::remove_if(g_overrides.begin(), g_overrides.end(), [&](const Override& o) { return o.name == name; }), g_overrides.end());
    } else if (cmd == "snap") {
        std::string path;
        ss >> path;
        if (!path.empty()) gpu::RequestSnapshot(path);
    } else if (cmd == "trace") {
        gpu::RequestTrace();
    } else if (cmd == "quit") {
        std::_Exit(0);
    }
}

PadState Poll() {
    PadState s;
    {
        std::scoped_lock lock {g_mutex};
        auto b = [&](Button flag, bool on) { if (on) s.buttons |= flag; };
        b(kA, Down(kReturn) || Down(kSpace));
        b(kB, Down(kBackspace) || Down(kEscape));
        b(kX, Down(kKeyX));
        b(kY, Down(kKeyY));
        b(kMenu, Down(kKeyP));
        b(kView, Down(kTab));
        b(kLeftShoulder, Down(kKeyQ));
        b(kRightShoulder, Down(kKeyE));
        b(kLeftThumb, Down(kShift));
        b(kRightThumb, Down(kKeyC));
        b(kDPadUp, Down(kUp));
        b(kDPadDown, Down(kDown));
        b(kDPadLeft, Down(kLeft));
        b(kDPadRight, Down(kRight));
        s.left_x = (Down(kKeyD) ? 1.f : 0.f) - (Down(kKeyA) ? 1.f : 0.f);
        s.left_y = (Down(kKeyW) ? 1.f : 0.f) - (Down(kKeyS) ? 1.f : 0.f);
        s.right_x = (Down(kKeyL) ? 1.f : 0.f) - (Down(kKeyJ) ? 1.f : 0.f);
        s.right_y = (Down(kKeyI) ? 1.f : 0.f) - (Down(kKeyK) ? 1.f : 0.f);
        s.left_trigger = Down(kKeyZ) ? 1.f : 0.f;
        s.right_trigger = Down(kKeyV) ? 1.f : 0.f;
    }
    if (g_mouse_buttons[0]) s.right_trigger = 1.f;
    if (g_mouse_buttons[1]) s.left_trigger = 1.f;
    // Mouse look: motion since the last poll becomes a right-stick push.
    const float mdx = g_mouse_dx.exchange(0), mdy = g_mouse_dy.exchange(0);
    if (mdx != 0 || mdy != 0) {
        s.right_x = std::clamp(s.right_x + mdx / 8.f, -1.f, 1.f);
        s.right_y = std::clamp(s.right_y - mdy / 8.f, -1.f, 1.f);
    }
    // A real controller overrides what the keyboard says where it is touched.
    @autoreleasepool {
        for (GCController* c in [GCController controllers]) {
            GCExtendedGamepad* p = c.extendedGamepad;
            if (p == nil) continue;
            auto on = [&](Button flag, bool v) { if (v) s.buttons |= flag; };
            on(kA, p.buttonA.pressed); on(kB, p.buttonB.pressed); on(kX, p.buttonX.pressed); on(kY, p.buttonY.pressed);
            on(kLeftShoulder, p.leftShoulder.pressed); on(kRightShoulder, p.rightShoulder.pressed);
            on(kDPadUp, p.dpad.up.pressed); on(kDPadDown, p.dpad.down.pressed); on(kDPadLeft, p.dpad.left.pressed); on(kDPadRight, p.dpad.right.pressed);
            on(kMenu, p.buttonMenu.pressed); on(kView, p.buttonOptions.pressed);
            on(kLeftThumb, p.leftThumbstickButton.pressed); on(kRightThumb, p.rightThumbstickButton.pressed);
            if (std::abs(p.leftThumbstick.xAxis.value) > 0.05f) s.left_x = p.leftThumbstick.xAxis.value;
            if (std::abs(p.leftThumbstick.yAxis.value) > 0.05f) s.left_y = p.leftThumbstick.yAxis.value;
            if (std::abs(p.rightThumbstick.xAxis.value) > 0.05f) s.right_x = p.rightThumbstick.xAxis.value;
            if (std::abs(p.rightThumbstick.yAxis.value) > 0.05f) s.right_y = p.rightThumbstick.yAxis.value;
            s.left_trigger = std::max(s.left_trigger, p.leftTrigger.value);
            s.right_trigger = std::max(s.right_trigger, p.rightTrigger.value);
        }
    }
    ApplyScript(s);
    ApplyOverrides(s);
    return s;
}

} // namespace utsuro::input
