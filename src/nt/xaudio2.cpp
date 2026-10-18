// xaudio2.cpp -- the host's XAudio2. The Era's xaudio2_9.dll talks to the console's audio
// co-processor; the title (through Miles) only needs the interface, so CreateXAudio2Object is
// replaced by a host object. Source voices keep their queued buffers and "play" them at the
// right rate on a pump thread, which also delivers the voice callbacks the title waits for.
#include "nt/xaudio2.h"

#include "nt/winrt.h"

#include "host/audio.h"

#include "engine/log.h"

#include <algorithm>
#include <chrono>
#include <cmath>
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
    bool started {false};
    bool flushed {false};
};

struct Voice {
    bool source {false};
    bool started {false};
    uint32_t channels {2}, rate {48000}, avg_bytes_per_sec {48000 * 4};
    uint32_t format_tag {1}, block_align {4}, bits {16};
    double cursor {0};        ///< Position in the current buffer, in source frames.
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
        VoiceOf(c.obj).volume = c.Float(0);
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
        static int shown = 0;
        if (shown++ < 6) UT_INFO("xaudio2: SubmitSourceBuffer %u bytes flags %#x loop %u at %#llx", buf.bytes, buf.flags, buf.loop_count, (unsigned long long)buf.data);
        VoiceOf(c.obj).queue.push_back(buf);
        return 0;
    }));
    source.push_back(M("FlushSourceBuffers", [](Call& c) -> uint32_t {
        std::scoped_lock lock {g_mutex};
        auto& v = VoiceOf(c.obj);
        // The title is told about every buffer that was dropped, like the real engine does.
        // (Delivered by the pump on its next pass: mark them finished.)
        for (auto& b : v.queue) b.flushed = true;
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
        const float r = c.Float(0);
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
        uint16_t tag = 1, channels = 2, align = 4, bits = 16;
        uint32_t rate = 48000, avg = 48000 * 4;
        if (fmt != nullptr) {
            std::memcpy(&tag, fmt, 2);
            std::memcpy(&channels, fmt + 2, 2);
            std::memcpy(&rate, fmt + 4, 4);
            std::memcpy(&avg, fmt + 8, 4);
            std::memcpy(&align, fmt + 12, 2);
            std::memcpy(&bits, fmt + 14, 2);
        }
        Obj* o = NewVoice(true, g_source, channels, rate);
        {
            std::scoped_lock lock {g_mutex};
            Voice& v = VoiceOf(*o);
            v.avg_bytes_per_sec = avg ? avg : rate * channels * 2;
            v.format_tag = tag;
            v.block_align = align ? align : channels * 2;
            v.bits = bits;
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
        // Output frames to produce this pass, carrying the fraction.
        static double carry = 0;
        carry += dt * audio::kRate;
        const size_t out_frames = static_cast<size_t>(std::clamp(carry, 0.0, 9600.0));
        carry -= static_cast<double>(out_frames);
        std::vector<float> mix(out_frames * 2, 0.f);
        std::vector<Pending> due;
        {
            std::scoped_lock lock {g_mutex};
            for (auto& [id, v] : g_voices) {
                if (!v.source || v.destroyed) continue;
                const bool pcm = (v.format_tag == 1 && v.bits == 16) || (v.format_tag == 3 && v.bits == 32);
                const double step = static_cast<double>(v.rate) * v.ratio / audio::kRate;   // source frames per output frame
                size_t out = 0;
                while (!v.queue.empty()) {
                    Buffer& b = v.queue.front();
                    if (!b.flushed && !v.started) break;
                    if (!b.started && !b.flushed) {
                        b.started = true;
                        if (v.callback) due.push_back({v.callback, 3, v.pointer, b.context});   // OnBufferStart
                    }
                    const double total = pcm ? b.bytes / static_cast<double>(v.block_align ? v.block_align : 1)
                                             : b.bytes / static_cast<double>(v.avg_bytes_per_sec ? v.avg_bytes_per_sec : 1) * v.rate;
                    bool finished = b.flushed;
                    // Play the buffer for as many output frames as are left in this pass.
                    while (!finished && out < out_frames) {
                        if (pcm && v.queue.front().data != 0) {
                            const uint64_t idx = static_cast<uint64_t>(v.cursor);
                            const uint32_t ch = std::max(1u, v.channels);
                            const uint8_t* base = reinterpret_cast<const uint8_t*>(b.data) + idx * v.block_align;
                            float f[8] = {};
                            for (uint32_t k = 0; k < std::min(ch, 8u); ++k) {
                                if (v.format_tag == 3) { float x; std::memcpy(&x, base + k * 4, 4); f[k] = x; }
                                else { int16_t x; std::memcpy(&x, base + k * 2, 2); f[k] = x / 32768.0f; }
                            }
                            float l, r;
                            if (ch == 1) l = r = f[0];
                            else if (ch >= 6) { l = f[0] + 0.7071f * f[2] + 0.7071f * f[4]; r = f[1] + 0.7071f * f[2] + 0.7071f * f[5]; }
                            else { l = f[0]; r = f[1]; }
                            mix[out * 2] += l * v.volume;
                            mix[out * 2 + 1] += r * v.volume;
                        }
                        ++out;
                        v.cursor += step;
                        v.samples_played += static_cast<uint64_t>(step);
                        if (v.cursor >= total) finished = true;
                    }
                    if (!finished) break;           // this buffer needs more output frames than this pass has
                    if (!b.flushed && b.loop_count > 0) {
                        if (b.loop_count != 255) --b.loop_count;
                        v.cursor = std::max(0.0, v.cursor - total);
                        if (v.callback) due.push_back({v.callback, 5, v.pointer, b.context});   // OnLoopEnd
                        continue;
                    }
                    if (v.callback) {
                        due.push_back({v.callback, 4, v.pointer, b.context});                    // OnBufferEnd
                        if (b.flags & 0x40) due.push_back({v.callback, 2, v.pointer, 0});        // END_OF_STREAM -> OnStreamEnd
                    }
                    v.cursor = std::max(0.0, v.cursor - total);
                    if (b.flushed) v.cursor = 0;
                    v.queue.pop_front();
                }
            }
        }
        {
            static float peak = 0;
            static auto since = std::chrono::steady_clock::now();
            for (float x : mix) peak = std::max(peak, std::fabs(x));
            if (std::getenv("UTSURO_AUDIOLOG") && now - since > std::chrono::seconds(1)) { UT_INFO("xaudio2: peak %.3f", peak); peak = 0; since = now; }
        }
        if (out_frames != 0) { static bool up = audio::Init(); (void)up; audio::Write(mix.data(), out_frames); }
        static int logged = 0;
        if (logged < 5) UT_INFO("xaudio2: pump pass %d, %zu callbacks, rsp %#llx", logged++, due.size(), (unsigned long long)c.args.rsp);
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
    g_pump->base_slots = 0;
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
    p[n++] = 0xEB;
    p[n] = static_cast<uint8_t>(-static_cast<int>(n + 1));   // back to the first instruction
    ++n;
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
