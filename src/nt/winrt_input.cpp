// winrt_input.cpp -- Windows.Xbox.Input: the one pad the host presents, fed by host/input.
#include "nt/winrt.h"

#include "host/input.h"

#include "engine/log.h"

#include <chrono>
#include <cstring>

namespace utsuro::nt::winrt {

namespace {

uint64_t NowMicros() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

Obj* g_pad = nullptr;

uint64_t PadPointer(const char* iface_name) {
    Runtime& rt = Runtime::Get();
    Iface* i = rt.FindInterface(iface_name);
    ++g_pad->refs;
    return rt.Pointer(*g_pad, *i);
}

uint32_t Bits(float f) { uint32_t b; std::memcpy(&b, &f, 4); return b; }
float FromBits(uint64_t b) { float f; uint32_t v = static_cast<uint32_t>(b); std::memcpy(&f, &v, 4); return f; }

} // namespace

void RegisterInput(Runtime& rt) {
    auto fn = [](const char* name, HostFn f) { Method m; m.name = name; m.fn = std::move(f); return m; };
    Iface& controller = rt.DefineInterface("IController", "9776ff56-9bfd-4581-ad45-b645bba526d6", {
        fn("get_Id", [](Call& c) -> uint32_t { *c.Ptr<uint64_t>(0) = 1; return 0; }),
        fn("get_Type", [](Call& c) -> uint32_t { *c.Ptr<uint64_t>(0) = Runtime::Get().MakeHString(u"Windows.Xbox.Input.Gamepad"); return 0; }),
        fn("get_User", [](Call& c) -> uint32_t { *c.Ptr<uint64_t>(0) = CurrentUserPointer(); return 0; }),
    });
    Iface& navigation = rt.DefineInterface("INavigationController", "AACAAAAB-AAAA-AAAA-AAAA-CDAAAAAACAAA", {
        fn("GetNavigationReading", [](Call& c) -> uint32_t { *c.Ptr<uint64_t>(0) = 0; return 0; }),
        fn("GetRawNavigationReading", [](Call& c) -> uint32_t { std::memset(c.Ptr<void>(0), 0, 16); return 0; }),
    }, true);

    Iface* gamepad = rt.FindInterface("IGamepad");
    if (gamepad == nullptr) { UT_ERROR("winrt: IGamepad missing"); return; }
    g_pad = rt.NewObject("Windows.Xbox.Input.Gamepad", {gamepad, &controller, &navigation});

    rt.Override("IGamepad", "GetRawCurrentReading", [](Call& c) -> uint32_t {
        const input::PadState s = input::Poll();
        auto* out = c.Ptr<uint8_t>(0);
        std::memset(out, 0, 40);
        const uint64_t t = NowMicros();
        std::memcpy(out, &t, 8);
        std::memcpy(out + 8, &s.buttons, 4);
        std::memcpy(out + 12, &s.left_trigger, 4);
        std::memcpy(out + 16, &s.right_trigger, 4);
        std::memcpy(out + 20, &s.left_x, 4);
        std::memcpy(out + 24, &s.left_y, 4);
        std::memcpy(out + 28, &s.right_x, 4);
        std::memcpy(out + 32, &s.right_y, 4);
        return 0;
    });
    rt.Override("IGamepad", "GetCurrentReading", [](Call& c) -> uint32_t {
        Runtime& r = Runtime::Get();
        Iface* reading = r.FindInterface("IGamepadReading");
        Obj* o = r.NewObject("Windows.Xbox.Input.GamepadReading", {reading});
        const input::PadState s = input::Poll();
        o->u64["buttons"] = s.buttons;
        o->u64["lt"] = Bits(s.left_trigger);
        o->u64["rt"] = Bits(s.right_trigger);
        o->u64["lx"] = Bits(s.left_x);
        o->u64["ly"] = Bits(s.left_y);
        o->u64["rx"] = Bits(s.right_x);
        o->u64["ry"] = Bits(s.right_y);
        o->u64["time"] = NowMicros();
        *c.Ptr<uint64_t>(0) = r.Pointer(*o, *reading);
        return 0;
    });
    auto flag = [&rt](const char* method, uint32_t mask) {
        rt.Override("IGamepadReading", method, [mask](Call& c) -> uint32_t { *c.Ptr<uint8_t>(0) = (c.obj.u64["buttons"] & mask) != 0; return 0; });
    };
    flag("get_IsDPadUpPressed", input::kDPadUp); flag("get_IsDPadDownPressed", input::kDPadDown);
    flag("get_IsDPadLeftPressed", input::kDPadLeft); flag("get_IsDPadRightPressed", input::kDPadRight);
    flag("get_IsMenuPressed", input::kMenu); flag("get_IsViewPressed", input::kView);
    flag("get_IsLeftThumbstickPressed", input::kLeftThumb); flag("get_IsRightThumbstickPressed", input::kRightThumb);
    flag("get_IsLeftShoulderPressed", input::kLeftShoulder); flag("get_IsRightShoulderPressed", input::kRightShoulder);
    flag("get_IsAPressed", input::kA); flag("get_IsBPressed", input::kB); flag("get_IsXPressed", input::kX); flag("get_IsYPressed", input::kY);
    rt.Override("IGamepadReading", "get_Buttons", [](Call& c) -> uint32_t { *c.Ptr<uint32_t>(0) = static_cast<uint32_t>(c.obj.u64["buttons"]); return 0; });
    rt.Override("IGamepadReading", "get_Timestamp", [](Call& c) -> uint32_t { *c.Ptr<uint64_t>(0) = c.obj.u64["time"]; return 0; });
    auto axis = [&rt](const char* method, const char* key) {
        std::string k = key;
        rt.Override("IGamepadReading", method, [k](Call& c) -> uint32_t { *c.Ptr<float>(0) = FromBits(c.obj.u64[k]); return 0; });
    };
    axis("get_LeftTrigger", "lt"); axis("get_RightTrigger", "rt");
    axis("get_LeftThumbstickX", "lx"); axis("get_LeftThumbstickY", "ly");
    axis("get_RightThumbstickX", "rx"); axis("get_RightThumbstickY", "ry");

    // The pad list and the controller list both hold the one pad.
    rt.Override("IGamepadStatics", "get_Gamepads", [](Call& c) -> uint32_t {
        *c.Ptr<uint64_t>(0) = Runtime::Get().NewVectorView({PadPointer("IGamepad")});
        return 0;
    });
    rt.OverrideAllNamed("IControllerStatics", "get_Controllers", [](Call& c) -> uint32_t {
        *c.Ptr<uint64_t>(0) = Runtime::Get().NewVectorView({PadPointer("IController")});
        return 0;
    });
    rt.OverrideAllNamed("IControllerStatics", "GetControllersOrderedLeftToRight", [](Call& c) -> uint32_t {
        *c.Ptr<uint64_t>(1) = Runtime::Get().NewVectorView({PadPointer("IController")});
        return 0;
    });
    rt.OverrideAllNamed("IControllerStatics", "GetControllerById", [](Call& c) -> uint32_t {
        *c.Ptr<uint64_t>(1) = c.Arg(0) == 1 ? PadPointer("IController") : 0;
        return 0;
    });
}

} // namespace utsuro::nt::winrt
