// mmdevice.cpp -- the audio endpoint enumerator (CLSID_MMDeviceEnumerator, with the console's IMMXboxDeviceEnumerator extras). The
// real MMDevAPI.dll opens a virtual audio bus device that does not exist here, and then the title's audio thread retries for ever; the
// host answers the class instead. There is one pretend output (HDMI, two channels) and nothing to enumerate behind it yet.
#include "nt/hle_classes.h"
#include "nt/winrt.h"
#include "nt/pe.h"
#include "nt/kernel.h"

#include "engine/log.h"
#include "host/audio.h"

#include <atomic>
#include <chrono>
#include <cstring>
#include <map>
#include <memory>
#include <functional>
#include <algorithm>
#include <mutex>
#include <thread>

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
Iface* g_collection = nullptr;

Iface* g_audio_client = nullptr;
Iface* g_render_client = nullptr;

// ---- WASAPI shared-mode output (IAudioClient + IAudioRenderClient) -------------------------------------------------------
// Titles that drive the speakers themselves (Wwise in Rocket League, Peggle 2) ask the endpoint for an IAudioClient. The
// host plays what they write: frames go to the CoreAudio ring as they are released; a host thread drains the "device
// buffer" at the real rate and signals the title's event every period, which is what WASAPI's event mode does.
constexpr uint32_t kRate = audio::kRate;
constexpr uint32_t kPeriodFrames = kRate / 100;   // 10 ms

struct Stream {
    std::mutex mutex;
    uint32_t channels {2}, bits {32};
    bool is_float {true};
    uint32_t buffer_frames {kRate / 10};
    uint64_t buffer {0};          // guest memory GetBuffer hands out
    double queued {0};            // frames written and not yet "played"
    uint32_t pending_get {0};     // frames asked for by the last GetBuffer
    uint64_t event {0};           // the title's event handle (event mode)
    bool started {false}, stop {false};
    std::thread clock;
};

std::mutex g_streams_mutex;
std::map<uint64_t, std::shared_ptr<Stream>> g_streams;   // client object id -> stream

std::shared_ptr<Stream> StreamOf(Obj& o) {
    std::scoped_lock lock {g_streams_mutex};
    auto& s = g_streams[o.id];
    if (!s) s = std::make_shared<Stream>();
    return s;
}

void SignalEvent(Kernel& k, uint64_t handle) {
    if (handle == 0) return;
    auto e = k.Get<EventObject>(handle, ObjType::Event);
    if (!e) return;
    std::scoped_lock lock {k.WaitMutex()};
    e->signaled = true;
    k.NotifyAll();
}

/// The device clock: drains `queued` at the output rate and wakes the title every period.
void RunClock(Kernel* k, std::shared_ptr<Stream> st) {
    auto last = std::chrono::steady_clock::now();
    for (;;) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        uint64_t event;
        {
            std::scoped_lock lock {st->mutex};
            if (st->stop || k->exiting) return;
            const auto now = std::chrono::steady_clock::now();
            const double played = std::chrono::duration<double>(now - last).count() * kRate;
            last = now;
            if (!st->started) continue;
            st->queued = std::max(0.0, st->queued - played);
            event = st->event;
        }
        SignalEvent(*k, event);
    }
}

