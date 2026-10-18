// process.cpp -- bringing a process into existence: ntdll, the executable, the PEB, the
// first thread; and the thread machinery shared with NtCreateThreadEx.
#include "nt/kernel.h"

#include "engine/log.h"

#include <pthread.h>
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
    GuestAddressSpace* space = GuestAddressSpace::Reserve(options_.arena_size, e, 0x10000);
    if (space == nullptr) { error = e; return false; }
    space_.reset(space);
    EngineOptions eo;
    eo.tso_enabled = true;
    engine_ = FexEngine::Create(*space_, eo, e);
    if (!engine_) { error = "engine: " + e; return false; }
    vm_ = std::make_unique<VirtualMemory>(*space_, space_->Base(), space_->Size());

    if (!PrepareNtdll(error)) return false;
    RegisterSyscalls();
    BuildKusd();
    PatchKusdReferences();
    timer_thread_ = std::thread([this] { TimerThreadMain(); });
    if (const char* wd = std::getenv("UTSURO_WATCHDOG")) {
        const int ms = std::max(100, std::atoi(wd) * 1000);
        std::thread([this, ms] {
            pthread_setname_np("utsuro-watchdog");
            for (;;) {
                std::this_thread::sleep_for(std::chrono::milliseconds(ms));
                DumpGuestThreads([this](uint64_t a) { return Describe(a); }, [this](uint64_t rip, const uint64_t* regs) {
                    const uint64_t rsp = regs[4];
                    if (Describe(rip).find("RtlEqualUnicodeString") != std::string::npos) {
                        auto s = [](uint64_t p) { return p > 0x10000 ? Utf16ToUtf8(ReadUnicodeString(reinterpret_cast<const UNICODE_STRING*>(p))) : std::string("?"); };
                        UT_WARN("    RtlEqualUnicodeString(\"%s\", \"%s\")", s(regs[1]).c_str(), s(regs[2]).c_str());
                    }
                    // A poor man's backtrace: stack words that point into an image.
                    int shown = 0;
                    for (uint64_t i = 0; i < 700 && shown < 60; ++i) {
                        const uint64_t v = *reinterpret_cast<uint64_t*>(rsp + i * 8);
                        if (v < 0x1000000000ull) continue;
                        const std::string d = Describe(v);
                        if (d.find('+') == std::string::npos || d[0] == '0') continue;
                        UT_WARN("    [rsp+%#llx] %s", (unsigned long long)(i * 8), d.c_str());
                        ++shown;
                    }
                    (void)rip;
                });
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
    const NTSTATUS rc = MapImageSection(section, &base, &size);
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
    PutU64(kusd_, 0x300, 10000000);              // QpcFrequency (10 MHz)
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

void Kernel::PatchKusdReferences() {
    if (kusd_ == 0) return;
    // KUSER_SHARED_DATA is at 0x7ffe0000 on Windows and this host cannot map anything
    // below 4 GB, so the ntdll instructions that name it are rewritten to use the copy
    // that sits beside ntdll. The forms found in a 64-bit ntdll:
    //   [rex] op modrm sib=0x25 disp32      (absolute operand)  -> rip-relative, nop in front
    //   mov r32, imm32 / mov r64, simm32    (address as a value) -> jmp to a stub that loads
    //                                                              the full 64-bit address
    //   movabs r64, imm64                                         -> immediate widened in place
    int sib_patched = 0, stub_patched = 0, abs_patched = 0, unknown = 0;
    uint64_t stub_area = kusd_ + 0x1000;
    uint64_t stub_next = stub_area;
    auto make_stub = [&](int reg, uint64_t value, uint64_t return_to) -> uint64_t {
        auto* p = reinterpret_cast<uint8_t*>(stub_next);
        p[0] = static_cast<uint8_t>(0x48 | (reg >> 3));
        p[1] = static_cast<uint8_t>(0xB8 + (reg & 7));
        std::memcpy(p + 2, &value, 8);
        p[10] = 0xE9;
        const int32_t rel = static_cast<int32_t>(return_to - (stub_next + 15));
        std::memcpy(p + 11, &rel, 4);
        const uint64_t at = stub_next;
        stub_next += 16;
        return at;
    };
    for (const auto& s : ntdll_image_->sections) {
        if ((s.characteristics & 0x20000000) == 0) continue;
        auto* code = reinterpret_cast<uint8_t*>(ntdll_base_ + s.rva);
        const uint32_t len = std::max(s.vsize, s.raw_size);
        for (uint32_t i = 8; i + 8 < len; ++i) {
            if (code[i + 2] != 0xFE || code[i + 3] != 0x7F || code[i + 1] > 0x0F) continue;
            const uint32_t offset = (static_cast<uint32_t>(code[i + 1]) << 8) | code[i];
            const uint64_t target = kusd_ + offset;
            // movabs r64, imm64 with the high half zero.
            if (code[i + 4] == 0 && code[i + 5] == 0 && code[i + 6] == 0 && code[i + 7] == 0 &&
                (code[i - 2] & 0xFE) == 0x48 && code[i - 1] >= 0xB8 && code[i - 1] <= 0xBF) {
                std::memcpy(code + i, &target, 8);
                ++abs_patched;
                continue;
            }
            // Absolute operand through SIB.
            if (i >= 4 && code[i - 1] == 0x25 && (code[i - 2] & 0xC7) == 0x04) {
                const uint32_t sib = i - 1;
                // Layout: [prefixes][rex] opcode(1-2 bytes) modrm sib disp32, modrm at sib-1.
                uint32_t first = (code[sib - 3] == 0x0F) ? sib - 3 : sib - 2;
                if ((code[first - 1] & 0xF0) == 0x40) --first;
                while (code[first - 1] == 0x66 || code[first - 1] == 0xF2 || code[first - 1] == 0xF3) --first;
                const uint8_t modrm = static_cast<uint8_t>((code[sib - 1] & 0x38) | 0x05);
                std::memmove(code + first + 1, code + first, sib - 1 - first);
                code[first] = 0x90;
                code[sib] = modrm;
                const int32_t rel = static_cast<int32_t>(target - reinterpret_cast<uint64_t>(code + i + 4));
                std::memcpy(code + i, &rel, 4);
                ++sib_patched;
                continue;
            }
            // mov r32, imm32  (b8+r), optionally with a REX.B prefix.
            if (code[i - 1] >= 0xB8 && code[i - 1] <= 0xBF && code[i + 4] != 0xFF) {
                int reg = code[i - 1] - 0xB8;
                uint32_t start = i - 1;
                if (code[i - 2] == 0x41) { reg += 8; start = i - 2; }
                const uint64_t after = reinterpret_cast<uint64_t>(code + i + 4);
                const uint64_t stub = make_stub(reg, target, after);
                code[start] = 0xE9;
                const int32_t rel = static_cast<int32_t>(stub - (reinterpret_cast<uint64_t>(code + start) + 5));
                std::memcpy(code + start + 1, &rel, 4);
                for (uint32_t k2 = start + 5; k2 < i + 4; ++k2) code[k2] = 0x90;
                ++stub_patched;
                continue;
            }
            // mov r64, simm32  (rex.W c7 /0)
            if ((code[i - 3] & 0xFE) == 0x48 && code[i - 2] == 0xC7 && (code[i - 1] & 0xF8) == 0xC0) {
                const int reg = (code[i - 1] & 7) + ((code[i - 3] & 1) ? 8 : 0);
                const uint32_t start = i - 3;
                const uint64_t after = reinterpret_cast<uint64_t>(code + i + 4);
                const uint64_t stub = make_stub(reg, target, after);
                code[start] = 0xE9;
                const int32_t rel = static_cast<int32_t>(stub - (reinterpret_cast<uint64_t>(code + start) + 5));
                std::memcpy(code + start + 1, &rel, 4);
                for (uint32_t k2 = start + 5; k2 < i + 4; ++k2) code[k2] = 0x90;
                ++stub_patched;
                continue;
            }
            ++unknown;
            UT_WARN("ntdll+%#x: unhandled KUSER_SHARED_DATA reference (%02x %02x %02x %02x %02x %02x %02x %02x)",
                    s.rva + i, code[i - 4], code[i - 3], code[i - 2], code[i - 1], code[i], code[i + 1], code[i + 2], code[i + 3]);
        }
    }
    // The stubs are code.
    uint64_t sb = stub_area, ss = 0x1000;
    vm_->Protect(&sb, &ss, PAGE_EXECUTE_READ, nullptr);
    space_->NotifyContentsReplaced(ntdll_base_, kusd_ + 0x2000);
    UT_INFO("KUSER_SHARED_DATA: %d operand, %d value (stub), %d absolute rewrites; %d left", sib_patched, stub_patched, abs_patched, unknown);
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
    const NTSTATUS rc = MapImageSection(section, &base, &size);
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
    std::unique_lock lock {exit_mutex_};
    exit_cv_.wait(lock, [&] { return process_done_; });
    return exit_code.load();
}

} // namespace utsuro::nt
