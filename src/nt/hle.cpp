// hle.cpp -- exports of guest DLLs that are implemented by the host instead. The entry of
// the guest function is overwritten with `mov r10, rcx; mov eax, N; syscall; ret`, which
// lands in Kernel::Dispatch like any other syscall; the host function's result is rax.
#include "nt/d3d.h"
#include "nt/kernel.h"
#include "nt/winrt.h"
#include "nt/xaudio2.h"

#include "engine/log.h"

#include <stdlib.h>

#include <algorithm>
#include <cstring>
#include <map>
#include <mutex>

namespace utsuro::nt {

namespace {

using HleFn = uint64_t (*)(Kernel&, NtThread&, SyscallArgs&);

struct Hook {
    const char* image;   ///< Lower-case file name.
    const char* name;    ///< Export name.
    HleFn fn;
    unsigned displaced;  ///< Non-zero: the host may decline (return -1) and the original runs; this many
                         ///< bytes (whole instructions, nothing relative) are copied to the stub.
};

// bcrypt!BCryptGenRandom(hAlgorithm, pbBuffer, cbBuffer, dwFlags): the Era bcrypt asks a
// kernel CNG driver for everything; random bytes are all the title's startup needs.
uint64_t BCryptGenRandom(Kernel&, NtThread&, SyscallArgs& a) {
    auto* buf = a.Ptr<uint8_t>(1);
    const uint32_t len = static_cast<uint32_t>(a[2]);
    if (buf == nullptr) return static_cast<uint64_t>(static_cast<int64_t>(STATUS_INVALID_PARAMETER));
    arc4random_buf(buf, len);
    return 0;
}

// d3d11_x!D3DAllocateGraphicsMemory(size, alignment, desiredGpuAddress, flags, ppAddress) and friends: the console reaches
// graphics-visible memory through the kernel; here it is ordinary guest memory, aligned as asked.
struct GraphicsBlock { uint64_t base; uint64_t size; };
std::mutex g_graphics_mutex;
std::map<uint64_t, GraphicsBlock> g_graphics;   // address handed out -> allocation

bool AllocAligned(Kernel& k, uint64_t size, uint64_t align, uint64_t desired, uint64_t* out) {
    align = std::max<uint64_t>(align, 0x10000);
    for (uint64_t extra : {uint64_t {0}, align}) {
        uint64_t base = desired, len = size + extra;
        if (k.Vm().Allocate(&base, &len, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE) != STATUS_SUCCESS) continue;
        const uint64_t at = (base + align - 1) & ~(align - 1);
        if (at + size > base + len) { k.Vm().Free(&base, &len, MEM_RELEASE); continue; }
        std::scoped_lock lock {g_graphics_mutex};
        g_graphics[at] = {base, len};
        *out = at;
        return true;
    }
    return false;
}

uint64_t D3DAllocateGraphicsMemory(Kernel& k, NtThread&, SyscallArgs& a) {
    auto* out = a.Ptr<uint64_t>(4);
    if (out == nullptr || a[1] > 0x20000) return 0x80070057u;   // E_INVALIDARG
    if (AllocAligned(k, a[0], a[1], a[2], out)) return 0;
    *out = 0;
    return 0x8007000Eu;   // E_OUTOFMEMORY
}

// acphal!ApuAlloc(ppVirtualAddress, pApuAddress, size, alignment, flags): memory the audio processor can see; the APU address
// handed back is just the low half of the host address, which is what the title gives back to the other Apu calls.
uint64_t ApuAlloc(Kernel& k, NtThread&, SyscallArgs& a) {
    auto* out = a.Ptr<uint64_t>(0);
    uint64_t at = 0;
    if (out == nullptr || !AllocAligned(k, a[2], a[3], 0, &at)) return 0x8007000Eu;
    *out = at;
    if (auto* apu = a.Ptr<uint32_t>(1)) *apu = static_cast<uint32_t>(at);
    return 0;
}

uint64_t ApuMapVirtualAddress(Kernel&, NtThread&, SyscallArgs& a) {
    if (auto* apu = a.Ptr<uint32_t>(0)) *apu = static_cast<uint32_t>(a[1]);
    return 0;
}

uint64_t ApuIsVirtualAddressValid(Kernel&, NtThread&, SyscallArgs&) { return 1; }

uint64_t D3DFreeGraphicsMemory(Kernel& k, NtThread&, SyscallArgs& a) {
    std::scoped_lock lock {g_graphics_mutex};
    auto it = g_graphics.find(a[0]);
    if (it == g_graphics.end()) return 0x80070057u;
    uint64_t base = it->second.base, len = 0;
    k.Vm().Free(&base, &len, MEM_RELEASE);
    g_graphics.erase(it);
    return 0;
}

uint64_t ReturnOk(Kernel&, NtThread&, SyscallArgs&) { return 0; }

std::string HstringToUtf8(uint64_t h) {
    if (h == 0) return {};
    const uint32_t len = *reinterpret_cast<const uint32_t*>(h + 4);
    const auto* buf = reinterpret_cast<const char16_t*>(*reinterpret_cast<const uint64_t*>(h + 0x10));
    return Utf16ToUtf8(std::u16string(buf, len));
}

std::string GuidToString(const uint8_t* g) {
    char b[64];
    std::snprintf(b, sizeof b, "{%08x-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x}", *reinterpret_cast<const uint32_t*>(g),
                  *reinterpret_cast<const uint16_t*>(g + 4), *reinterpret_cast<const uint16_t*>(g + 6), g[8], g[9], g[10], g[11], g[12], g[13], g[14], g[15]);
    return b;
}

// Logs the guest call chain of a displaced-prologue hook (four pushes, then the return address).
void LogCaller(Kernel& k, NtThread& thread, SyscallArgs& a, const char* what) {
    GuestCpuState st;
    if (thread.control == nullptr || !thread.control->CaptureCpuState(&st)) return;
    const uint64_t ret = *reinterpret_cast<uint64_t*>(a.rsp + 32);
    st.gregs[4] = a.rsp + 40;
    const auto frames = k.Backtrace(ret, st.gregs, 10);
    UT_INFO("%s called from:", what);
    for (size_t i = 0; i < frames.size(); ++i) UT_INFO("      #%zu %s", i, k.Describe(frames[i]).c_str());
}

// combase!RoGetActivationFactory(HSTRING activatableClassId, REFIID iid, void** factory)
uint64_t RoGetActivationFactory(Kernel& k, NtThread& thread, SyscallArgs& a) {
    const std::string name = HstringToUtf8(a[0]);
    winrt::Guid iid;
    std::memcpy(iid.b, a.Ptr<uint8_t>(1), 16);
    auto* out = a.Ptr<uint64_t>(2);
    uint64_t p = 0;
    const uint32_t hr = winrt::Runtime::Get().GetActivationFactory(name, iid, &p);
    if (hr == 0) { UT_DEBUG("RoGetActivationFactory(%s, %s) -> host class", name.c_str(), iid.ToString().c_str()); if (out) *out = p; return 0; }
    UT_INFO("RoGetActivationFactory(%s, %s): not a host class (%#x), using the system's", name.c_str(), iid.ToString().c_str(), hr);
    LogCaller(k, thread, a, "RoGetActivationFactory");
    return ~0ull;  // decline: run the original
}

const Hook kHooks[] = {
    {"combase.dll", "RoGetActivationFactory", RoGetActivationFactory, 5},
    {"bcrypt.dll", "BCryptGenRandom", BCryptGenRandom, 0},
    {"d3d11_x.dll", "D3D11CreateDevice", d3d::CreateDevice, 0},
    {"d3d11_x.dll", "D3DAllocateGraphicsMemory", D3DAllocateGraphicsMemory, 0},
    {"d3d11_x.dll", "D3DFreeGraphicsMemory", D3DFreeGraphicsMemory, 0},
    {"d3d11_x.dll", "D3DConfigureVirtualMemory", ReturnOk, 0},
    {"d3d11_x.dll", "D3DMapEsramMemory", ReturnOk, 0},
    {"d3d11_x.dll", "DXGIXSetVLineNotification", ReturnOk, 0},
    {"d3d11_x.dll", "DXGIXSetFrameNotification", ReturnOk, 0},
    {"acphal.dll", "ApuAlloc", ApuAlloc, 0},
    {"acphal.dll", "ApuFree", D3DFreeGraphicsMemory, 0},
    {"acphal.dll", "ApuCreateHeap", ReturnOk, 0},
    {"acphal.dll", "ApuMapVirtualAddress", ApuMapVirtualAddress, 0},
    {"acphal.dll", "ApuIsVirtualAddressValid", ApuIsVirtualAddressValid, 0},
    {"xaudio2_9.dll", "CreateXAudio2Object", xaudio2::CreateXAudio2Object, 0},
};

constexpr uint32_t kHleBase = 0xFFFF8000u;  // below winrt::Runtime::kBase's span ends here

} // namespace

void Kernel::ApplyHleHooks(const std::string& image, uint64_t base) {
    const auto exports = ListExports(base);
    bool any = false;
    for (size_t i = 0; i < sizeof(kHooks) / sizeof(kHooks[0]); ++i) {
        if (image != kHooks[i].image) continue;
        auto it = exports.find(kHooks[i].name);
        if (it == exports.end()) { UT_WARN("hle: %s!%s is not exported", kHooks[i].image, kHooks[i].name); continue; }
        auto* fn = reinterpret_cast<uint8_t*>(it->second);
        const uint32_t number = kHleBase + static_cast<uint32_t>(i);
        if (kHooks[i].displaced == 0) {
            size_t n = 0;
            fn[n++] = 0x4C; fn[n++] = 0x8B; fn[n++] = 0xD1;         // mov r10, rcx
            fn[n++] = 0xB8;
            std::memcpy(fn + n, &number, 4); n += 4;                  // mov eax, number
            fn[n++] = 0x0F; fn[n++] = 0x05;                           // syscall
            fn[n++] = 0xC3;                                           // ret
        } else {
            if (hle_stub_next_ == 0) hle_stub_next_ = kusd_ + 0xC000;
            auto* stub = reinterpret_cast<uint8_t*>(hle_stub_next_);
            size_t n = 0;
            auto put = [&](std::initializer_list<uint8_t> bytes) { for (uint8_t v : bytes) stub[n++] = v; };
            put({0x50, 0x51, 0x41, 0x53, 0x41, 0x52});                // push rax, rcx, r11, r10
            put({0x4C, 0x8B, 0xD1, 0xB8});                            // mov r10, rcx; mov eax, number
            std::memcpy(stub + n, &number, 4); n += 4;
            put({0x0F, 0x05});                                        // syscall
            put({0x48, 0x83, 0xF8, 0xFF, 0x74, 0x05});                // cmp rax, -1; je fall
            put({0x48, 0x83, 0xC4, 0x20, 0xC3});                      // add rsp, 32; ret
            put({0x41, 0x5A, 0x41, 0x5B, 0x59, 0x58});                // fall: pop r10, r11, rcx, rax
            std::memcpy(stub + n, fn, kHooks[i].displaced); n += kHooks[i].displaced;
            stub[n++] = 0xE9;
            const int32_t back = static_cast<int32_t>((reinterpret_cast<uint64_t>(fn) + kHooks[i].displaced) - (hle_stub_next_ + n + 4));
            std::memcpy(stub + n, &back, 4); n += 4;
            fn[0] = 0xE9;
            const int32_t fwd = static_cast<int32_t>(hle_stub_next_ - (reinterpret_cast<uint64_t>(fn) + 5));
            std::memcpy(fn + 1, &fwd, 4);
            for (unsigned k = 5; k < kHooks[i].displaced; ++k) fn[k] = 0x90;
            hle_stub_next_ += (n + 15) & ~15ull;
        }
        any = true;
        UT_INFO("hle: %s!%s", kHooks[i].image, kHooks[i].name);
    }
    if (any) {
        uint64_t sb = kusd_ + 0x1000, ss = 0xF000;
        vm_->Protect(&sb, &ss, PAGE_EXECUTE_READ, nullptr);
        space_->NotifyContentsReplaced(base, base + 0x1000000);
        space_->NotifyContentsReplaced(kusd_ + 0x1000, kusd_ + 0x10000);
    }
}

bool Kernel::DispatchHle(NtThread& thread, uint32_t number, SyscallArgs& args, uint64_t* result) {
    if (number >= winrt::Runtime::kBase && number < kHleBase) return winrt::Runtime::Get().Dispatch(thread, number, args, result);
    if (number < kHleBase || number >= kHleBase + sizeof(kHooks) / sizeof(kHooks[0])) return false;
    // The syscall instruction ended the stub; return to the stub's own `ret`.
    *result = kHooks[number - kHleBase].fn(*this, thread, args);
    return true;
}

} // namespace utsuro::nt
