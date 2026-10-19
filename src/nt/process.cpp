// process.cpp -- bringing a process into existence: ntdll, the executable, the PEB, the
// first thread; and the thread machinery shared with NtCreateThreadEx.
#include "nt/kernel.h"
#include "nt/winrt.h"

#include "engine/log.h"
#include "jit/jit.h"

#include <pthread.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>

namespace utsuro::nt {

namespace {

uint64_t AlignUp(uint64_t v, uint64_t a) { return (v + a - 1) & ~(a - 1); }
uint64_t AlignDown(uint64_t v, uint64_t a) { return v & ~(a - 1); }

void PutU16(uint64_t base, size_t off, uint16_t v) { *reinterpret_cast<uint16_t*>(base + off) = v; }
void PutU32(uint64_t base, size_t off, uint32_t v) { *reinterpret_cast<uint32_t*>(base + off) = v; }
void PutU64(uint64_t base, size_t off, uint64_t v) { *reinterpret_cast<uint64_t*>(base + off) = v; }

struct StartData {
    Kernel* kernel;
    std::shared_ptr<NtThread> thread;
    uint64_t rip;
    uint64_t rsp;
    uint64_t rcx;
    uint64_t gs;
};

thread_local NtThread* g_self = nullptr;

} // namespace

// The Kernel implementation reaches the thread-local through this.
NtThread* CurrentNtThreadRaw() { return g_self; }

// ---------------------------------------------------------------------------------------
// Boot
// ---------------------------------------------------------------------------------------

bool Kernel::Boot(std::string& error) {
    std::string e;
    if (!jit::Install(e)) { error = e; return false; }
    // The guest lives 1:1 at host addresses, so the arena is as big as the host will give: 128 GB on a Mac, less where
    // the process is limited (Minecraft reaches the menu in 2 GB and plays in 4). UTSURO_ARENA_GB pins it.
    GuestAddressSpace* space = nullptr;
    uint64_t want = options_.arena_size;
    if (const char* gb = std::getenv("UTSURO_ARENA_GB")) want = std::max<uint64_t>(1, std::strtoull(gb, nullptr, 0)) << 30;
    for (; want >= (uint64_t {2} << 30); want >>= 1) {
        space = GuestAddressSpace::Reserve(want, e, 0x10000);
        if (space != nullptr) break;
        UT_WARN("no room for a %llu GB guest arena: %s", (unsigned long long)(want >> 30), e.c_str());
    }
    if (space == nullptr) { error = e; return false; }
    space_.reset(space);
    EngineOptions eo;
    eo.tso_enabled = true;
    engine_ = FexEngine::Create(*space_, eo, e);
    if (!engine_) { error = "engine: " + e; return false; }
    vm_ = std::make_unique<VirtualMemory>(*space_, space_->Base(), space_->Size());

    InitRegistry(options_.system_root + "/System32/config");
    RegisterPackageClasses(options_.title_root + "/AppxManifest.xml");
    winrt_init_pending_ = true;
    if (!PrepareNtdll(error)) return false;
    RegisterSyscalls();
    BuildKusd();
    ApplyKusdPatches(*ntdll_image_, ntdll_base_);
    winrt::Runtime::Get().Init(*this);
    InstallTracepointsFor("ntdll.dll", ntdll_base_);
    timer_thread_ = std::thread([this] { TimerThreadMain(); });
    SetFaultReporter([this](uint64_t rip, const uint64_t* regs) {
        const auto frames = Backtrace(rip, regs, 24);
        for (size_t i = 0; i < frames.size(); ++i) UT_WARN("    #%zu %s", i, Describe(frames[i]).c_str());
        {
            // The top of the stack, annotated: enough to spot a return address or an argument.
            const uint64_t sp = regs[4];
            for (int i = 0; i < 24; ++i) {
                const uint64_t w = *reinterpret_cast<uint64_t*>(sp + i * 8);
                UT_WARN("    [rsp+%#x] %#llx %s", i * 8, (unsigned long long)w, w > 0x1000000000ull ? Describe(w).c_str() : "");
            }
        }
        DumpRequestedMemory("UTSURO_FAULTDUMP");
    });
    if (const char* sm = std::getenv("UTSURO_SAMPLE")) {
        // Histogram of guest rips over `sm` seconds, printed once: where is the time going?
        const int seconds = std::max(1, std::atoi(sm));
        std::thread([this, seconds] {
            pthread_setname_np("utsuro-sampler");
            std::map<uint64_t, int> hist;
            std::map<std::string, int> pairs;
            std::map<std::string, int> chains;
            std::map<uint64_t, int> rsps;
            const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
            while (std::chrono::steady_clock::now() < end) {
                std::vector<uint64_t> rips;
                std::vector<std::array<uint64_t, 16>> regs;
                SampleGuestRips(rips, &regs);
                for (size_t i = 0; i < rips.size(); ++i) {
                    ++hist[rips[i] & ~0xFull];
                    {
                        // The first few image-looking words on the stack, as a crude call chain.
                        std::string chain;
                        int got = 0;
                        for (uint64_t w = 0; w < 400 && got < 7; ++w) {
                            const uint64_t v = *reinterpret_cast<uint64_t*>(regs[i][4] + w * 8);
                            if (v < 0x1000000000ull) continue;
                            std::string d = Describe(v);
                            if (d.empty() || d[0] == '0' || d.find("+0x") == std::string::npos || d.find("+0x0") == 0) continue;
                            const auto plus = d.find(" (");
                            chain += " <" + (plus == std::string::npos ? d : d.substr(0, plus));
                            ++got;
                        }
                        ++chains[chain];
                        ++rsps[regs[i][4]];
                        if (Describe(rips[i]).find("LdrResolveDelayLoadsFromDll+0x11") != std::string::npos) {
                            char b[200];
                            std::snprintf(b, sizeof b, "head=%#llx [head]=%#llx [head+8]=%#llx cur=%#llx [cur]=%#llx [cur+8]=%#llx hash@entry=%#x r13=%#llx", (unsigned long long)regs[i][14], (unsigned long long)*reinterpret_cast<uint64_t*>(regs[i][14]), (unsigned long long)*reinterpret_cast<uint64_t*>(regs[i][14] + 8), (unsigned long long)regs[i][15], (unsigned long long)*reinterpret_cast<uint64_t*>(regs[i][15]), (unsigned long long)*reinterpret_cast<uint64_t*>(regs[i][15] + 8), *reinterpret_cast<uint32_t*>(regs[i][15] - 0x70 + 0x100), (unsigned long long)regs[i][13]);
                            ++pairs[b];
                        }
                    }
                    if (Describe(rips[i]).find("RtlEqualUnicodeString+0x") != std::string::npos && regs[i][1] > 0x10000 && regs[i][2] > 0x10000) {
                        auto s = [](uint64_t p) { return Utf16ToUtf8(ReadUnicodeString(reinterpret_cast<const UNICODE_STRING*>(p))); };
                        ++pairs[s(regs[i][1]).substr(0, 60) + "  vs  " + s(regs[i][2]).substr(0, 60)];
                    }
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            std::vector<std::pair<int, uint64_t>> sorted;
            for (auto& [a, n] : hist) sorted.emplace_back(n, a);
            std::sort(sorted.rbegin(), sorted.rend());
            for (size_t i = 0; i < sorted.size() && i < 60; ++i) UT_WARN("sample %5d  %#llx %s", sorted[i].first, (unsigned long long)sorted[i].second, Describe(sorted[i].second).c_str());
            for (auto& [k, n] : pairs) UT_WARN("pair %5d  %s", n, k.c_str());
            for (auto& [k, n] : rsps) UT_WARN("rsp %#llx x%d", (unsigned long long)k, n);
            for (auto& [k, n] : chains) if (n > 30) UT_WARN("chain %5d %s", n, k.c_str());
            std::fflush(stderr);
        }).detach();
    }
    if (const char* wd = std::getenv("UTSURO_WATCHDOG")) {
        const int ms = std::max(100, std::atoi(wd) * 1000);
        std::thread([this, ms] {
            pthread_setname_np("utsuro-watchdog");
            for (;;) {
                std::this_thread::sleep_for(std::chrono::milliseconds(ms));
                DumpGuestThreads([this](uint64_t a) { return Describe(a); }, [this](uint64_t rip, const uint64_t* regs) {
                    const auto frames = Backtrace(rip, regs, 20);
                    for (size_t i = 0; i < frames.size(); ++i) UT_WARN("    #%zu %s", i, Describe(frames[i]).c_str());
                    if (Describe(rip).find("RtlEqualUnicodeString") != std::string::npos) {
                        auto s = [](uint64_t p) { return p > 0x10000 ? Utf16ToUtf8(ReadUnicodeString(reinterpret_cast<const UNICODE_STRING*>(p))) : std::string("?"); };
                        UT_WARN("    RtlEqualUnicodeString(\"%s\", \"%s\")", s(regs[1]).c_str(), s(regs[2]).c_str());
                    }
                });
                if (const char* dump = std::getenv("UTSURO_DUMP")) {
                    // UTSURO_DUMP=rva:len writes that part of ntdll as it is in memory now.
                    const uint64_t rva = std::strtoull(dump, nullptr, 16);
                    const char* colon = std::strchr(dump, ':');
                    const size_t len = colon ? std::strtoull(colon + 1, nullptr, 0) : 256;
                    if (FILE* f = std::fopen("/tmp/utsuro-dump.bin", "wb")) {
                        std::fwrite(reinterpret_cast<void*>(ntdll_base_ + rva), 1, len, f);
                        std::fclose(f);
                    }
                }
                DumpLoaderState();
                DumpHashTable();
                UT_WARN("globals: 9c410=%#llx 9c470=%#llx", (unsigned long long)*reinterpret_cast<uint64_t*>(ntdll_base_ + 0x9c410),
                        (unsigned long long)*reinterpret_cast<uint64_t*>(ntdll_base_ + 0x9c470));
                for (auto& [b, n] : image_names) if (b != ntdll_base_) DumpImportProgress(b);
            }
        }).detach();
    }
    if (!BuildProcess(error)) return false;
    return true;
}

// UTSURO_xxx=image+rva:len[:deref] dumps qwords of a loaded image (following the pointer stored there when
// ":deref" is given).
void Kernel::DumpRequestedMemory(const char* env) {
    const char* fd = std::getenv(env);
    if (fd == nullptr) return;
    const std::string spec = fd;
    const auto plus = spec.find('+'), colon = spec.find(':');
    if (plus == std::string::npos || colon == std::string::npos) return;
    std::string want = spec.substr(0, plus);
    for (auto& c : want) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    std::scoped_lock lock {image_mutex};
    for (auto& [b, n] : image_names) {
        std::string low = n.substr(n.rfind('\\') + 1);
        for (auto& c : low) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (low != want) continue;
        uint64_t at = b + std::strtoull(spec.c_str() + plus + 1, nullptr, 16);
        const size_t len = std::strtoull(spec.c_str() + colon + 1, nullptr, 0);
        if (spec.find(":deref") != std::string::npos) at = *reinterpret_cast<uint64_t*>(at);
        for (size_t o = 0; o < len; o += 16)
            UT_WARN("    %#llx: %016llx %016llx", (unsigned long long)(at + o), (unsigned long long)*reinterpret_cast<uint64_t*>(at + o), (unsigned long long)*reinterpret_cast<uint64_t*>(at + o + 8));
    }
}

void Kernel::DumpLoaderState() {
    // Walks PEB->Ldr's load-order list, which is plain guest memory.
    const uint64_t ldr = *reinterpret_cast<uint64_t*>(peb_ + peb::kLdr);
    if (ldr == 0) { UT_WARN("loader: PEB->Ldr is not set up yet"); return; }
    const uint64_t head = ldr + 0x10;
    uint64_t cur = *reinterpret_cast<uint64_t*>(head);
    int n = 0;
    UT_WARN("loader: InLoadOrderModuleList at %#llx", (unsigned long long)head);
    while (cur != head && n < 40) {
        const uint64_t entry = cur;  // InLoadOrderLinks is the first field
        const uint64_t base = *reinterpret_cast<uint64_t*>(entry + 0x30);
        const auto* name = reinterpret_cast<const UNICODE_STRING*>(entry + 0x58);
        UT_WARN("loader:   %#llx base %#llx %s  loadcount %d flags %#x", (unsigned long long)entry, (unsigned long long)base,
                Utf16ToUtf8(ReadUnicodeString(name)).c_str(), *reinterpret_cast<int32_t*>(entry + 0x6C), *reinterpret_cast<uint32_t*>(entry + 0x68));
        const uint64_t node = *reinterpret_cast<uint64_t*>(entry + 0x90);
        if (node > 0x10000) {
            const auto* q = reinterpret_cast<const uint32_t*>(node);
            UT_WARN("loader:     ddag node %#llx: %08x %08x %08x %08x | %08x %08x %08x %08x | %08x %08x %08x %08x", (unsigned long long)node,
                    q[0], q[1], q[2], q[3], q[4], q[5], q[6], q[7], q[8], q[9], q[10], q[11]);
            UT_WARN("loader:     state=%d (node+0x38)  deps=%u  node+0x20=%08x %08x  +0x28=%08x%08x +0x30=%08x%08x", q[14], q[8], q[8], q[9], q[11], q[10], q[13], q[12]);
        }
        cur = *reinterpret_cast<uint64_t*>(cur);
        ++n;
    }
}

void Kernel::DumpImportProgress(uint64_t base) {
    const auto* b = reinterpret_cast<const uint8_t*>(base);
    const uint32_t nt = *reinterpret_cast<const uint32_t*>(b + 0x3C);
    const uint32_t imp = *reinterpret_cast<const uint32_t*>(b + nt + 24 + 112 + 8);
    if (imp == 0) return;
    for (auto* d = reinterpret_cast<const uint32_t*>(b + imp); d[3] != 0; d += 5) {
        const uint64_t* ilt = reinterpret_cast<const uint64_t*>(b + (d[0] ? d[0] : d[4]));
        const uint64_t* iat = reinterpret_cast<const uint64_t*>(b + d[4]);
        int total = 0, bound = 0;
        for (; ilt[total] != 0; ++total) if (iat[total] != ilt[total]) ++bound;
        UT_WARN("imports of %s: %s  %d/%d bound", Describe(base).c_str(), reinterpret_cast<const char*>(b + d[3]), bound, total);
    }
}

void Kernel::DumpHashTable() {
    const uint64_t table = ntdll_base_ + 0x9c4f0;
    for (int b = 0; b < 32; ++b) {
        const uint64_t head = table + b * 16;
        uint64_t cur = *reinterpret_cast<uint64_t*>(head);
        const uint64_t blink = *reinterpret_cast<uint64_t*>(head + 8);
        int n = 0;
        std::string chain;
        while (cur != head && n < 6 && cur > 0x10000) {
            char buf[40];
            std::snprintf(buf, sizeof buf, " %#llx", (unsigned long long)cur);
            chain += buf;
            cur = *reinterpret_cast<uint64_t*>(cur);
            ++n;
        }
        if (n == 0 && cur == head && blink == head) continue;
        UT_WARN("hash[%d] head %#llx flink %#llx blink %#llx chain:%s%s", b, (unsigned long long)head,
                (unsigned long long)*reinterpret_cast<uint64_t*>(head), (unsigned long long)blink, chain.c_str(),
                cur == head ? " (closed)" : " ...");
    }
}

bool Kernel::PrepareNtdll(std::string& error) {
    const std::string path = options_.system_root + "/System32/ntdll.dll";
    std::string e;
    ntdll_image_ = LoadPe(path, e);
    if (!ntdll_image_) { error = path + ": " + e; return false; }
    ntdll_image_->nt_path = "C:\\Windows\\System32\\ntdll.dll";
    SectionObject section;
    section.image = std::move(ntdll_image_);
    uint64_t base = 0, size = 0;
    const NTSTATUS rc = MapImageSection(section, &base, &size, true);
    ntdll_image_ = std::move(section.image);
    if (rc != STATUS_SUCCESS && rc != STATUS_IMAGE_NOT_AT_BASE) { error = "cannot map ntdll"; return false; }
    ntdll_base_ = base;
    UT_INFO("ntdll at %#llx", (unsigned long long)ntdll_base_);
    ki_user_apc_dispatcher = FindExport(ntdll_base_, "KiUserApcDispatcher");
    ki_user_exception_dispatcher = FindExport(ntdll_base_, "KiUserExceptionDispatcher");
    return true;
}

void Kernel::BuildKusd() {
    // KUSER_SHARED_DATA lives at a fixed 0x7ffe0000 on Windows, which this host cannot map.
    // It goes right after ntdll instead, and the few instructions that name the fixed
    // address are rewritten (PatchKusdReferences) to reach it relative to themselves.
    uint64_t want = AlignUp(ntdll_base_ + ntdll_image_->size_of_image, kAllocGranularity);
    uint64_t size = 0x10000;
    NTSTATUS rc = vm_->Allocate(&want, &size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (rc != STATUS_SUCCESS) {
        UT_ERROR("cannot place KUSER_SHARED_DATA after ntdll: %#x", (uint32_t)rc);
        return;
    }
    kusd_ = want;
    auto* k = reinterpret_cast<uint8_t*>(kusd_);
    PutU32(kusd_, 0x0, 0);                       // TickCountLowDeprecated
    PutU32(kusd_, 0x4, 0x0FA00000);              // TickCountMultiplier (15.625ms)
    PutU16(kusd_, 0x2c, 0x8664);                 // ImageNumberLow
    PutU16(kusd_, 0x2e, 0x8664);                 // ImageNumberHigh
    const char16_t root[] = u"C:\\Windows";
    std::memcpy(k + 0x30, root, sizeof(root));   // NtSystemRoot
    PutU32(kusd_, 0x238, 0);                     // MaxStackTraceDepth
    PutU32(kusd_, 0x26c, 6);                     // NtMajorVersion
    PutU32(kusd_, 0x270, 2);                     // NtMinorVersion
    PutU32(kusd_, 0x264, 1);                     // NtProductType: workstation
    k[0x268] = 1;                                // ProductTypeIsValid
    PutU32(kusd_, 0x2d4, 0);                     // SafeBootMode
    PutU32(kusd_, 0x2e8, 0x200000);              // NumberOfPhysicalPages (8 GB / 4K)
    k[0x2ec] = 0;                                // KdDebuggerEnabled
    PutU32(kusd_, 0x2f0, nprocs_);               // ActiveProcessorCount
    // QpcFrequency. The console OS from 6.2.13xxx on reads the counter itself (rdtscp, which FEXCore answers with the host's virtual
    // counter) and takes only the frequency from here, so then it has to be the host counter's; before that the counter is a system
    // call that this runtime keeps at 10 MHz.
    uint64_t qpc_frequency = 10000000;
    {
        const uint64_t qpc = FindExport(ntdll_base_, "RtlQueryPerformanceCounter");
        if (qpc != 0) {
            const auto* code = reinterpret_cast<const uint8_t*>(qpc);
            for (int i = 0; i < 12; ++i) {
                if (code[i] == 0x0F && code[i + 1] == 0x01 && code[i + 2] == 0xF9) {   // rdtscp
#if defined(__aarch64__)
                    uint64_t f;
                    asm volatile("mrs %0, cntfrq_el0" : "=r"(f));
                    qpc_frequency = f;
#endif
                    break;
                }
            }
        }
    }
    PutU64(kusd_, 0x300, qpc_frequency);         // QpcFrequency
    k[0x274 + 10] = 1;                           // PF_XMMI64_INSTRUCTIONS_AVAILABLE etc. (index 10)
    k[0x274 + 6] = 1;                            // PF_XMMI_INSTRUCTIONS_AVAILABLE
    k[0x274 + 7] = 1;                            // PF_3DNOW (benign)
    k[0x274 + 13] = 1;                           // PF_SSE3_INSTRUCTIONS_AVAILABLE
    k[0x274 + 17] = 1;                           // PF_SSSE3
    k[0x274 + 18] = 1;                           // PF_SSE4_1
    k[0x274 + 20] = 1;                           // PF_SSE4_2
    k[0x274 + 12] = 1;                           // PF_NX_ENABLED
    k[0x274 + 23] = 1;                           // PF_FASTFAIL_AVAILABLE
    // Clock values; refreshed by the timer thread.
    const int64_t now = NtSystemTime();
    PutU32(kusd_, 0x14, static_cast<uint32_t>(now & 0xFFFFFFFF));
    PutU32(kusd_, 0x18, static_cast<uint32_t>(now >> 32));
    PutU32(kusd_, 0x1c, static_cast<uint32_t>(now >> 32));
}

// UTSURO_TP="image+0xRVA:len,..." patches the function at that address to log every call
// (arguments and caller) before running its displaced first `len` bytes, which must be whole
// instructions with no relative operands or relocations. "ntdll" is patched at boot, other
// images when they are mapped.
void Kernel::InstallTracepointsFor(const std::string& image, uint64_t base) {
    if (kusd_ == 0) return;
    if (tp_stub_next_ == 0) tp_stub_next_ = kusd_ + 0xA000;
    std::vector<std::string> specs;
    if (image == "ntdll.dll") {
        char b[64];
        // The export is a thunk (sub rsp / ... / call <real function> / add rsp / ret); the real function's start differs between OS builds.
        const uint64_t thunk = FindExport(ntdll_base_, "vDbgPrintExWithPrefix");
        uint64_t real = thunk + 0x20;
        if (*reinterpret_cast<const uint8_t*>(thunk + 0x13) == 0xE8) real = thunk + 0x18 + *reinterpret_cast<const int32_t*>(thunk + 0x14);
        std::snprintf(b, sizeof b, "ntdll.dll+%llx:6", (unsigned long long)(real - ntdll_base_));
        specs.push_back(b);  // always on: ntdll's own diagnostics (DbgPrint)
    }
    if (const char* user = std::getenv("UTSURO_TP")) {
        std::string s = user;
        size_t pos = 0;
        while (pos < s.size()) {
            size_t end = s.find(',', pos);
            if (end == std::string::npos) end = s.size();
            specs.push_back(s.substr(pos, end - pos));
            pos = end + 1;
        }
    }
    bool any = false;
    for (const auto& item : specs) {
        const bool want_return = !item.empty() && item.back() == '!';
        const auto plus = item.find('+');
        const auto colon = item.find(':');
        if (plus == std::string::npos || colon == std::string::npos) continue;
        std::string want = item.substr(0, plus);
        for (auto& c : want) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (want == "ntdll") want = "ntdll.dll";
        if (want != image) continue;
        const uint64_t rva = std::strtoull(item.substr(plus + 1, colon - plus - 1).c_str(), nullptr, 16);
        const unsigned len = static_cast<unsigned>(std::atoi(item.substr(colon + 1).c_str()));
        if (len < 5 || len > 16) continue;
        const uint64_t at = base + rva;
        auto* fn = reinterpret_cast<uint8_t*>(at);
        auto* stub = reinterpret_cast<uint8_t*>(tp_stub_next_);
        const int id = static_cast<int>(tracepoints.size());
        size_t n = 0;
        auto put = [&](std::initializer_list<uint8_t> bytes) { for (uint8_t b : bytes) stub[n++] = b; };
        put({0x50, 0x51, 0x41, 0x53, 0x41, 0x52});            // push rax, rcx, r11, r10
        put({0x4C, 0x8B, 0xD1});                              // mov r10, rcx
        put({0xB8});
        const uint32_t number = 0xFFFF0000u + static_cast<uint32_t>(id);
        std::memcpy(stub + n, &number, 4); n += 4;            // mov eax, number
        put({0x0F, 0x05});                                    // syscall
        put({0x41, 0x5A, 0x41, 0x5B, 0x59, 0x58});            // pop r10, r11, rcx, rax
        std::memcpy(stub + n, fn, len); n += len;             // the displaced instructions
        stub[n++] = 0xE9;
        const int32_t back = static_cast<int32_t>((at + len) - (tp_stub_next_ + n + 4));
        std::memcpy(stub + n, &back, 4); n += 4;
        fn[0] = 0xE9;
        const int32_t fwd = static_cast<int32_t>(tp_stub_next_ - (at + 5));
        std::memcpy(fn + 1, &fwd, 4);
        for (unsigned k2 = 5; k2 < len; ++k2) fn[k2] = 0x90;
        tracepoints.push_back({item.substr(0, colon), at, want_return});
        UT_INFO("tracepoint %d at %s", id, item.substr(0, colon).c_str());
        tp_stub_next_ += (n + 15) & ~15ull;
        if (want_return && tp_return_stub_ == 0) {
            auto* rs = reinterpret_cast<uint8_t*>(tp_stub_next_);
            size_t m = 0;
            rs[m++] = 0x4C; rs[m++] = 0x8B; rs[m++] = 0xD0;            // mov r10, rax
            rs[m++] = 0xB8;
            const uint32_t rn = 0xFFFF7001u;
            std::memcpy(rs + m, &rn, 4); m += 4;                        // mov eax, 0xFFFF7001
            rs[m++] = 0x0F; rs[m++] = 0x05;                            // syscall
            rs[m++] = 0xCC;
            tp_return_stub_ = tp_stub_next_;
            tp_stub_next_ += 16;
        }
        any = true;
    }
    if (any) {
        uint64_t sb = kusd_ + 0x1000, ss = 0xF000;
        vm_->Protect(&sb, &ss, PAGE_EXECUTE_READ, nullptr);
        space_->NotifyContentsReplaced(base, base + 0x1000000);
        space_->NotifyContentsReplaced(kusd_ + 0x1000, kusd_ + 0x10000);
    }
}

uint64_t Kernel::BuildProcessParameters() {
    const std::u16string image = Utf8ToUtf16("G:\\" + options_.exe_name);
    const std::u16string cwd = Utf8ToUtf16(options_.cwd_nt);
    const std::u16string dll_path = u"C:\\Windows\\System32;C:\\Windows";
    const std::u16string cmdline = Utf8ToUtf16(options_.command_line.empty() ? "\"G:\\" + options_.exe_name + "\"" : options_.command_line);
    const std::u16string title = image;
    const std::u16string desktop = u"";
    std::u16string env;
    auto add = [&](const char* kv) { env += Utf8ToUtf16(kv); env.push_back(u'\0'); };
    add("SystemRoot=C:\\Windows");
    add("SystemDrive=C:");
    add("windir=C:\\Windows");
    add("PATH=C:\\Windows\\System32;C:\\Windows");
    add("TEMP=C:\\Users\\Default\\AppData\\Local\\Temp");
    add("TMP=C:\\Users\\Default\\AppData\\Local\\Temp");
    add("USERPROFILE=C:\\Users\\Default");
    add("NUMBER_OF_PROCESSORS=8");
    add("PROCESSOR_ARCHITECTURE=AMD64");
    env.push_back(u'\0');

    constexpr size_t kHeader = 0x410;
    auto sz = [](const std::u16string& s) { return (s.size() + 1) * 2; };
    const size_t total = AlignUp(kHeader + sz(image) + sz(cwd) + sz(dll_path) + sz(cmdline) + sz(title) + sz(desktop) + env.size() * 2 + 64, 16);
    uint64_t base = 0, size = total;
    if (vm_->Allocate(&base, &size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE) != STATUS_SUCCESS) return 0;
    std::memset(reinterpret_cast<void*>(base), 0, size);
    uint64_t cursor = base + kHeader;
    auto place = [&](const std::u16string& s, size_t field) -> uint64_t {
        const uint64_t at = cursor;
        std::memcpy(reinterpret_cast<void*>(at), s.c_str(), s.size() * 2 + 2);
        cursor += sz(s);
        if (field != 0) {
            PutU16(base, field, static_cast<uint16_t>(s.size() * 2));
            PutU16(base, field + 2, static_cast<uint16_t>(s.size() * 2 + 2));
            PutU64(base, field + 8, at);
        }
        return at;
    };
    PutU32(base, 0x0, static_cast<uint32_t>(total));        // MaximumLength
    PutU32(base, 0x4, static_cast<uint32_t>(total));        // Length
    PutU32(base, 0x8, 1);                                   // Flags: PROCESS_PARAMETERS_NORMALIZED
    place(cwd, 0x38);
    PutU64(base, 0x48, 0);                                  // CurrentDirectory.Handle
    place(dll_path, 0x50);
    place(image, 0x60);
    place(cmdline, 0x70);
    place(title, 0xb0);
    place(desktop, 0xc0);
    const uint64_t env_at = cursor;
    std::memcpy(reinterpret_cast<void*>(env_at), env.data(), env.size() * 2);
    cursor += env.size() * 2;
    PutU64(base, 0x80, env_at);
    PutU64(base, 0x3f0, env.size() * 2);                    // EnvironmentSize
    PutU32(base, 0x3f8, 0);                                 // EnvironmentVersion
    return base;
}

bool Kernel::BuildProcess(std::string& error) {
    // The executable.
    const std::string exe_path = options_.title_root + "/" + options_.exe_name;
    std::string e;
    auto exe = LoadPe(exe_path, e);
    if (!exe) { error = exe_path + ": " + e; return false; }
    exe->nt_path = "G:\\" + options_.exe_name;
    SectionObject section;
    section.image = std::move(exe);
    uint64_t base = 0, size = 0;
    const NTSTATUS rc = MapImageSection(section, &base, &size, false);  // ntdll relocates the executable itself
    exe_image_ = std::move(section.image);
    if (rc != STATUS_SUCCESS && rc != STATUS_IMAGE_NOT_AT_BASE) { error = "cannot map the executable"; return false; }
    exe_base_ = base;
    UT_INFO("%s at %#llx (preferred %#llx), entry %#llx", options_.exe_name.c_str(), (unsigned long long)exe_base_,
            (unsigned long long)exe_image_->image_base, (unsigned long long)(exe_base_ + exe_image_->entry_rva));

    // PEB.
    uint64_t peb_size = peb::kSize;
    peb_ = 0;
    if (vm_->Allocate(&peb_, &peb_size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE) != STATUS_SUCCESS) { error = "peb"; return false; }
    std::memset(reinterpret_cast<void*>(peb_), 0, peb_size);
    auto* pb = reinterpret_cast<uint8_t*>(peb_);
    pb[peb::kBitField] = 0x4;  // IsLegacyProcess? no: ImageUsesLargePages=0, IsProtectedProcess=0, IsImageDynamicallyRelocated=1 (bit 2)
    PutU64(peb_, peb::kImageBaseAddress, exe_base_);
    PutU64(peb_, 0x8, ~0ull);  // Mutant
    PutU32(peb_, peb::kNumberOfProcessors, nprocs_);
    PutU32(peb_, peb::kNtGlobalFlag, 0);
    PutU64(peb_, peb::kCriticalSectionTimeout, static_cast<uint64_t>(-30ll * 24 * 3600 * 10000000ll));
    PutU64(peb_, peb::kHeapSegmentReserve, exe_image_->heap_reserve ? exe_image_->heap_reserve : 0x100000);
    PutU64(peb_, peb::kHeapSegmentCommit, exe_image_->heap_commit ? exe_image_->heap_commit : 0x2000);
    PutU64(peb_, peb::kHeapDeCommitTotalFreeThreshold, 0x10000);
    PutU64(peb_, peb::kHeapDeCommitFreeBlockThreshold, 0x1000);
    PutU32(peb_, peb::kOSMajorVersion, 6);
    PutU32(peb_, peb::kOSMinorVersion, 2);
    PutU16(peb_, peb::kOSBuildNumber, 9200);
    PutU16(peb_, peb::kOSCSDVersion, 0);
    PutU32(peb_, peb::kOSPlatformId, 2);
    PutU32(peb_, peb::kImageSubsystem, exe_image_->subsystem);
    PutU32(peb_, peb::kImageSubsystemMajorVersion, exe_image_->major_subsystem);
    PutU32(peb_, peb::kImageSubsystemMinorVersion, exe_image_->minor_subsystem);
    PutU64(peb_, peb::kActiveProcessAffinityMask, (1ull << nprocs_) - 1);
    PutU32(peb_, peb::kSessionId, 1);
    // National language tables: AnsiCodePageData, OemCodePageData, UnicodeCaseTableData.
    uint64_t ansi = 0, oem = 0, casemap = 0;
    if (!BuildNlsTables(*vm_, &ansi, &oem, &casemap)) { error = "nls tables"; return false; }
    PutU64(peb_, 0xA0, ansi);
    PutU64(peb_, 0xA8, oem);
    PutU64(peb_, 0xB0, casemap);
    // The process heap table and its lock are ntdll's to create.
    PutU64(peb_, peb::kProcessParameters, BuildProcessParameters());

    // The first thread.
    const uint64_t stack = std::max<uint64_t>(exe_image_->stack_reserve, 0x800000);
    auto thread = NewThread(stack, exe_image_->stack_commit, e);
    if (!thread) { error = "main thread: " + e; return false; }
    const uint64_t entry = exe_base_ + exe_image_->entry_rva;
    const uint64_t start = FindExport(ntdll_base_, "RtlUserThreadStart");
    StartThread(thread, start, entry, peb_, false);
    return true;
}

// ---------------------------------------------------------------------------------------
// Threads
// ---------------------------------------------------------------------------------------

std::shared_ptr<NtThread> Kernel::NewThread(uint64_t stack_reserve, uint64_t stack_commit, std::string& error) {
    uint64_t tid;
    {
        std::scoped_lock lock {thread_mutex_};
        tid = next_tid_;
        next_tid_ += 4;
    }
    auto t = std::make_shared<NtThread>(*this, tid);
    // Stack.
    stack_reserve = AlignUp(std::max<uint64_t>(stack_reserve, 0x40000), 0x10000);
    uint64_t sbase = 0, ssize = stack_reserve;
    if (vm_->Allocate(&sbase, &ssize, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE) != STATUS_SUCCESS) { error = "stack"; return nullptr; }
    // TEB.
    uint64_t tbase = 0, tsize = teb::kSize;
    if (vm_->Allocate(&tbase, &tsize, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE) != STATUS_SUCCESS) { error = "teb"; return nullptr; }
    std::memset(reinterpret_cast<void*>(tbase), 0, tsize);
    t->teb = tbase;
    t->stack_alloc_base = sbase;
    t->stack_base = sbase + ssize;
    t->stack_limit = sbase;
    PutU64(tbase, teb::kStackBase, t->stack_base);
    PutU64(tbase, teb::kStackLimit, t->stack_limit);
    PutU64(tbase, teb::kSelf, tbase);
    PutU64(tbase, teb::kClientIdProcess, 0x4);
    PutU64(tbase, teb::kClientIdThread, tid);
    PutU64(tbase, teb::kProcessEnvironmentBlock, peb_);
    PutU64(tbase, teb::kDeallocationStack, sbase);
    PutU64(tbase, teb::kExceptionList, ~0ull);
    // ActivationContextStackPointer points at an empty stack kept inside the TEB.
    PutU64(tbase, 0x2c8, tbase + 0x1c00);
    PutU64(tbase, 0x1c00 + 0x20, tbase + 0x1c00 + 0x20);  // FrameListCache.Flink
    PutU64(tbase, 0x1c00 + 0x28, tbase + 0x1c00 + 0x20);  // FrameListCache.Blink
    {
        std::scoped_lock lock {thread_mutex_};
        threads_[tid] = t;
    }
    return t;
}

namespace {

void* HostThreadMain(void* raw) {
    std::unique_ptr<StartData> sd {static_cast<StartData*>(raw)};
    Kernel& k = *sd->kernel;
    auto& t = *sd->thread;
    g_self = &t;
    pthread_setname_np(("guest-" + std::to_string(t.tid_)).c_str());
    {
        // The first thread runs the game's main loop and presents the frames: it keeps the top class. Everything else starts a step
        // below it and is moved by what the game says about it (NtSetInformationThread, ThreadBasePriority).
        static std::atomic<bool> first {true};
        if (first.exchange(false)) { t.base_priority = 9; }
        t.ApplyHostPriority();
    }
    std::string err;
    t.guest = k.Engine().StartThread(sd->rip, sd->rsp, t, err);
    if (!t.guest) {
        UT_ERROR("thread %llu: cannot start guest: %s", (unsigned long long)t.tid_, err.c_str());
        return nullptr;
    }
    t.control = t.guest.get();
    GuestCpuState st;
    t.guest->CaptureCpuState(&st);
    st.gregs[1] = sd->rcx;       // rcx = CONTEXT*
    st.gregs[2] = k.NtdllBase(); // rdx
    st.mxcsr = 0x1f80;
    t.guest->RestoreCpuState(st);
    t.guest->SetGsBase(sd->gs);
    // A thread created suspended waits to be resumed.
    {
        std::unique_lock lock {k.WaitMutex()};
        k.WaitCv().wait(lock, [&] { return t.suspend_count.load() <= 0 || k.exiting.load(); });
    }
    const RunResult r = t.guest->Run();
    UT_INFO("thread %llu ended: %s (status %d, rip %#llx)", (unsigned long long)t.tid_, r.message.c_str(), r.status,
            (unsigned long long)r.rip);
    if (std::getenv("UTSURO_HIST") != nullptr) {
        const size_t n = std::min(t.history_count, t.history.size());
        for (size_t i = 0; i < n; ++i) {
            const auto& h = t.history[(t.history_count - n + i) % t.history.size()];
            UT_INFO("thread %llu history: %s <- %s", (unsigned long long)t.tid_, k.SyscallName(h.first), k.Describe(h.second).c_str());
        }
    }
    if (r.outcome != RunOutcome::Exited) {
        // Debugging aid: UTSURO_HEXDUMP=<reg>[+off]:<len>[,...] dumps guest memory around a register of the fault.
        if (const char* spec = std::getenv("UTSURO_HEXDUMP")) {
            uint64_t g[16], rip;
            if (LastFaultRegisters(g, &rip)) {
                static const char* const names[16] = {"rax", "rcx", "rdx", "rbx", "rsp", "rbp", "rsi", "rdi", "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15"};
                std::string list = spec;
                size_t pos = 0;
                while (pos < list.size()) {
                    size_t end = list.find(',', pos);
                    if (end == std::string::npos) end = list.size();
                    const std::string item = list.substr(pos, end - pos);
                    pos = end + 1;
                    const size_t colon = item.find(':');
                    if (colon == std::string::npos) continue;
                    std::string reg = item.substr(0, colon);
                    long long off = 0;
                    const size_t plus = reg.find_first_of("+-");
                    if (plus != std::string::npos) { off = std::strtoll(reg.c_str() + plus, nullptr, 0); reg = reg.substr(0, plus); }
                    const size_t len = std::strtoull(item.c_str() + colon + 1, nullptr, 0);
                    if (reg == "exe") {   // the title image's base, for globals
                        const uint64_t at = k.ImageBase() + off;
                        if (!k.Vm().Contains(at, len)) { UT_INFO("hexdump exe%+lld: not mapped", off); continue; }
                        std::string line;
                        char b[8];
                        for (size_t q = 0; q < len && q < 64; ++q) { std::snprintf(b, sizeof b, "%02x", reinterpret_cast<const uint8_t*>(at)[q]); line += b; if (q % 8 == 7) line += ' '; }
                        UT_INFO("hexdump exe%+lld: %s", off, line.c_str());
                        continue;
                    }
                    for (int i = 0; i < 16; ++i) {
                        if (reg != names[i]) continue;
                        const uint64_t at = g[i] + off;
                        if (!k.Vm().Contains(at, len)) { UT_INFO("hexdump %s%+lld: not mapped", reg.c_str(), off); break; }
                        for (size_t o = 0; o < len; o += 32) {
                            std::string line;
                            char b[8];
                            for (size_t q = 0; q < 32 && o + q < len; ++q) { std::snprintf(b, sizeof b, "%02x", reinterpret_cast<const uint8_t*>(at)[o + q]); line += b; if (q % 8 == 7) line += ' '; }
                            UT_INFO("hexdump %s%+lld +%#zx: %s", reg.c_str(), off, o, line.c_str());
                        }
                    }
                }
            }
        }
        // Debugging aid: UTSURO_SCAN=<text> prints where that text (ASCII or UTF-16) sits in the guest's heap.
        if (const char* needle = std::getenv("UTSURO_SCAN")) {
            const std::string a = needle;
            std::string w;
            for (char c : a) { w += c; w += '\0'; }
            int shown = 0;
            k.Vm().ForEachReadable([&](uint64_t base, const uint8_t* mem, uint64_t len) {
                for (const std::string* pat : {static_cast<const std::string*>(&a), static_cast<const std::string*>(&w)}) {
                    const uint8_t* at = mem;
                    const uint8_t* end = mem + len;
                    while (shown < 12 && (at = static_cast<const uint8_t*>(memmem(at, static_cast<size_t>(end - at), pat->data(), pat->size()))) != nullptr) {
                        std::string ctx;
                        const bool wide = pat == &w;
                        for (int i = -40; i < 240; ++i) {
                            const uint8_t* q = at + i * (wide ? 2 : 1);
                            if (q < mem || q + 1 > end) continue;
                            const uint8_t c = *q;
                            ctx += (c >= 32 && c < 127) ? static_cast<char>(c) : (c == 10 || c == 13) ? '|' : '.';
                        }
                        UT_INFO("scan: %s at %#llx: %s", wide ? "utf16" : "ascii", (unsigned long long)(base + (at - mem)), ctx.c_str());
                        ++shown;
                        at += pat->size();
                    }
                }
            });
        }
    }
    k.ExitThread(t, r.outcome == RunOutcome::Exited ? t.exit_status : -1);
    return nullptr;
}

} // namespace

void Kernel::StartThread(std::shared_ptr<NtThread> t, uint64_t context_rip, uint64_t arg0, uint64_t arg1, bool suspended) {
    // The CONTEXT the thread will "continue" into once ntdll has initialised it, placed at
    // the top of its stack the way the kernel does for a new thread.
    const uint64_t ctx_addr = AlignDown(t->stack_base - sizeof(CONTEXT) - 0x200, 16);
    auto* ctx = reinterpret_cast<CONTEXT*>(ctx_addr);
    std::memset(ctx, 0, sizeof(CONTEXT));
    ctx->ContextFlags = CONTEXT_FULL | CONTEXT_SEGMENTS;
    ctx->MxCsr = 0x1f80;
    ctx->SegCs = 0x33;
    ctx->SegDs = ctx->SegEs = ctx->SegSs = 0x2b;
    ctx->SegGs = 0x2b;
    ctx->SegFs = 0x53;
    ctx->EFlags = 0x200;
    ctx->ControlWord = 0x27f;
    ctx->MxCsr2 = 0x1f80;
    ctx->MxCsr_Mask = 0xffff;
    ctx->Rip = context_rip;
    ctx->Rcx = arg0;
    ctx->Rdx = arg1;
    ctx->Rsp = ctx_addr - 0x28;  // (rsp + 8) is 16-byte aligned at function entry
    t->start_rip = context_rip;
    t->start_arg = arg0;
    t->suspend_count = suspended ? 1 : 0;

    auto* sd = new StartData {this, t, FindExport(ntdll_base_, "LdrInitializeThunk"), ctx_addr - 0x48, ctx_addr, t->teb};
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 16 << 20);
    pthread_t pt;
    if (pthread_create(&pt, &attr, HostThreadMain, sd) != 0) {
        UT_ERROR("pthread_create failed");
        delete sd;
        return;
    }
    pthread_detach(pt);
    pthread_attr_destroy(&attr);
}

void Kernel::ExitThread(NtThread& thread, int32_t status) {
    {
        std::scoped_lock lock {wait_mutex_};
        thread.exit_status = status;
        thread.terminated = true;
        thread.signaled = true;
    }
    wait_cv_.notify_all();
    // Release the stack and TEB, except for the one thread running on the stack we would
    // be freeing; it has already left the guest.
    uint64_t b = thread.stack_alloc_base, s = 0;
    vm_->Free(&b, &s, MEM_RELEASE);
    b = thread.teb; s = 0;
    vm_->Free(&b, &s, MEM_RELEASE);
}

void Kernel::ExitProcess(int32_t status) {
    exiting = true;
    exit_code = status;
    std::fflush(stdout);
    std::fflush(stderr);
    UT_INFO("process exit, status %d", status);
    _exit(status);
}

int Kernel::Run() {
    // The main thread has nothing to do but wait; the process ends in ExitProcess.
    // Debugging aid: UTSURO_TLSWATCH=<tid>:<offset> logs every change of the qword at that offset in the thread's implicit TLS block.
    // Debugging aid: UTSURO_GLOBALWATCH=<rva>:<offset> logs every change of the qword at [image base + rva] + offset (a pointer to an object, e.g. the RHI).
    std::thread global_watch;
    if (const char* spec = std::getenv("UTSURO_GLOBALWATCH")) {
        const uint64_t rva = std::strtoull(spec, nullptr, 16);
        const char* colon = std::strchr(spec, ':');
        const uint64_t offset = colon != nullptr ? std::strtoull(colon + 1, nullptr, 16) : 0;
        global_watch = std::thread([this, rva, offset] {
            uint64_t last = ~0ull, last_object = 0;
            while (true) {
                {
                    std::scoped_lock el {exit_mutex_};
                    if (process_done_) break;
                }
                if (exe_base_ != 0 && vm_->Contains(exe_base_ + rva, 8)) {
                    const uint64_t object = *reinterpret_cast<volatile uint64_t*>(exe_base_ + rva);
                    if (object != last_object) { UT_INFO("globalwatch: global is now %#llx", (unsigned long long)object); last_object = object; last = ~0ull; }
                    if (object != 0 && vm_->Contains(object + offset, 8)) {
                        const uint64_t now = *reinterpret_cast<volatile uint64_t*>(object + offset);
                        if (now != last) { UT_INFO("globalwatch: [%#llx+%#llx] %#llx -> %#llx", (unsigned long long)object, (unsigned long long)offset, (unsigned long long)(last == ~0ull ? 0 : last), (unsigned long long)now); last = now; }
                    }
                }
                std::this_thread::sleep_for(std::chrono::microseconds(20));
            }
        });
    }
    std::thread tls_watch;
    if (const char* spec = std::getenv("UTSURO_TLSWATCH")) {
        const uint64_t want_tid = std::strtoull(spec, nullptr, 0);
        const char* colon = std::strchr(spec, ':');
        const uint64_t offset = colon != nullptr ? std::strtoull(colon + 1, nullptr, 16) : 8;
        tls_watch = std::thread([this, want_tid, offset] {
            uint64_t last = ~0ull;
            while (true) {
                {
                    std::scoped_lock el {exit_mutex_};
                    if (process_done_) break;
                }
                std::shared_ptr<NtThread> t;
                {
                    std::scoped_lock tl {thread_mutex_};
                    auto it = threads_.find(want_tid);
                    if (it != threads_.end()) t = it->second;
                }
                if (t != nullptr && t->teb != 0) {
                    const uint64_t array_ptr = *reinterpret_cast<volatile uint64_t*>(t->teb + 0x58);
                    if (array_ptr != 0 && vm_->Contains(array_ptr, 8)) {
                        const uint64_t block = *reinterpret_cast<volatile uint64_t*>(array_ptr);
                        if (block != 0 && vm_->Contains(block + offset, 8)) {
                            const uint64_t now = *reinterpret_cast<volatile uint64_t*>(block + offset);
                            if (now != last) {
                                UT_INFO("tlswatch: thread %llu [%#llx] %#llx -> %#llx (last syscall %s %#llx at %s)", (unsigned long long)want_tid, (unsigned long long)offset,
                                        (unsigned long long)(last == ~0ull ? 0 : last), (unsigned long long)now, (t->current_syscall >= winrt::Runtime::kBase ? winrt::Runtime::Get().NameOf(static_cast<uint32_t>(t->current_syscall)) : std::string(SyscallName(static_cast<uint32_t>(t->current_syscall)))).c_str(), (unsigned long long)t->current_syscall, Describe(t->current_rip).c_str());
                                last = now;
                            }
                        }
                    }
                }
                std::this_thread::sleep_for(std::chrono::microseconds(20));
            }
        });
    }
    std::unique_lock lock {exit_mutex_};
    // Debugging aid: UTSURO_STACKS=<seconds> prints, at that interval, what every guest thread is blocked in.
    if (const char* every = std::getenv("UTSURO_STACKS")) {
        const auto period = std::chrono::seconds(std::max(1, std::atoi(every)));
        while (!exit_cv_.wait_for(lock, period, [&] { return process_done_; })) {
            std::vector<std::shared_ptr<NtThread>> list;
            {
                std::scoped_lock tl {thread_mutex_};
                for (auto& [id, t] : threads_) list.push_back(t);
            }
            UT_INFO("---- threads ----");
            for (auto& t : list) {
                if (t->current_args != nullptr)
                {
                    UT_INFO("thread %llu %s: in %s(%#llx) from %s (syscall site %s)", (unsigned long long)t->tid_, t->name.c_str(), SyscallName(static_cast<uint32_t>(t->current_syscall)),
                            (unsigned long long)(t->current_args ? t->current_args->reg[0] : 0), Describe(t->current_ret).c_str(), Describe(t->current_rip).c_str());
                    const auto frames = Backtrace(t->stack_rip, t->stack_regs, 14);
                    for (size_t i = 0; i < frames.size(); ++i) UT_INFO("    #%zu %s", i, Describe(frames[i]).c_str());
                }
                else
                {
                    UT_INFO("thread %llu %s: running (last syscall %s at %s)", (unsigned long long)t->tid_, t->name.c_str(), SyscallName(static_cast<uint32_t>(t->current_syscall)),
                            Describe(t->current_rip).c_str());
                    if (std::getenv("UTSURO_STACKS_RUNNING") != nullptr && t->control != nullptr) {   // sampled with a signal: where it is right now
                        uint64_t rip, regs[16];
                        if (t->control->SampleRunning(&rip, regs)) {
                            UT_INFO("    sampled at %s, rsp %#llx", Describe(rip).c_str(), (unsigned long long)regs[4]);
                            const auto frames = Backtrace(rip, regs, 14);
                            for (size_t i = 0; i < frames.size(); ++i) UT_INFO("    #%zu %s", i, Describe(frames[i]).c_str());
                        }
                    }
                }
            }
        }
    }
    exit_cv_.wait(lock, [&] { return process_done_; });
    if (tls_watch.joinable()) tls_watch.detach();
    if (global_watch.joinable()) global_watch.detach();
    return exit_code.load();
}

} // namespace utsuro::nt
