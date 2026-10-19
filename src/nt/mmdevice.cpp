// mmdevice.cpp -- the audio endpoint enumerator (CLSID_MMDeviceEnumerator, with the console's IMMXboxDeviceEnumerator extras). The
// real MMDevAPI.dll opens a virtual audio bus device that does not exist here, and then the title's audio thread retries for ever; the
// host answers the class instead. There is one pretend output (HDMI, two channels) and nothing to enumerate behind it yet.
#include "nt/hle_classes.h"
#include "nt/winrt.h"

#include "engine/log.h"

namespace utsuro::nt::mmdevice {

namespace {

using winrt::Call;
using winrt::Guid;
using winrt::Iface;
using winrt::Method;
using winrt::Obj;
using winrt::Runtime;

constexpr uint32_t kSOk = 0, kENoInterface = 0x80004002u, kENotFound = 0x80070490u, kEClassNotAvailable = 0x80040111u;

const Guid& ClsidEnumerator() { static const Guid g = Guid::Parse("BCDE0395-E52F-467C-8E3D-C4579291692E"); return g; }

Method M(const char* name, winrt::HostFn fn) {
    Method m;
    m.name = name;
    m.fn = std::move(fn);
    return m;
}

Iface* g_enumerator = nullptr;
Iface* g_factory = nullptr;

void Define(Runtime& rt) {
    if (g_enumerator != nullptr) return;
    static const char* const names[] = {"EnumAudioEndpoints", "GetDefaultAudioEndpoint", "GetDevice", "RegisterEndpointNotificationCallback",
                                        "UnregisterEndpointNotificationCallback", "GetHdAudioChannelCounts",
                                        "RegisterChannelCountNotificationCallback", "UnregisterChannelCountNotificationCallback"};
    std::vector<Method> methods;
    for (const char* n : names) {
        const std::string name = n;
        methods.push_back(M(n, [name](Call& c) -> uint32_t {
            if (name == "GetHdAudioChannelCounts") {
                if (auto* hdmi = c.Ptr<uint32_t>(0)) *hdmi = 2;
                if (auto* spdif = c.Ptr<uint32_t>(1)) *spdif = 2;
                return kSOk;
            }
            if (name == "EnumAudioEndpoints" || name == "GetDefaultAudioEndpoint" || name == "GetDevice") {
                UT_INFO("mmdevice: %s asked for; there are no endpoints", name.c_str());
                const int out = name == "EnumAudioEndpoints" ? 2 : name == "GetDefaultAudioEndpoint" ? 2 : 1;
                if (auto* p = c.Ptr<uint64_t>(out)) *p = 0;
                return kENotFound;
            }
            return kSOk;   // (Un)RegisterEndpointNotificationCallback and the channel-count ones
        }));
    }
    g_enumerator = &rt.DefineComInterface("IMMXboxDeviceEnumerator", "8B557ADC-555E-47A0-B223-43477E481DAD", std::move(methods), nullptr);
    g_enumerator->aliases.push_back(Guid::Parse("A95664D2-9614-4F35-A746-DE8DB63617E6"));   // IMMDeviceEnumerator: a prefix of the vtable above

    std::vector<Method> factory;
    factory.push_back(M("CreateInstance", [](Call& c) -> uint32_t {
        // CreateInstance(pUnkOuter, riid, ppv)
        const Guid iid = *c.Ptr<Guid>(1);
        auto* out = c.Ptr<uint64_t>(2);
        if (out == nullptr) return 0x80004003u;
        Runtime& r = Runtime::Get();
        static const Guid unknown = Guid::Parse("00000000-0000-0000-C000-000000000046");
        if (!(iid == unknown) && !(iid == g_enumerator->iid) && std::find(g_enumerator->aliases.begin(), g_enumerator->aliases.end(), iid) == g_enumerator->aliases.end()) {
            *out = 0;
            return kENoInterface;
        }
        Obj* o = r.NewObject("MMDevAPI.MMDeviceEnumerator", {g_enumerator});
        *out = r.Pointer(*o, *g_enumerator);
        return kSOk;
    }));
    factory.push_back(M("LockServer", [](Call&) -> uint32_t { return kSOk; }));
    g_factory = &rt.DefineComInterface("IClassFactory", "00000001-0000-0000-C000-000000000046", std::move(factory), nullptr);
}

} // namespace

// mmdevapi!DllGetClassObject(REFCLSID, REFIID, void**)
uint64_t DllGetClassObject(Kernel&, NtThread&, SyscallArgs& a) {
    Runtime& r = Runtime::Get();
    Define(r);
    const auto* clsid = a.Ptr<Guid>(0);
    auto* out = a.Ptr<uint64_t>(2);
    if (out == nullptr) return 0x80004003u;
    if (clsid == nullptr || !(*clsid == ClsidEnumerator())) { *out = 0; return kEClassNotAvailable; }
    Obj* o = r.NewObject("MMDevAPI.ClassFactory", {g_factory});
    *out = r.Pointer(*o, *g_factory);
    return kSOk;
}

} // namespace utsuro::nt::mmdevice