/// Writes `frames` of the title's format to the host output as stereo float.
void Play(const Stream& st, const uint8_t* data, uint32_t frames, bool silent) {
    static thread_local std::vector<float> out;
    out.assign(static_cast<size_t>(frames) * 2, 0.f);
    if (!silent) {
        for (uint32_t i = 0; i < frames; ++i) {
            float ch[8] = {};
            for (uint32_t c = 0; c < st.channels && c < 8; ++c) {
                const size_t at = (static_cast<size_t>(i) * st.channels + c) * (st.bits / 8);
                if (st.is_float && st.bits == 32) { float f; std::memcpy(&f, data + at, 4); ch[c] = f; }
                else if (st.bits == 16) { int16_t v; std::memcpy(&v, data + at, 2); ch[c] = v / 32768.f; }
                else if (st.bits == 32) { int32_t v; std::memcpy(&v, data + at, 4); ch[c] = v / 2147483648.f; }
            }
            if (st.channels == 1) { out[i * 2] = out[i * 2 + 1] = ch[0]; continue; }
            float l = ch[0], r = ch[1];
            if (st.channels >= 6) {   // 5.1/7.1 down to stereo: centre and surrounds at -3 dB, LFE dropped
                const float c = ch[2] * 0.707f;
                l += c + ch[4] * 0.707f + (st.channels >= 8 ? ch[6] * 0.707f : 0.f);
                r += c + ch[5] * 0.707f + (st.channels >= 8 ? ch[7] * 0.707f : 0.f);
            }
            out[i * 2] = std::clamp(l, -1.f, 1.f);
            out[i * 2 + 1] = std::clamp(r, -1.f, 1.f);
        }
    }
    static bool up = audio::Init(); (void)up;
    audio::Write(out.data(), frames);
}

/// GetMixFormat's answer: WAVEFORMATEXTENSIBLE, 48 kHz stereo float, in memory the caller frees with CoTaskMemFree.
void WriteMixFormat(uint8_t* w) {
    std::memset(w, 0, 40);
    const uint16_t tag = 0xFFFE, channels = 2, block = 8, bits = 32, extra = 22;
    const uint32_t rate = kRate, bytes = kRate * 8, mask = 3;
    std::memcpy(w + 0, &tag, 2); std::memcpy(w + 2, &channels, 2); std::memcpy(w + 4, &rate, 4); std::memcpy(w + 8, &bytes, 4);
    std::memcpy(w + 12, &block, 2); std::memcpy(w + 14, &bits, 2); std::memcpy(w + 16, &extra, 2);
    std::memcpy(w + 18, &bits, 2);   // wValidBitsPerSample
    std::memcpy(w + 20, &mask, 4);   // dwChannelMask: front left, front right
    static const uint8_t ieee_float[16] = {0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71};
    std::memcpy(w + 24, ieee_float, 16);
}

