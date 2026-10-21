// sys_info.cpp -- queries about the process, the system and objects; handle plumbing.
#include "nt/kernel.h"

#include "engine/log.h"

#include <sys/sysctl.h>
#include <unistd.h>

#include <cstring>

namespace utsuro::nt {

namespace {

constexpr uint64_t kSelfProcess = ~0ull;

bool IsSelfProcess(HANDLE h) { return h == kSelfProcess || h == 0x4; }

void SetRet(SyscallArgs& a, int idx, uint32_t v) {
    if (auto* r = a.Ptr<uint32_t>(idx)) *r = v;
}

/// Writes a UNICODE_STRING followed by its text into a caller's buffer, as the
/// "...Name"/"...ImageFileName" classes do.
NTSTATUS PutUnicodeString(uint8_t* buf, uint32_t len, const std::u16string& s, uint32_t* ret) {
    const uint32_t need = static_cast<uint32_t>(sizeof(UNICODE_STRING) + s.size() * 2 + 2);
    if (ret) *ret = need;
    if (len < need) return STATUS_INFO_LENGTH_MISMATCH;
    auto* us = reinterpret_cast<UNICODE_STRING*>(buf);
    us->Length = static_cast<uint16_t>(s.size() * 2);
    us->MaximumLength = us->Length + 2;
    us->Buffer = reinterpret_cast<uint64_t>(buf + sizeof(UNICODE_STRING));
    std::memcpy(buf + sizeof(UNICODE_STRING), s.c_str(), s.size() * 2 + 2);
    return STATUS_SUCCESS;
}

// NtClose(Handle)
NTSTATUS NtClose(Kernel& k, NtThread&, SyscallArgs& a) {
    return k.CloseHandle(a[0]) ? STATUS_SUCCESS : STATUS_INVALID_HANDLE;
}

// NtDuplicateObject(SrcProcess, SrcHandle, DstProcess, PHANDLE Dst, Access, Attributes, Options)
NTSTATUS NtDuplicateObject(Kernel& k, NtThread& t, SyscallArgs& a) {
    const HANDLE src = a[1];
    std::shared_ptr<Object> obj;
    if (src == ~0ull) {
        UT_WARN("duplicating the process pseudo handle is not supported");
        return STATUS_NOT_SUPPORTED;
    }
    if (src == ~1ull) obj = k.FindThread(t.tid_);
    else obj = k.GetObject(src);
    if (!obj) return STATUS_INVALID_HANDLE;
    if (auto* dst = a.Ptr<HANDLE>(3)) *dst = k.AddHandle(obj);
    if (a[6] & 1) k.CloseHandle(src);  // DUPLICATE_CLOSE_SOURCE
    return STATUS_SUCCESS;
}

// NtQueryObject(Handle, Class, Buffer, Length, PULONG Ret)
NTSTATUS NtQueryObject(Kernel& k, NtThread&, SyscallArgs& a) {
    auto o = k.GetObject(a[0]);
    const uint32_t cls = static_cast<uint32_t>(a[1]);
    auto* buf = a.Ptr<uint8_t>(2);
    const uint32_t len = static_cast<uint32_t>(a[3]);
    if (!o) return STATUS_INVALID_HANDLE;
    static const char* const names[] = {"File", "Section", "Event", "Semaphore", "Mutant", "Thread", "Process", "Key",
                                        "Timer", "IoCompletion", "Directory", "SymbolicLink", "Token", "KeyedEvent",
                                        "Job", "TpWorkerFactory", "Object"};
    switch (cls) {
    case 0: {  // ObjectBasicInformation
        if (len < 56) return STATUS_INFO_LENGTH_MISMATCH;
        std::memset(buf, 0, 56);
        *reinterpret_cast<uint32_t*>(buf + 4) = 0x1FFFFF;
        *reinterpret_cast<uint32_t*>(buf + 8) = 1;
        *reinterpret_cast<uint32_t*>(buf + 12) = 1;
        SetRet(a, 4, 56);
        return STATUS_SUCCESS;
    }
    case 1: {  // ObjectNameInformation
        std::string name;
        if (o->type == ObjType::File) {
            name = "\\Device\\HarddiskVolume1" + (static_cast<FileObject&>(*o).nt_path.size() > 2 ? static_cast<FileObject&>(*o).nt_path.substr(2) : "\\");
        } else {
            name = o->name;
        }
        return PutUnicodeString(buf, len, Utf8ToUtf16(name), a.Ptr<uint32_t>(4));
    }
    case 2: {  // ObjectTypeInformation: UNICODE_STRING TypeName, then counters
        const std::u16string tn = Utf8ToUtf16(names[static_cast<int>(o->type)]);
        const uint32_t header = 0x68;
        const uint32_t need = header + static_cast<uint32_t>(tn.size() * 2 + 2);
        SetRet(a, 4, need);
        if (len < need) return STATUS_INFO_LENGTH_MISMATCH;
        std::memset(buf, 0, header);
        auto* us = reinterpret_cast<UNICODE_STRING*>(buf);
        us->Length = static_cast<uint16_t>(tn.size() * 2);
        us->MaximumLength = us->Length + 2;
        us->Buffer = reinterpret_cast<uint64_t>(buf + header);
        std::memcpy(buf + header, tn.c_str(), tn.size() * 2 + 2);
        return STATUS_SUCCESS;
    }
    case 4:  // ObjectHandleFlagInformation: Inherit, ProtectFromClose
        if (len < 2) return STATUS_INFO_LENGTH_MISMATCH;
        buf[0] = buf[1] = 0;
        SetRet(a, 4, 2);
        return STATUS_SUCCESS;
    default:
        return STATUS_INVALID_INFO_CLASS;
    }
}

NTSTATUS Succeed(Kernel&, NtThread&, SyscallArgs&) { return STATUS_SUCCESS; }

// NtQueryInformationProcess(Process, Class, Buffer, Length, PULONG Ret)
NTSTATUS NtQueryInformationProcess(Kernel& k, NtThread&, SyscallArgs& a) {
    if (!IsSelfProcess(a[0])) return STATUS_NOT_SUPPORTED;
    const uint32_t cls = static_cast<uint32_t>(a[1]);
    auto* buf = a.Ptr<uint8_t>(2);
    const uint32_t len = static_cast<uint32_t>(a[3]);
    auto need = [&](uint32_t n) { SetRet(a, 4, n); return len >= n; };
    switch (cls) {
    case 0: {  // ProcessBasicInformation
        if (!need(48)) return STATUS_INFO_LENGTH_MISMATCH;
        std::memset(buf, 0, 48);
        *reinterpret_cast<int32_t*>(buf) = 0x103;
        *reinterpret_cast<uint64_t*>(buf + 8) = k.PebAddress();
        *reinterpret_cast<uint64_t*>(buf + 16) = (1ull << k.NumberOfProcessors()) - 1;
        *reinterpret_cast<int32_t*>(buf + 24) = 8;
        *reinterpret_cast<uint64_t*>(buf + 32) = 4;
        *reinterpret_cast<uint64_t*>(buf + 40) = 0;
        return STATUS_SUCCESS;
    }
    case 7:   // ProcessDebugPort
    case 26:  // ProcessWow64Information
        if (!need(8)) return STATUS_INFO_LENGTH_MISMATCH;
        *reinterpret_cast<uint64_t*>(buf) = 0;
        return STATUS_SUCCESS;
    case 20:  // ProcessHandleCount
        if (!need(4)) return STATUS_INFO_LENGTH_MISMATCH;
        *reinterpret_cast<uint32_t*>(buf) = 32;
        return STATUS_SUCCESS;
    case 22:  // ProcessSessionInformation
        if (!need(4)) return STATUS_INFO_LENGTH_MISMATCH;
        *reinterpret_cast<uint32_t*>(buf) = 1;
        return STATUS_SUCCESS;
    case 29:  // ProcessBreakOnTermination
        if (!need(4)) return STATUS_INFO_LENGTH_MISMATCH;
        *reinterpret_cast<uint32_t*>(buf) = 0;
        return STATUS_SUCCESS;
    case 30:  // ProcessDebugObjectHandle
        return static_cast<NTSTATUS>(0xC0000353);  // STATUS_PORT_NOT_SET
    case 31:  // ProcessDebugFlags
        if (!need(4)) return STATUS_INFO_LENGTH_MISMATCH;
        *reinterpret_cast<uint32_t*>(buf) = 1;
        return STATUS_SUCCESS;
    case 34:  // ProcessExecuteFlags
        if (!need(4)) return STATUS_INFO_LENGTH_MISMATCH;
        *reinterpret_cast<uint32_t*>(buf) = 0x2;
        return STATUS_SUCCESS;
    case 36:  // ProcessCookie
        if (!need(4)) return STATUS_INFO_LENGTH_MISMATCH;
        *reinterpret_cast<uint32_t*>(buf) = 0x5A17C0DE;
        return STATUS_SUCCESS;
    case 27:    // ProcessImageFileName (NT form)
    case 43: {  // ProcessImageFileNameWin32
        std::string name = "G:\\" + k.Options().exe_name;
        if (cls == 27) name = "\\Device\\HarddiskVolume1\\" + k.Options().exe_name;
        uint32_t ret = 0;
        const NTSTATUS rc = PutUnicodeString(buf, len, Utf8ToUtf16(name), &ret);
        SetRet(a, 4, ret);
        return rc;
    }
    case 4:   // ProcessQuotaLimits
    case 1:   // ProcessQuotaLimits (kernel numbering varies)
    case 2:   // ProcessIoCounters
    case 3:   // ProcessVmCounters
        if (len < 8) return STATUS_INFO_LENGTH_MISMATCH;
        std::memset(buf, 0, len);
        SetRet(a, 4, len);
        return STATUS_SUCCESS;
    case 12:  // ProcessPriorityClass
        if (!need(2)) return STATUS_INFO_LENGTH_MISMATCH;
        buf[0] = 0; buf[1] = 2;
        return STATUS_SUCCESS;
    case 23:  // ProcessLUIDDeviceMapsEnabled
        if (!need(4)) return STATUS_INFO_LENGTH_MISMATCH;
        *reinterpret_cast<uint32_t*>(buf) = 0;
        return STATUS_SUCCESS;
    case 37: {  // ProcessImageInformation: SECTION_IMAGE_INFORMATION
        if (!need(64)) return STATUS_INFO_LENGTH_MISMATCH;
        std::memset(buf, 0, 64);
        return STATUS_SUCCESS;
    }
    default:
        UT_DEBUG("NtQueryInformationProcess class %u not handled", cls);
        return STATUS_INVALID_INFO_CLASS;
    }
}

// NtSetInformationProcess(Process, Class, Buffer, Length)
NTSTATUS NtSetInformationProcess(Kernel& k, NtThread&, SyscallArgs& a) {
    const uint32_t cls = static_cast<uint32_t>(a[1]);
    auto* buf = a.Ptr<uint8_t>(2);
    const uint32_t len = static_cast<uint32_t>(a[3]);
    if (cls == 41 && buf != nullptr && (len == 0x28 || len == 0x18)) {
        // ProcessThreadStackAllocation: RtlCreateUserStack (2012+ ntdll) has the kernel reserve a thread or fiber
        // stack and hand back its base; it then commits the top itself. PROCESS_STACK_ALLOCATION_INFORMATION
        // {ReserveSize, ZeroBits, StackBase}, in the _EX form after a 16-byte node preference header.
        // Committed in full, like thread stacks (NewThread): there is no guard-page growth here, and the title's
        // fibers run past the 128 KB the caller commits.
        auto* info = reinterpret_cast<uint64_t*>(buf + (len == 0x28 ? 0x10 : 0));
        uint64_t base = 0, size = info[0];
        const NTSTATUS rc = k.Vm().Allocate(&base, &size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        UT_DEBUG("process: stack reservation of %#llx bytes -> %#x at %#llx", (unsigned long long)info[0], (uint32_t)rc, (unsigned long long)base);
        if (rc != STATUS_SUCCESS) return rc;
        info[2] = base;
        return STATUS_SUCCESS;
    }
    return STATUS_SUCCESS;
}

// NtQuerySystemInformation(Class, Buffer, Length, PULONG Ret)
NTSTATUS NtQuerySystemInformation(Kernel& k, NtThread&, SyscallArgs& a) {
    const uint32_t cls = static_cast<uint32_t>(a[0]);
    auto* buf = a.Ptr<uint8_t>(1);
    const uint32_t len = static_cast<uint32_t>(a[2]);
    auto need = [&](uint32_t n) { SetRet(a, 3, n); return len >= n; };
    switch (cls) {
    case 0:    // SystemBasicInformation
    case 62: { // SystemEmulationBasicInformation
        if (!need(64)) return STATUS_INFO_LENGTH_MISMATCH;
        std::memset(buf, 0, 64);
        *reinterpret_cast<uint32_t*>(buf + 4) = 156250;     // TimerResolution
        *reinterpret_cast<uint32_t*>(buf + 8) = 4096;       // PageSize
        *reinterpret_cast<uint32_t*>(buf + 12) = 0x200000;  // NumberOfPhysicalPages
        *reinterpret_cast<uint32_t*>(buf + 16) = 1;
        *reinterpret_cast<uint32_t*>(buf + 20) = 0x1FFFFF;
        *reinterpret_cast<uint32_t*>(buf + 24) = 0x10000;   // AllocationGranularity
        *reinterpret_cast<uint64_t*>(buf + 32) = 0x10000;
        *reinterpret_cast<uint64_t*>(buf + 40) = 0x00007FFFFFFEFFFFull;
        *reinterpret_cast<uint64_t*>(buf + 48) = (1ull << k.NumberOfProcessors()) - 1;
        buf[56] = static_cast<uint8_t>(k.NumberOfProcessors());
        return STATUS_SUCCESS;
    }
    case 1: {  // SystemProcessorInformation
        if (!need(12)) return STATUS_INFO_LENGTH_MISMATCH;
        std::memset(buf, 0, 12);
        *reinterpret_cast<uint16_t*>(buf) = 9;        // PROCESSOR_ARCHITECTURE_AMD64
        *reinterpret_cast<uint16_t*>(buf + 2) = 6;    // level
        *reinterpret_cast<uint16_t*>(buf + 4) = 0x3C03;
        *reinterpret_cast<uint16_t*>(buf + 6) = static_cast<uint16_t>(k.NumberOfProcessors());
        return STATUS_SUCCESS;
    }
    case 3: {  // SystemTimeOfDayInformation
        if (!need(48)) return STATUS_INFO_LENGTH_MISMATCH;
        std::memset(buf, 0, 48);
        const int64_t now = Kernel::NtSystemTime();
        *reinterpret_cast<int64_t*>(buf) = now - 100ll * 10000000ll;
        *reinterpret_cast<int64_t*>(buf + 8) = now;
        *reinterpret_cast<int32_t*>(buf + 24) = 0;
        return STATUS_SUCCESS;
    }
    case 9:  // SystemFlagsInformation
        if (!need(4)) return STATUS_INFO_LENGTH_MISMATCH;
        *reinterpret_cast<uint32_t*>(buf) = 0;
        return STATUS_SUCCESS;
    case 23: {  // SystemInterruptInformation
        const uint32_t n = 24 * k.NumberOfProcessors();
        if (!need(n)) return STATUS_INFO_LENGTH_MISMATCH;
        std::memset(buf, 0, n);
        return STATUS_SUCCESS;
    }
    case 8: {  // SystemProcessorPerformanceInformation: 48 bytes per processor
        const uint32_t n = 48 * k.NumberOfProcessors();
        if (!need(n)) return STATUS_INFO_LENGTH_MISMATCH;
        std::memset(buf, 0, n);
        return STATUS_SUCCESS;
    }
    case 55: {  // SystemNumaProcessorMap: HighestNodeNumber, then a group affinity per node
        if (!need(0x408)) return STATUS_INFO_LENGTH_MISMATCH;
        std::memset(buf, 0, 0x408);
        *reinterpret_cast<uint32_t*>(buf) = 0;
        *reinterpret_cast<uint64_t*>(buf + 8) = (1ull << k.NumberOfProcessors()) - 1;
        return STATUS_SUCCESS;
    }
    case 35:  // SystemRangeStartInformation
        if (!need(8)) return STATUS_INFO_LENGTH_MISMATCH;
        *reinterpret_cast<uint64_t*>(buf) = 0xFFFF080000000000ull;
        return STATUS_SUCCESS;
    case 29:  // SystemRecommendedSharedDataAlignment
        if (!need(4)) return STATUS_INFO_LENGTH_MISMATCH;
        *reinterpret_cast<uint32_t*>(buf) = 64;
        return STATUS_SUCCESS;
    case 2:    // SystemPerformanceInformation
    case 5:    // SystemProcessInformation: just terminate the list
        if (!need(len < 0x100 ? 0x100 : len > 0x2000 ? 0x2000 : len)) return STATUS_INFO_LENGTH_MISMATCH;
        std::memset(buf, 0, std::min<uint32_t>(len, 0x100));
        return STATUS_SUCCESS;
    default:
        UT_DEBUG("NtQuerySystemInformation class %u not handled", cls);
        return STATUS_INVALID_INFO_CLASS;
    }
}

// NtQuerySystemInformationEx(Class, Input, InputLength, Output, OutputLength, PULONG ReturnLength)
NTSTATUS NtQuerySystemInformationEx(Kernel& k, NtThread&, SyscallArgs& a) {
    const uint32_t cls = static_cast<uint32_t>(a[0]);
    if (cls != 107) {   // SystemLogicalProcessorAndGroupInformation: GetLogicalProcessorInformationEx
        UT_DEBUG("NtQuerySystemInformationEx class %u not handled", cls);
        return STATUS_INVALID_INFO_CLASS;
    }
    const uint32_t relationship = a[1] != 0 && static_cast<uint32_t>(a[2]) >= 4 ? *a.Ptr<uint32_t>(1) : 0xFFFF;
    const uint32_t n = k.NumberOfProcessors();
    const uint64_t all = n >= 64 ? ~0ull : (1ull << n) - 1;
    std::vector<uint8_t> out;
    auto record = [&](uint32_t rel, uint32_t size) -> uint8_t* {
        const size_t at = out.size();
        out.resize(at + size);
        *reinterpret_cast<uint32_t*>(&out[at]) = rel;
        *reinterpret_cast<uint32_t*>(&out[at + 4]) = size;
        return &out[at + 8];
    };
    auto affinity = [](uint8_t* p, uint64_t mask) { *reinterpret_cast<uint64_t*>(p) = mask; };   // Mask, Group 0
    if (relationship == 0 || relationship == 0xFFFF) {
        for (uint32_t i = 0; i < n; ++i) {
            uint8_t* r = record(0, 48);   // RelationProcessorCore
            r[0] = 0;
            *reinterpret_cast<uint16_t*>(r + 22) = 1;
            affinity(r + 24, 1ull << i);
        }
    }
    if (relationship == 1 || relationship == 0xFFFF) {
        uint8_t* r = record(1, 48);       // RelationNumaNode
        affinity(r + 24, all);
    }
    if (relationship == 2 || relationship == 0xFFFF) {
        for (uint32_t level = 1; level <= 3; ++level) {
            uint8_t* r = record(2, 56);   // RelationCache: Level, Associativity, LineSize, CacheSize, Type
            r[0] = static_cast<uint8_t>(level);
            r[1] = 8;
            *reinterpret_cast<uint16_t*>(r + 2) = 64;
            *reinterpret_cast<uint32_t*>(r + 4) = level == 1 ? 32 * 1024 : level == 2 ? 256 * 1024 : 4 * 1024 * 1024;
            *reinterpret_cast<uint32_t*>(r + 8) = level == 1 ? 1 : 2;   // data / unified
            affinity(r + 32, all);
        }
    }
    if (relationship == 3 || relationship == 0xFFFF) {
        uint8_t* r = record(3, 48);       // RelationProcessorPackage
        *reinterpret_cast<uint16_t*>(r + 22) = 1;
        affinity(r + 24, all);
    }
    if (relationship == 4 || relationship == 0xFFFF) {
        uint8_t* r = record(4, 80);       // RelationGroup
        *reinterpret_cast<uint16_t*>(r) = 1;
        *reinterpret_cast<uint16_t*>(r + 2) = 1;
        r[24] = static_cast<uint8_t>(n);
        r[25] = static_cast<uint8_t>(n);
        affinity(r + 24 + 40, all);
    }
    if (auto* ret = a.Ptr<uint32_t>(5)) *ret = static_cast<uint32_t>(out.size());
    if (static_cast<uint32_t>(a[4]) < out.size()) return STATUS_INFO_LENGTH_MISMATCH;
    std::memcpy(a.Ptr<uint8_t>(3), out.data(), out.size());
    return STATUS_SUCCESS;
}

// NtQueryDefaultLocale(BOOLEAN UserProfile, PLCID)
NTSTATUS NtQueryDefaultLocale(Kernel&, NtThread&, SyscallArgs& a) {
    *a.Ptr<uint32_t>(1) = 0x0409;
    return STATUS_SUCCESS;
}

NTSTATUS NtQueryInstallUILanguage(Kernel&, NtThread&, SyscallArgs& a) {
    *a.Ptr<uint16_t>(0) = 0x0409;
    return STATUS_SUCCESS;
}

// NtQueryTimerResolution(PULONG Max, PULONG Min, PULONG Current) in 100ns
NTSTATUS NtQueryTimerResolution(Kernel&, NtThread&, SyscallArgs& a) {
    *a.Ptr<uint32_t>(0) = 156250;
    *a.Ptr<uint32_t>(1) = 5000;
    *a.Ptr<uint32_t>(2) = 156250;
    return STATUS_SUCCESS;
}

NTSTATUS NtSetTimerResolution(Kernel&, NtThread&, SyscallArgs& a) {
    if (auto* c = a.Ptr<uint32_t>(2)) *c = 10000;
    return STATUS_SUCCESS;
}

NTSTATUS NtAllocateLocallyUniqueId(Kernel&, NtThread&, SyscallArgs& a) {
    static std::atomic<uint64_t> next {1000};
    auto* luid = a.Ptr<uint64_t>(0);
    luid[0] = next.fetch_add(1);
    return STATUS_SUCCESS;
}

// NtQueryInformationToken(Token, Class, Buffer, Length, PULONG Ret)
NTSTATUS NtQueryInformationToken(Kernel&, NtThread&, SyscallArgs& a) {
    const uint32_t cls = static_cast<uint32_t>(a[1]);
    auto* buf = a.Ptr<uint8_t>(2);
    const uint32_t len = static_cast<uint32_t>(a[3]);
    auto need = [&](uint32_t n) { SetRet(a, 4, n); return len >= n; };
    switch (cls) {
    case 1: {  // TokenUser: SID_AND_ATTRIBUTES + SID S-1-5-18-ish
        if (!need(8 + 8 + 16)) return STATUS_BUFFER_TOO_SMALL;
        std::memset(buf, 0, 32);
        *reinterpret_cast<uint64_t*>(buf) = reinterpret_cast<uint64_t>(buf + 16);
        buf[16] = 1;      // revision
        buf[17] = 1;      // sub authority count
        buf[18 + 5] = 5;  // NT authority
        *reinterpret_cast<uint32_t*>(buf + 24) = 18;
        return STATUS_SUCCESS;
    }
    case 12:  // TokenSessionId
        if (!need(4)) return STATUS_BUFFER_TOO_SMALL;
        *reinterpret_cast<uint32_t*>(buf) = 1;
        return STATUS_SUCCESS;
    case 20:  // TokenElevation
    case 29:  // TokenIsAppContainer
        if (!need(4)) return STATUS_BUFFER_TOO_SMALL;
        *reinterpret_cast<uint32_t*>(buf) = 0;
        return STATUS_SUCCESS;
    default:
        UT_DEBUG("NtQueryInformationToken class %u not handled", cls);
        return STATUS_INVALID_INFO_CLASS;
    }
}

NTSTATUS NtPowerInformation(Kernel&, NtThread&, SyscallArgs&) { return STATUS_NOT_SUPPORTED; }

NTSTATUS NtIsProcessInJob(Kernel&, NtThread&, SyscallArgs&) { return 0x10B; }  // STATUS_PROCESS_NOT_IN_JOB

NTSTATUS NtOpenProcess(Kernel&, NtThread&, SyscallArgs& a) {
    *a.Ptr<HANDLE>(0) = kSelfProcess;
    return STATUS_SUCCESS;
}

} // namespace

void RegisterInfoSyscalls(std::unordered_map<std::string, SyscallFn>& t) {
    t["NtClose"] = NtClose;
    t["NtDuplicateObject"] = NtDuplicateObject;
    t["NtQueryObject"] = NtQueryObject;
    t["NtSetInformationObject"] = Succeed;
    t["NtQueryInformationProcess"] = NtQueryInformationProcess;
    t["NtSetInformationProcess"] = NtSetInformationProcess;
    t["NtQuerySystemInformation"] = NtQuerySystemInformation;
    t["NtQuerySystemInformationEx"] = NtQuerySystemInformationEx;
    t["NtQueryDefaultLocale"] = NtQueryDefaultLocale;
    t["NtQueryInstallUILanguage"] = NtQueryInstallUILanguage;
    t["NtQueryTimerResolution"] = NtQueryTimerResolution;
    t["NtSetTimerResolution"] = NtSetTimerResolution;
    t["NtAllocateLocallyUniqueId"] = NtAllocateLocallyUniqueId;
    t["NtQueryInformationToken"] = NtQueryInformationToken;
    t["NtPowerInformation"] = NtPowerInformation;
    t["NtIsProcessInJob"] = NtIsProcessInJob;
    t["NtOpenProcess"] = NtOpenProcess;
}

} // namespace utsuro::nt
