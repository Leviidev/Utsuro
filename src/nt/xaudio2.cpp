// xaudio2.cpp -- the host's XAudio2. The Era's xaudio2_9.dll talks to the console's audio
// co-processor; the title (through Miles) only needs the interface, so CreateXAudio2Object is
// replaced by a host object. Source voices keep their queued buffers and "play" them at the
// right rate on a pump thread, which also delivers the voice callbacks the title waits for.
#include "nt/xaudio2.h"

#include "nt/winrt.h"

#include "engine/log.h"

#include <chrono>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <thread>

namespace utsuro::nt::xaudio2 {

namespace {

using winrt::Call;
using winrt::Iface;
using winrt::Obj;
using winrt::Runtime;

struct Buffer {
    uint64_t data {0};
    uint32_t bytes {0}, flags {0};
    uint64_t context {0};
    uint32_t play_begin {0}, play_length {0}, loop_begin {0}, loop_length {0}, loop_count {0};
    double played {0};        ///< Seconds of this buffer already consumed.
    bool started {false};
};

struct Voice {
    bool source {false};
    bool started {false};
    uint32_t channels {2}, rate {48000}, avg_bytes_per_sec {48000 * 4};
    float ratio {1.0f}, volume {1.0f};
    uint64_t callback {0};
    std::deque<Buffer> queue;
    uint64_t samples_played {0};
    uint64_t pointer {0};     ///< The guest pointer of the voice, for callbacks that need it.
    bool destroyed {false};
};

std::mutex g_mutex;
std::map<uint64_t, Voice> g_voices;    // by object id
Iface* g_engine = nullptr;
Iface* g_source = nullptr;
Iface* g_master = nullptr;
Iface* g_submix = nullptr;
Iface* g_pump = nullptr;
bool g_pump_started = false;
uint64_t g_pump_object = 0;

/// A callback the pump owes the title: delegate slot (IXAudio2VoiceCallback) and its arguments.
struct Pending { uint64_t callback; int slot; uint64_t self; uint64_t arg; };

uint64_t Vtable(uint64_t object, int slot) {
    return reinterpret_cast<uint64_t*>(*reinterpret_cast<uint64_t*>(object))[slot];
}

Voice& VoiceOf(Obj& o) { return g_voices[o.id]; }

winrt::Method M(const char* name, winrt::HostFn fn = nullptr) {
    winrt::Method m;
    m.name = name;
    m.fn = std::move(fn);
    return m;
}

/// The IXAudio2Voice methods every voice type starts with (no IUnknown: voices are bare objects).
std::vector<winrt::Method> VoiceMethods() {
    std::vector<winrt::Method> v;
    v.push_back(M("GetVoiceDetails", [](Call& c) -> uint32_t {
        auto& voice = VoiceOf(c.obj);
        if (c.Arg(0)) {
            auto* d = c.Ptr<uint32_t>(0);   // CreationFlags, ActiveFlags, InputChannels, InputSampleRate
            d[0] = 0; d[1] = 0; d[2] = voice.channels; d[3] = voice.rate;
        }
        return 0;
    }));
    for (const char* n : {"SetOutputVoices", "SetEffectChain", "EnableEffect", "DisableEffect", "GetEffectState", "SetEffectParameters",
                          "GetEffectParameters", "SetFilterParameters", "GetFilterParameters", "SetOutputFilterParameters", "GetOutputFilterParameters"})
        v.push_back(M(n));
    v.push_back(M("SetVolume", [](Call& c) -> uint32_t {
        std::scoped_lock lock {g_mutex};
        const uint32_t bits = static_cast<uint32_t>(c.Arg(0));
        std::memcpy(&VoiceOf(c.obj).volume, &bits, 4);
        return 0;
    }));
    v.push_back(M("GetVolume", [](Call& c) -> uint32_t {
        if (c.Arg(0)) *c.Ptr<float>(0) = VoiceOf(c.obj).volume;
        return 0;
    }));
    for (const char* n : {"SetChannelVolumes", "GetChannelVolumes", "SetOutputMatrix", "GetOutputMatrix"}) v.push_back(M(n));
    v.push_back(M("DestroyVoice", [](Call& c) -> uint32_t {
        std::scoped_lock lock {g_mutex};
        auto& voice = VoiceOf(c.obj);
        voice.destroyed = true;
        voice.started = false;
        voice.queue.clear();
        return 0;
    }));
    return v;
}

void StartPump(Kernel& k);

Obj* NewVoice(bool source, Iface* iface, uint32_t channels, uint32_t rate) {
    Runtime& rt = Runtime::Get();
    Obj* o = rt.NewObject(source ? "XAudio2SourceVoice" : "XAudio2MasteringVoice", {iface});
    std::scoped_lock lock {g_mutex};
    Voice& v = g_voices[o->id];
    v.source = source;
    v.channels = channels;
    v.rate = rate;
    v.avg_bytes_per_sec = rate * channels * 2;
    v.pointer = rt.Pointer(*o, *iface);
    return o;
}

} // namespace

void Register(winrt::Runtime& rt) {
    // ---- voices ----
    auto master = VoiceMethods();
    master.push_back(M("GetChannelMask", [](Call& c) -> uint32_t { if (c.Arg(0)) *c.Ptr<uint32_t>(0) = 3; return 0; }));
    g_master = &rt.DefineComInterface("IXAudio2MasteringVoice", "00000000-0000-0000-0000-00000000A002", std::move(master), nullptr);
    g_master->base_slots = 0;
    g_submix = &rt.DefineComInterface("IXAudio2SubmixVoice", "00000000-0000-0000-0000-00000000A003", VoiceMethods(), nullptr);
    g_submix->base_slots = 0;

    auto source = VoiceMethods();
    source.push_back(M("Start", [](Call& c) -> uint32_t { std::scoped_lock lock {g_mutex}; VoiceOf(c.obj).started = true; return 0; }));
    source.push_back(M("Stop", [](Call& c) -> uint32_t { std::scoped_lock lock {g_mutex}; VoiceOf(c.obj).started = false; return 0; }));
    source.push_back(M("SubmitSourceBuffer", [](Call& c) -> uint32_t {
        const auto* b = c.Ptr<uint8_t>(0);
        if (b == nullptr) return 0x80070057u;
        Buffer buf;
        std::memcpy(&buf.flags, b, 4);
        std::memcpy(&buf.bytes, b + 4, 4);
        std::memcpy(&buf.data, b + 8, 8);
        std::memcpy(&buf.play_begin, b + 16, 4);
        std::memcpy(&buf.play_length, b + 20, 4);
        std::memcpy(&buf.loop_begin, b + 24, 4);
        std::memcpy(&buf.loop_length, b + 28, 4);
        std::memcpy(&buf.loop_count, b + 32, 4);
        std::memcpy(&buf.context, b + 40, 8);
        std::scoped_lock lock {g_mutex};
        VoiceOf(c.obj).queue.push_back(buf);
        return 0;
    }));
    source.push_back(M("FlushSourceBuffers", [](Call& c) -> uint32_t {
        std::scoped_lock lock {g_mutex};
        auto& v = VoiceOf(c.obj);
        // The title is told about every buffer that was dropped, like the real engine does.
        // (Delivered by the pump on its next pass: mark them finished.)
        for (auto& b : v.queue) { b.played = 1e9; }
        return 0;
    }));
    source.push_back(M("Discontinuity"));
    source.push_back(M("ExitLoop", [](Call& c) -> uint32_t {
        std::scoped_lock lock {g_mutex};
        auto& v = VoiceOf(c.obj);
        if (!v.queue.empty()) v.queue.front().loop_count = 0;
        return 0;
    }));
    source.push_back(M("GetState", [](Call& c) -> uint32_t {
        std::scoped_lock lock {g_mutex};
        auto& v = VoiceOf(c.obj);
        if (c.Arg(0)) {
            auto* s = c.Ptr<uint8_t>(0);   // pCurrentBufferContext, BuffersQueued, SamplesPlayed
            const uint64_t ctx = v.queue.empty() ? 0 : v.queue.front().context;
            const uint32_t n = static_cast<uint32_t>(v.queue.size());
            std::memcpy(s, &ctx, 8);
            std::memcpy(s + 8, &n, 4);
            std::memcpy(s + 16, &v.samples_played, 8);
        }
        return 0;
    }));
    source.push_back(M("SetFrequencyRatio", [](Call& c) -> uint32_t {
        std::scoped_lock lock {g_mutex};
        const uint32_t bits = static_cast<uint32_t>(c.Arg(0));
        float r;
        std::memcpy(&r, &bits, 4);
        VoiceOf(c.obj).ratio = r > 0 ? r : 1.0f;
        return 0;
    }));
    source.push_back(M("GetFrequencyRatio", [](Call& c) -> uint32_t { if (c.Arg(0)) *c.Ptr<float>(0) = VoiceOf(c.obj).ratio; return 0; }));
    source.push_back(M("SetSourceSampleRate"));
    g_source = &rt.DefineComInterface("IXAudio2SourceVoice", "00000000-0000-0000-0000-00000000A001", std::move(source), nullptr);
    g_source->base_slots = 0;

    // ---- the engine ----
    std::vector<winrt::Method> engine;
    engine.push_back(M("RegisterForCallbacks"));
    engine.push_back(M("UnregisterForCallbacks"));
    engine.push_back(M("CreateSourceVoice", [](Call& c) -> uint32_t {
        // (ppSourceVoice, pSourceFormat, Flags, MaxFrequencyRatio, pCallback, pSendList, pEffectChain)
        const auto* fmt = c.Ptr<uint8_t>(1);
        uint16_t tag = 1, channels = 2;
        uint32_t rate = 48000, avg = 48000 * 4;
        if (fmt != nullptr) {
            std::memcpy(&tag, fmt, 2);
            std::memcpy(&channels, fmt + 2, 2);
            std::memcpy(&rate, fmt + 4, 4);
            std::memcpy(&avg, fmt + 8, 4);
        }
        Obj* o = NewVoice(true, g_source, channels, rate);
        {
            std::scoped_lock lock {g_mutex};
            Voice& v = VoiceOf(*o);
            v.avg_bytes_per_sec = avg ? avg : rate * channels * 2;
            v.callback = c.Arg(4);
        }
        UT_INFO("xaudio2: source voice tag %#x, %u ch, %u Hz", tag, channels, rate);
        *c.Ptr<uint64_t>(0) = Runtime::Get().Pointer(*o, *g_source);
        StartPump(c.kernel);
        return 0;
    }));
    engine.push_back(M("CreateSubmixVoice", [](Call& c) -> uint32_t {
        Obj* o = NewVoice(false, g_submix, static_cast<uint32_t>(c.Arg(1)), static_cast<uint32_t>(c.Arg(2)));
        *c.Ptr<uint64_t>(0) = Runtime::Get().Pointer(*o, *g_submix);
        return 0;
    }));
    engine.push_back(M("CreateMasteringVoice", [](Call& c) -> uint32_t {
        // (ppMasteringVoice, InputChannels, InputSampleRate, Flags, szDeviceId, pEffectChain, StreamCategory)
        Obj* o = NewVoice(false, g_master, 2, 48000);
        *c.Ptr<uint64_t>(0) = Runtime::Get().Pointer(*o, *g_master);
        UT_INFO("xaudio2: mastering voice");
        return 0;
    }));
    engine.push_back(M("StartEngine"));
    engine.push_back(M("StopEngine"));
    engine.push_back(M("CommitChanges"));
    engine.push_back(M("GetPerformanceData", [](Call& c) -> uint32_t { if (c.Arg(0)) std::memset(c.Ptr<void>(0), 0, 64); return 0; }));
    engine.push_back(M("SetDebugConfiguration"));
    for (int i = 0; i < 12; ++i) engine.push_back(M("Reserved"));   // Xbox One's additions after the PC layout
    g_engine = &rt.DefineComInterface("IXAudio2", "2B02E3CF-2E0B-4EC3-BE45-1B2A3FE7210D", std::move(engine), nullptr);

    // ---- the pump: a guest thread that loops into the host, which plays time forward ----
    std::vector<winrt::Method> pump;
    pump.push_back(M("Pump", [](Call& c) -> uint32_t {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        if (c.kernel.exiting) { std::this_thread::sleep_for(std::chrono::seconds(1)); return 0; }
        static auto last = std::chrono::steady_clock::now();
        const auto now = std::chrono::steady_clock::now();
        const double dt = std::chrono::duration<double>(now - last).count();
        last = now;
        std::vector<Pending> due;
        {
            std::scoped_lock lock {g_mutex};
            for (auto& [id, v] : g_voices) {
                if (!v.source || v.destroyed) continue;
                double budget = v.started ? dt * v.ratio : 0.0;
                while (!v.queue.empty()) {
                    Buffer& b = v.queue.front();
                    const bool flushed = b.played >= 1e8;
                    if (!flushed && !v.started) break;
                    if (!b.started && !flushed) {
                        b.started = true;
                        if (v.callback) due.push_back({v.callback, 3, v.pointer, b.context});   // OnBufferStart
                    }
                    const double length = b.bytes / static_cast<double>(v.avg_bytes_per_sec ? v.avg_bytes_per_sec : 1);
                    if (!flushed) {
                        const double take = std::min(budget, length - b.played);
                        b.played += take;
                        budget -= take;
                        v.samples_played += static_cast<uint64_t>(take * v.rate);
                    }
                    if (!flushed && b.played < length - 1e-9) break;
                    if (!flushed && b.loop_count > 0) {
                        if (b.loop_count != 255) --b.loop_count;
                        b.played = 0;
                        if (v.callback) due.push_back({v.callback, 5, v.pointer, b.context});   // OnLoopEnd
                        continue;
                    }
                    if (v.callback) {
                        due.push_back({v.callback, 4, v.pointer, b.context});                    // OnBufferEnd
                        if (b.flags & 0x40) due.push_back({v.callback, 2, v.pointer, 0});        // END_OF_STREAM -> OnStreamEnd
                    }
                    v.queue.pop_front();
                }
            }
        }
        if (due.empty()) return 0;
        auto idx = std::make_shared<size_t>(0);
        auto step = std::make_shared<std::function<GuestStep(NtThread&, uint64_t)>>();
        auto list = std::make_shared<std::vector<Pending>>(std::move(due));
        *step = [list, idx, step](NtThread&, uint64_t) -> GuestStep {
            if (*idx >= list->size()) return GuestStep::Return(0);
            const Pending p = (*list)[(*idx)++];
            // IXAudio2VoiceCallback: slots 0 PassStart, 1 PassEnd, 2 StreamEnd, 3 BufferStart, 4 BufferEnd, 5 LoopEnd, 6 Error
            return GuestStep::Call(Vtable(p.callback, p.slot), {p.callback, p.slot == 2 ? 0 : p.arg}, *step);
        };
        c.thread.CallGuest((*step)(c.thread, 0));
        return 0;
    }));
    g_pump = &rt.DefineComInterface("XAudio2Pump", "00000000-0000-0000-0000-00000000A0FF", std::move(pump), nullptr);
}

namespace {

void StartPump(Kernel& k) {
    {
        std::scoped_lock lock {g_mutex};
        if (g_pump_started) return;
        g_pump_started = true;
    }
    Runtime& rt = Runtime::Get();
    Obj* o = rt.NewObject("XAudio2Pump", {g_pump});
    const uint64_t block = rt.Pointer(*o, *g_pump);
    const uint64_t stub = reinterpret_cast<uint64_t*>(g_pump->vtable)[0];
    // loop: movabs rcx, block; movabs rax, stub; call rax; jmp loop
    const uint64_t code = rt.AllocGuest(48, 16);
    auto* p = reinterpret_cast<uint8_t*>(code);
    size_t n = 0;
    p[n++] = 0x48; p[n++] = 0xB9; std::memcpy(p + n, &block, 8); n += 8;
    p[n++] = 0x48; p[n++] = 0xB8; std::memcpy(p + n, &stub, 8); n += 8;
    p[n++] = 0xFF; p[n++] = 0xD0;
    p[n++] = 0xEB; p[n++] = static_cast<uint8_t>(-static_cast<int>(n + 1));
    std::string error;
    auto thread = k.NewThread(0x100000, 0x10000, error);
    if (!thread) { UT_ERROR("xaudio2: cannot start the pump thread: %s", error.c_str()); return; }
    k.StartThread(thread, FindExport(k.NtdllBase(), "RtlUserThreadStart"), code, 0, false);
    UT_INFO("xaudio2: pump thread %llu", (unsigned long long)thread->tid_);
}

} // namespace

uint64_t CreateXAudio2Object(Kernel&, NtThread&, SyscallArgs& a) {
    // (IXAudio2** ppXAudio2, UINT32 Flags, XAUDIO2_PROCESSOR Processor)
    Runtime& rt = Runtime::Get();
    Obj* o = rt.NewObject("XAudio2", {g_engine});
    if (a[0] != 0) *a.Ptr<uint64_t>(0) = rt.Pointer(*o, *g_engine);
    UT_INFO("xaudio2: engine created");
    return 0;
}

} // namespace utsuro::nt::xaudio2