/// Allocates guest memory the title will free with CoTaskMemFree, fills it, stores the pointer at `out`.
uint32_t GuestCoTaskAlloc(Call& c, uint64_t* out, size_t bytes, std::function<void(uint8_t*)> fill) {
    Kernel& k = c.kernel;
    const uint64_t combase = k.FindLoadedImage("combase.dll");
    const uint64_t co_alloc = combase ? FindExport(combase, "CoTaskMemAlloc") : 0;
    auto done = [out, fill](NtThread&, uint64_t mem) -> GuestStep {
        if (mem != 0) fill(reinterpret_cast<uint8_t*>(mem));
        *out = mem;
        return GuestStep::Return(mem != 0 ? 0 : 0x8007000Eu);
    };
    if (co_alloc != 0) {
        c.thread.CallGuest(GuestStep::Call(co_alloc, {bytes}, done));
    } else {
        const uint64_t heap = *reinterpret_cast<const uint64_t*>(k.PebAddress() + 0x30);
        c.thread.CallGuest(GuestStep::Call(FindExport(k.NtdllBase(), "RtlAllocateHeap"), {heap, 0, bytes}, done));
    }
    return kSOk;
}

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
            if (name == "GetDevice" && c.Ptr<uint64_t>(1) != nullptr) { *c.Ptr<uint64_t>(1) = EndpointPointer(); return kSOk; }
            if (name == "EnumAudioEndpoints" && c.Ptr<uint64_t>(2) != nullptr) {
                // (dataFlow, stateMask, ppDevices): the one output for eRender (0) and eAll (2); capture has none. Wwise polls this
                // and shuts its XAudio2 device down while it is empty.
                const uint32_t flow = static_cast<uint32_t>(c.Arg(0));
                Runtime& r = Runtime::Get();
                Obj* list = r.NewObject("MMDevAPI.MMDeviceCollection", {g_collection});
                list->u64["count"] = (flow == 0 || flow == 2) ? 1 : 0;
                *c.Ptr<uint64_t>(2) = r.Pointer(*list, *g_collection);
                static std::atomic<int> shown {0};
                if (shown++ < 3) UT_INFO("mmdevice: EnumAudioEndpoints(flow %u) -> %llu endpoint(s)", flow, (unsigned long long)list->u64["count"]);
                return kSOk;
            }   // (id, ppDevice): the one output, whatever the id
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
            auto* p = c.Ptr<uint64_t>(3);
            if (c.Ptr<Guid>(0) != nullptr && p != nullptr) {
                const Guid iid = *c.Ptr<Guid>(0);
                if (iid == g_audio_client->iid || std::find(g_audio_client->aliases.begin(), g_audio_client->aliases.end(), iid) != g_audio_client->aliases.end()) {
                    Runtime& r = Runtime::Get();
                    Obj* o = r.NewObject("MMDevAPI.AudioClient", {g_audio_client});
                    *p = r.Pointer(*o, *g_audio_client);
                    static std::atomic<int> shown {0};
                    if (shown++ < 3) UT_INFO("mmdevice: Activate -> IAudioClient %p", reinterpret_cast<void*>(*p));
                    return kSOk;
                }
                UT_INFO("mmdevice: Activate asked for %s", iid.ToString().c_str());
            }
            if (p) *p = 0;
            return kENoInterface;
        }));
        device.push_back(M("OpenPropertyStore", [](Call& c) -> uint32_t { if (auto* p = c.Ptr<uint64_t>(1)) *p = 0; return 0x80004001u; }));
        device.push_back(M("GetId", [](Call& c) -> uint32_t {
            // The caller frees the string with CoTaskMemFree. If combase.dll is loaded, call CoTaskMemAlloc.
            static const char16_t id[] = u"{0.0.0.00000000}.{utsuro-render}";
            auto* out = c.Ptr<uint64_t>(0);
            if (out == nullptr) return 0x80004003u;
            Kernel& k = c.kernel;
            const uint64_t combase = k.FindLoadedImage("combase.dll");
            const uint64_t co_alloc = combase ? FindExport(combase, "CoTaskMemAlloc") : 0;
            if (co_alloc != 0) {
                c.thread.CallGuest(GuestStep::Call(co_alloc, {sizeof id}, [out](NtThread&, uint64_t text) -> GuestStep {
                    if (text != 0) std::memcpy(reinterpret_cast<void*>(text), id, sizeof id);
                    *out = text;
                    return GuestStep::Return(text != 0 ? 0 : 0x8007000Eu);
                }));
            } else {
                const uint64_t heap = *reinterpret_cast<const uint64_t*>(k.PebAddress() + 0x30);
                const uint64_t alloc = FindExport(k.NtdllBase(), "RtlAllocateHeap");
                c.thread.CallGuest(GuestStep::Call(alloc, {heap, 0, sizeof id}, [out](NtThread&, uint64_t text) -> GuestStep {
                    if (text != 0) std::memcpy(reinterpret_cast<void*>(text), id, sizeof id);
                    *out = text;
                    return GuestStep::Return(text != 0 ? 0 : 0x8007000Eu);
                }));
            }
            return kSOk;
        }));
        device.push_back(M("GetState", [](Call& c) -> uint32_t { if (auto* p = c.Ptr<uint32_t>(0)) *p = 1; return kSOk; }));   // DEVICE_STATE_ACTIVE
        g_device = &rt.DefineComInterface("IMMDevice", "D666063F-1587-4E43-81F1-B948E807363F", std::move(device), nullptr);
    }
    {
        std::vector<Method> render;
        // GetBuffer(NumFramesRequested, ppData)
        render.push_back(M("GetBuffer", [](Call& c) -> uint32_t {
            auto st = StreamOf(c.obj);
            Obj* client = Runtime::Get().FromPointer(c.obj.u64["client"]);
            auto cs = client ? StreamOf(*client) : st;
            std::scoped_lock lock {cs->mutex};
            const uint32_t want = static_cast<uint32_t>(c.Arg(0));
            if (want > cs->buffer_frames - static_cast<uint32_t>(cs->queued)) return 0x88890006u;   // AUDCLNT_E_BUFFER_TOO_LARGE
            cs->pending_get = want;
            if (auto* out = c.Ptr<uint64_t>(1)) *out = cs->buffer;
            return kSOk;
        }));
        // ReleaseBuffer(NumFramesWritten, dwFlags)
        render.push_back(M("ReleaseBuffer", [](Call& c) -> uint32_t {
            Obj* client = Runtime::Get().FromPointer(c.obj.u64["client"]);
            if (client == nullptr) return 0x80004005u;
            auto cs = StreamOf(*client);
            std::scoped_lock lock {cs->mutex};
            const uint32_t frames = std::min<uint32_t>(static_cast<uint32_t>(c.Arg(0)), cs->pending_get);
            Play(*cs, reinterpret_cast<const uint8_t*>(cs->buffer), frames, (c.Arg(1) & 2) != 0);   // AUDCLNT_BUFFERFLAGS_SILENT
            cs->queued += frames;
            cs->pending_get = 0;
            return kSOk;
        }));
        g_render_client = &rt.DefineComInterface("IAudioRenderClient", "F294ACFC-3146-4483-A7BF-ADDCA7C260E2", std::move(render), nullptr);

        std::vector<Method> client;
        // Initialize(ShareMode, StreamFlags, hnsBufferDuration, hnsPeriodicity, pFormat, AudioSessionGuid)
        client.push_back(M("Initialize", [](Call& c) -> uint32_t {
            auto st = StreamOf(c.obj);
            std::scoped_lock lock {st->mutex};
            const auto* f = c.Ptr<uint8_t>(4);
            if (f != nullptr) {
                uint16_t tag, channels, bits; uint32_t rate;
                std::memcpy(&tag, f, 2); std::memcpy(&channels, f + 2, 2); std::memcpy(&rate, f + 4, 4); std::memcpy(&bits, f + 14, 2);
                st->channels = std::max<uint16_t>(1, channels);
                st->bits = bits;
                st->is_float = tag == 3 || (tag == 0xFFFE && f[24] == 3);
                UT_INFO("mmdevice: IAudioClient::Initialize(mode %llu, flags %#llx, %llu ms): %u ch, %u Hz, %u-bit %s", (unsigned long long)c.Arg(0), (unsigned long long)c.Arg(1),
                        (unsigned long long)(c.Arg(2) / 10000), channels, rate, bits, st->is_float ? "float" : "int");
                if (rate != kRate) UT_WARN("mmdevice: the title asked for %u Hz; output is %u Hz and is not resampled", rate, kRate);
            }
            const uint64_t duration = c.Arg(2);
            st->buffer_frames = std::max<uint32_t>(kPeriodFrames * 2, static_cast<uint32_t>(duration * kRate / 10000000ull));
            st->buffer = Runtime::Get().AllocGuest(static_cast<size_t>(st->buffer_frames) * st->channels * std::max<uint32_t>(2, st->bits / 8), 64);
            if (!st->clock.joinable()) st->clock = std::thread(RunClock, &c.kernel, st);
            return kSOk;
        }));
        client.push_back(M("GetBufferSize", [](Call& c) -> uint32_t { if (auto* p = c.Ptr<uint32_t>(0)) *p = StreamOf(c.obj)->buffer_frames; return kSOk; }));
        client.push_back(M("GetStreamLatency", [](Call& c) -> uint32_t { if (auto* p = c.Ptr<uint64_t>(0)) *p = 100000; return kSOk; }));   // 10 ms
        client.push_back(M("GetCurrentPadding", [](Call& c) -> uint32_t {
            auto st = StreamOf(c.obj);
            std::scoped_lock lock {st->mutex};
            if (auto* p = c.Ptr<uint32_t>(0)) *p = static_cast<uint32_t>(st->queued);
            return kSOk;
        }));
        client.push_back(M("IsFormatSupported", [](Call& c) -> uint32_t { if (auto* p = c.Ptr<uint64_t>(2)) *p = 0; return kSOk; }));
        client.push_back(M("GetMixFormat", [](Call& c) -> uint32_t {
            auto* out = c.Ptr<uint64_t>(0);
            if (out == nullptr) return 0x80004003u;
            return GuestCoTaskAlloc(c, out, 40, WriteMixFormat);
        }));
        client.push_back(M("GetDevicePeriod", [](Call& c) -> uint32_t {
            if (auto* p = c.Ptr<uint64_t>(0)) *p = 100000;   // 10 ms
            if (auto* p = c.Ptr<uint64_t>(1)) *p = 30000;    // 3 ms
            return kSOk;
        }));
        client.push_back(M("Start", [](Call& c) -> uint32_t { auto st = StreamOf(c.obj); std::scoped_lock lock {st->mutex}; st->started = true; return kSOk; }));
        client.push_back(M("Stop", [](Call& c) -> uint32_t { auto st = StreamOf(c.obj); std::scoped_lock lock {st->mutex}; st->started = false; return kSOk; }));
        client.push_back(M("Reset", [](Call& c) -> uint32_t { auto st = StreamOf(c.obj); std::scoped_lock lock {st->mutex}; st->queued = 0; return kSOk; }));
        client.push_back(M("SetEventHandle", [](Call& c) -> uint32_t { auto st = StreamOf(c.obj); std::scoped_lock lock {st->mutex}; st->event = c.Arg(0); return kSOk; }));
        // GetService(riid, ppv)
        client.push_back(M("GetService", [](Call& c) -> uint32_t {
            auto* out = c.Ptr<uint64_t>(1);
            if (out == nullptr || c.Ptr<Guid>(0) == nullptr) return 0x80004003u;
            const Guid iid = *c.Ptr<Guid>(0);
            if (iid == g_render_client->iid) {
                Runtime& r = Runtime::Get();
                Obj* o = r.NewObject("MMDevAPI.AudioRenderClient", {g_render_client});
                o->u64["client"] = r.Pointer(c.obj, c.iface);
                *out = r.Pointer(*o, *g_render_client);
                return kSOk;
            }
            UT_INFO("mmdevice: IAudioClient::GetService asked for %s", iid.ToString().c_str());
            *out = 0;
            return kENoInterface;
        }));
        // IAudioClient2 / Xbox extensions: answered quietly.
        client.push_back(M("IsOffloadCapable", [](Call& c) -> uint32_t { if (auto* p = c.Ptr<uint32_t>(1)) *p = 0; return kSOk; }));
        client.push_back(M("SetClientProperties", [](Call&) -> uint32_t { return kSOk; }));
        client.push_back(M("GetBufferSizeLimits", [](Call& c) -> uint32_t {
            if (auto* p = c.Ptr<uint64_t>(2)) *p = 30000;
            if (auto* p = c.Ptr<uint64_t>(3)) *p = 20000000;
            return kSOk;
        }));
        g_audio_client = &rt.DefineComInterface("IAudioClient", "1CB9AD4C-DBFA-4C32-B178-C2F568A703B2", std::move(client), nullptr);
        g_audio_client->aliases.push_back(Guid::Parse("726778CD-F60A-4EDA-82DE-E47610CD78AA"));   // IAudioClient2
    }
    {
        std::vector<Method> collection;
        collection.push_back(M("GetCount", [](Call& c) -> uint32_t { if (auto* p = c.Ptr<uint32_t>(0)) *p = static_cast<uint32_t>(c.obj.u64["count"]); return kSOk; }));
        collection.push_back(M("Item", [](Call& c) -> uint32_t {
            auto* out = c.Ptr<uint64_t>(1);
            if (out == nullptr) return 0x80004003u;
            if (c.Arg(0) >= c.obj.u64["count"]) { *out = 0; return 0x80070057u; }
            *out = EndpointPointer();
            return kSOk;
        }));
        g_collection = &rt.DefineComInterface("IMMDeviceCollection", "0BD7A1BE-7A1A-44DB-8397-CC5392387B5E", std::move(collection), nullptr);
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
