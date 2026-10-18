// sys_misc.cpp -- the registry (empty for now), tracing, object namespace and other
// calls the loader and runtime make but that need no real behaviour yet.
#include "nt/kernel.h"

#include "engine/log.h"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstring>

namespace utsuro::nt {

namespace {

NTSTATUS DebugFilterEnabled(Kernel&, NtThread&, SyscallArgs&) { return 1; }
NTSTATUS NotFound(Kernel&, NtThread&, SyscallArgs&) { return STATUS_OBJECT_NAME_NOT_FOUND; }
NTSTATUS NoMore(Kernel&, NtThread&, SyscallArgs&) { return STATUS_NO_MORE_ENTRIES; }
NTSTATUS Succeed(Kernel&, NtThread&, SyscallArgs&) { return STATUS_SUCCESS; }
NTSTATUS NotImplemented(Kernel&, NtThread&, SyscallArgs&) { return STATUS_NOT_IMPLEMENTED; }
NTSTATUS Unsupported(Kernel&, NtThread&, SyscallArgs&) { return STATUS_NOT_SUPPORTED; }

// NtGetNlsSectionPtr(Type, SectionData, ContextData, PVOID* Pointer, PULONG Size)
// ntdll asks for the kernel's NLS tables as a mapped section. The only table file in the
// image is locale.nls; everything else is reported missing.
NTSTATUS NtGetNlsSectionPtr(Kernel& k, NtThread&, SyscallArgs& a) {
    const uint32_t type = static_cast<uint32_t>(a[0]);
    UT_DEBUG("NtGetNlsSectionPtr type %u data %llu", type, (unsigned long long)a[1]);
    std::string host;
    bool exists = false;
    if (type == 11 && k.ResolvePath("C:\\Windows\\System32\\locale.nls", &host, &exists) && exists) {
        std::vector<uint8_t> data;
        FILE* f = std::fopen(host.c_str(), "rb");
        if (f == nullptr) return STATUS_OBJECT_NAME_NOT_FOUND;
        std::fseek(f, 0, SEEK_END);
        const long sz = std::ftell(f);
        std::fseek(f, 0, SEEK_SET);
        uint64_t base = 0, size = static_cast<uint64_t>(sz);
        if (k.Vm().Allocate(&base, &size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE) != STATUS_SUCCESS) { std::fclose(f); return STATUS_NO_MEMORY; }
        std::fread(reinterpret_cast<void*>(base), 1, static_cast<size_t>(sz), f);
        std::fclose(f);
        *a.Ptr<uint64_t>(3) = base;
        if (auto* s = a.Ptr<uint32_t>(4)) *s = static_cast<uint32_t>(sz);
        return STATUS_SUCCESS;
    }
    return STATUS_OBJECT_NAME_NOT_FOUND;
}

// NtInitializeNlsFiles(PVOID* BaseAddress, PLCID DefaultLocaleId, PLARGE_INTEGER DefaultCasingTableSize):
// maps locale.nls, the system's locale data, and reports the default locale.
NTSTATUS NtInitializeNlsFiles(Kernel& k, NtThread&, SyscallArgs& a) {
    std::string host;
    bool exists = false;
    if (!k.ResolvePath("C:\\Windows\\System32\\locale.nls", &host, &exists) || !exists) return STATUS_OBJECT_NAME_NOT_FOUND;
    FILE* f = std::fopen(host.c_str(), "rb");
    if (f == nullptr) return STATUS_OBJECT_NAME_NOT_FOUND;
    std::fseek(f, 0, SEEK_END);
    const long sz = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    uint64_t base = 0, size = static_cast<uint64_t>(sz);
    if (k.Vm().Allocate(&base, &size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE, RegionKind::Mapped, "locale.nls") != STATUS_SUCCESS) {
        std::fclose(f);
        return STATUS_NO_MEMORY;
    }
    std::fread(reinterpret_cast<void*>(base), 1, static_cast<size_t>(sz), f);
    std::fclose(f);
    uint64_t pb = base, ps = size;
    k.Vm().Protect(&pb, &ps, PAGE_READONLY, nullptr);
    *a.Ptr<uint64_t>(0) = base;
    *a.Ptr<uint32_t>(1) = 0x0409;
    if (auto* c = a.Ptr<int64_t>(2)) *c = sz;
    return STATUS_SUCCESS;
}

// NtTraceEvent(Handle, Flags, FieldSize, Fields): drop ETW events.
NTSTATUS NtTraceEvent(Kernel&, NtThread&, SyscallArgs&) { return STATUS_SUCCESS; }

std::string ObjectPath(Kernel& k, const OBJECT_ATTRIBUTES* oa) {
    if (oa == nullptr || oa->ObjectName == 0) return {};
    std::string name = Utf16ToUtf8(ReadUnicodeString(reinterpret_cast<const UNICODE_STRING*>(oa->ObjectName)));
    if (oa->RootDirectory != 0) {
        auto root = k.GetObject(oa->RootDirectory);
        if (root) name = root->name + "\\" + name;
    }
    return name;
}

NTSTATUS NtOpenDirectoryObject(Kernel& k, NtThread&, SyscallArgs& a) {
    auto d = std::make_shared<DirectoryObject>();
    d->name = ObjectPath(k, a.Ptr<OBJECT_ATTRIBUTES>(2));
    *a.Ptr<HANDLE>(0) = k.AddHandle(d);
    return STATUS_SUCCESS;
}

// NtOpenSymbolicLinkObject(PHANDLE, Access, POBJECT_ATTRIBUTES)
NTSTATUS NtOpenSymbolicLinkObject(Kernel& k, NtThread&, SyscallArgs& a) {
    const std::string path = ObjectPath(k, a.Ptr<OBJECT_ATTRIBUTES>(2));
    auto link = std::make_shared<SymbolicLinkObject>();
    link->name = path;
    if (path == "\\KnownDlls\\KnownDllPath") link->target = "C:\\Windows\\System32";
    else if (path == "\\KnownDlls\\KnownDllPath32") link->target = "C:\\Windows\\SysWOW64";
    else {
        UT_DEBUG("symbolic link %s not found", path.c_str());
        return STATUS_OBJECT_NAME_NOT_FOUND;
    }
    *a.Ptr<HANDLE>(0) = k.AddHandle(link);
    return STATUS_SUCCESS;
}

// NtQuerySymbolicLinkObject(Handle, PUNICODE_STRING LinkTarget, PULONG ReturnedLength)
NTSTATUS NtQuerySymbolicLinkObject(Kernel& k, NtThread&, SyscallArgs& a) {
    auto l = k.Get<SymbolicLinkObject>(a[0], ObjType::SymbolicLink);
    if (!l) return STATUS_INVALID_HANDLE;
    auto* us = a.Ptr<UNICODE_STRING>(1);
    const std::u16string text = Utf8ToUtf16(l->target);
    const uint32_t bytes = static_cast<uint32_t>(text.size() * 2);
    if (auto* r = a.Ptr<uint32_t>(2)) *r = bytes;
    if (us->MaximumLength < bytes) return STATUS_BUFFER_TOO_SMALL;
    std::memcpy(reinterpret_cast<void*>(us->Buffer), text.data(), bytes);
    us->Length = static_cast<uint16_t>(bytes);
    return STATUS_SUCCESS;
}

NTSTATUS NtCreateDirectoryObject(Kernel& k, NtThread& t, SyscallArgs& a) { return NtOpenDirectoryObject(k, t, a); }

} // namespace

void RegisterMiscSyscalls(std::unordered_map<std::string, SyscallFn>& t) {
    t["NtGetNlsSectionPtr"] = NtGetNlsSectionPtr;
    t["NtInitializeNlsFiles"] = NtInitializeNlsFiles;
    t["NtTraceEvent"] = NtTraceEvent;
    t["NtTraceControl"] = Unsupported;
    t["NtOpenDirectoryObject"] = NtOpenDirectoryObject;
    t["NtCreateDirectoryObject"] = NtCreateDirectoryObject;
    t["NtOpenSymbolicLinkObject"] = NtOpenSymbolicLinkObject;
    t["NtQuerySymbolicLinkObject"] = NtQuerySymbolicLinkObject;
    t["NtQueryDirectoryObject"] = NoMore;
    t["NtCreateWnfStateName"] = NotImplemented;
    t["NtQueryWnfStateData"] = NotImplemented;
    t["NtUpdateWnfStateData"] = NotImplemented;
    t["NtSubscribeWnfStateChange"] = NotImplemented;
    t["NtQueryDebugFilterState"] = DebugFilterEnabled;
    t["NtQueryIntervalProfile"] = Succeed;
}

} // namespace utsuro::nt
