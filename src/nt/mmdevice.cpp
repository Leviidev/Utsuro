// mmdevice.cpp -- the audio endpoint enumerator (CLSID_MMDeviceEnumerator, with the console's IMMXboxDeviceEnumerator extras). The
// real MMDevAPI.dll opens a virtual audio bus device that does not exist here, and then the title's audio thread retries for ever; the
// host answers the class instead. There is one pretend output (HDMI, two channels) and nothing to enumerate behind it yet.
#include "nt/hle_classes.h"
#include "nt/winrt.h"
#include "nt/pe.h"
#include "nt/kernel.h"

#include "engine/log.h"

#include <cstring>

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
Iface* g_device = nullptr;

/// The one pretend output endpoint (IMMDevice): an id and an active state; Activate and property stores are not offered.
uint64_t EndpointPointer() {
    Runtime& r = Runtime::Get();
    Obj* o = r.NewObject("MMDevAPI.MMDevice", {g_device});
    return r.Pointer(*o, *g_device);
}

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
            if (name == "GetDefaultAudioEndpoint") {   // (dataFlow, role, ppEndpoint): the one output, for rendering
                const uint32_t flow = static_cast<uint32_t>(c.Arg(0));
                if (flow == 0 /* eRender */ && c.Ptr<uint64_t>(2) != nullptr) { *c.Ptr<uint64_t>(2) = EndpointPointer(); return kSOk; }
            }
            if (name == "GetDevice" && c.Ptr<uint64_t>(1) != nullptr) { *c.Ptr<uint64_t>(1) = EndpointPointer(); return kSOk; }   // (id, ppDevice): the one output, whatever the id
            if (name == "EnumAudioEndpoints" || name == "GetDefaultAudioEndpoint" || name == "GetDevice") {
                UT_INFO("mmdevice: %s asked for; there are no endpoints", name.c_str());
                const int out = name == "EnumAudioEndpoints" ? 2 : name == "GetDefaultAudioEndpoint" ? 2 : 1;
                if (auto* p = c.Ptr<uint64_t>(out)) *p = 0;
                return kENotFound;
            }
            return kSOk;   // (Un)RegisterEndpointNotificationCallback and the channel-count ones
        }));
    }
    {
        std::vector<Method> device;
        device.push_back(M("Activate", [](Call& c) -> uint32_t {
            // Activate(iid, clsctx, params, ppv)
            if (c.Ptr<Guid>(0) != nullptr) UT_INFO("mmdevice: Activate asked for %s", c.Ptr<Guid>(0)->ToString().c_str());
            if (auto* p = c.Ptr<uint64_t>(3)) *p = 0;
            return kENoInterface;
        }));
        device.push_back(M("OpenPropertyStore", [](Call& c) -> uint32_t { if (auto* p = c.Ptr<uint64_t>(1)) *p = 0; return 0x80004001u; }));
        device.push_back(M("GetId", [](Call& c) -> uint32_t {
            // The caller frees the string with CoTaskMemFree, so it comes from the process heap, allocated by the guest's own RtlAllocateHeap.
            static const char16_t id[] = u"{0.0.0.00000000}.{utsuro-render}";
            auto* out = c.Ptr<uint64_t>(0);
            if (out == nullptr) return 0x80004003u;
            Kernel& k = c.kernel;
            const uint64_t heap = *reinterpret_cast<const uint64_t*>(k.PebAddress() + 0x30);
            const uint64_t alloc = FindExport(k.NtdllBase(), "RtlAllocateHeap");
            c.thread.CallGuest(GuestStep::Call(alloc, {heap, 0, sizeof id}, [out](NtThread&, uint64_t text) -> GuestStep {
                if (text != 0) std::memcpy(reinterpret_cast<void*>(text), id, sizeof id);
                *out = text;
                return GuestStep::Return(text != 0 ? 0 : 0x8007000Eu);
            }));
            return kSOk;
        }));
        device.push_back(M("GetState", [](Call& c) -> uint32_t { if (auto* p = c.Ptr<uint32_t>(0)) *p = 1; return kSOk; }));   // DEVICE_STATE_ACTIVE
        g_device = &rt.DefineComInterface("IMMDevice", "D666063F-1587-4E43-81F1-B948E807363F", std::move(device), nullptr);
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
