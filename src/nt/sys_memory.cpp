// sys_memory.cpp -- virtual memory and sections.
#include "nt/kernel.h"

#include "engine/log.h"

#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cstring>

namespace utsuro::nt {

namespace {

constexpr uint64_t kCurrentProcess = ~0ull;

bool IsSelf(HANDLE h) { return h == kCurrentProcess || h == 0x4; }

// NtAllocateVirtualMemory(Process, PVOID* Base, ULONG_PTR ZeroBits, PSIZE_T Size, ULONG Type, ULONG Protect)
NTSTATUS NtAllocateVirtualMemory(Kernel& k, NtThread&, SyscallArgs& a) {
    if (!IsSelf(a[0])) return STATUS_NOT_SUPPORTED;
    auto* base = a.Ptr<uint64_t>(1);
    auto* size = a.Ptr<uint64_t>(3);
    const uint32_t type = static_cast<uint32_t>(a[4]);
    const uint32_t prot = static_cast<uint32_t>(a[5]);
    uint64_t b = *base, s = *size;
    const NTSTATUS rc = k.Vm().Allocate(&b, &s, type, prot);
    UT_DEBUG("vm alloc(base %#llx size %#llx type %#x prot %#x) -> %#x base %#llx size %#llx", (unsigned long long)*base,
             (unsigned long long)*size, type, prot, (uint32_t)rc, (unsigned long long)b, (unsigned long long)s);
    if (rc == STATUS_SUCCESS) {
        *base = b;
        *size = s;
    }
    return rc;
}

// NtFreeVirtualMemory(Process, PVOID* Base, PSIZE_T Size, ULONG Type)
NTSTATUS NtFreeVirtualMemory(Kernel& k, NtThread&, SyscallArgs& a) {
    if (!IsSelf(a[0])) return STATUS_NOT_SUPPORTED;
    auto* base = a.Ptr<uint64_t>(1);
    auto* size = a.Ptr<uint64_t>(2);
    uint64_t b = *base, s = *size;
    const NTSTATUS rc = k.Vm().Free(&b, &s, static_cast<uint32_t>(a[3]));
    UT_DEBUG("vm free(base %#llx size %#llx type %#x) -> %#x", (unsigned long long)*base, (unsigned long long)*size,
             (uint32_t)a[3], (uint32_t)rc);
    if (rc == STATUS_SUCCESS) {
        *base = b;
        *size = s;
    }
    return rc;
}

// NtProtectVirtualMemory(Process, PVOID* Base, PSIZE_T Size, ULONG New, PULONG Old)
NTSTATUS NtProtectVirtualMemory(Kernel& k, NtThread&, SyscallArgs& a) {
    if (!IsSelf(a[0])) return STATUS_NOT_SUPPORTED;
    auto* base = a.Ptr<uint64_t>(1);
    auto* size = a.Ptr<uint64_t>(2);
    uint64_t b = *base, s = *size;
    uint32_t old = 0;
    const NTSTATUS rc = k.Vm().Protect(&b, &s, static_cast<uint32_t>(a[3]), &old);
    if (rc == STATUS_SUCCESS) {
        *base = b;
        *size = s;
        if (auto* o = a.Ptr<uint32_t>(4)) *o = old;
    }
    return rc;
}

#pragma pack(push, 1)
struct MEMORY_BASIC_INFORMATION64 {
    uint64_t BaseAddress, AllocationBase;
    uint32_t AllocationProtect;
    uint16_t PartitionId;
    uint16_t _pad;
    uint64_t RegionSize;
    uint32_t State, Protect, Type;
    uint32_t _pad2;
};
#pragma pack(pop)
static_assert(sizeof(MEMORY_BASIC_INFORMATION64) == 48, "mbi");

// NtQueryVirtualMemory(Process, Base, Class, Buffer, Length, PSIZE_T ReturnLength)
NTSTATUS NtQueryVirtualMemory(Kernel& k, NtThread&, SyscallArgs& a) {
    if (!IsSelf(a[0])) return STATUS_NOT_SUPPORTED;
    const uint64_t address = a[1];
    const uint32_t cls = static_cast<uint32_t>(a[2]);
    auto* buf = a.Ptr<uint8_t>(3);
    const uint64_t len = a[4];
    auto* ret = a.Ptr<uint64_t>(5);
    const auto info = k.Vm().Query(address);
    if (cls == 0) {  // MemoryBasicInformation
        if (len < sizeof(MEMORY_BASIC_INFORMATION64)) return STATUS_INFO_LENGTH_MISMATCH;
        MEMORY_BASIC_INFORMATION64 m {};
        m.BaseAddress = info.base;
        m.AllocationBase = info.allocation_base;
        m.AllocationProtect = info.allocation_protect;
        m.RegionSize = info.size;
        m.State = info.state;
        m.Protect = info.protect;
        m.Type = info.type;
        std::memcpy(buf, &m, sizeof(m));
        if (ret) *ret = sizeof(m);
        return STATUS_SUCCESS;
    }
    if (cls == 2) {  // MemoryMappedFilenameInformation: UNICODE_STRING followed by the text
        if (info.state == MEM_FREE || info.name.empty() || info.type == MEM_PRIVATE) return STATUS_INVALID_ADDRESS;
        // The NT device form of the path.
        std::u16string name = Utf8ToUtf16("\\Device\\HarddiskVolume1" + info.name.substr(info.name.size() > 1 && info.name[1] == ':' ? 2 : 0));
        const uint64_t need = sizeof(UNICODE_STRING) + name.size() * 2 + 2;
        if (ret) *ret = need;
        if (len < need) return STATUS_BUFFER_OVERFLOW;
        auto* us = reinterpret_cast<UNICODE_STRING*>(buf);
        us->Length = static_cast<uint16_t>(name.size() * 2);
        us->MaximumLength = us->Length + 2;
        us->Buffer = reinterpret_cast<uint64_t>(buf + sizeof(UNICODE_STRING));
        std::memcpy(buf + sizeof(UNICODE_STRING), name.c_str(), name.size() * 2 + 2);
        return STATUS_SUCCESS;
    }
    if (cls == 3) {  // MemoryRegionInformation
        if (len < 32) return STATUS_INFO_LENGTH_MISMATCH;
        if (info.state == MEM_FREE) return STATUS_INVALID_ADDRESS;
        std::memset(buf, 0, std::min<uint64_t>(len, 40));
        *reinterpret_cast<uint64_t*>(buf + 0) = info.allocation_base;
        *reinterpret_cast<uint32_t*>(buf + 8) = info.allocation_protect;
        // In this Windows 8 era, RegionType carries MEM_IMAGE / MEM_MAPPED / MEM_PRIVATE.
        *reinterpret_cast<uint32_t*>(buf + 12) = info.type;
        *reinterpret_cast<uint64_t*>(buf + 16) = info.alloc_size;
        *reinterpret_cast<uint64_t*>(buf + 24) = info.commit_size;
        if (ret) *ret = 32;
        return STATUS_SUCCESS;
    }
    if (cls == 4) {  // MemoryWorkingSetExInformation: {VirtualAddress, VirtualAttributes} pairs
        const uint64_t count = len / 16;
        for (uint64_t i = 0; i < count; ++i) {
            const uint64_t va = *reinterpret_cast<uint64_t*>(buf + i * 16);
            const auto inf = k.Vm().Query(va);
            uint64_t attrs = 0;
            if (inf.state == MEM_COMMIT) attrs = 1;  // Valid
            *reinterpret_cast<uint64_t*>(buf + i * 16 + 8) = attrs;
        }
        if (ret) *ret = count * 16;
        return STATUS_SUCCESS;
    }
    UT_DEBUG("NtQueryVirtualMemory class %u not handled", cls);
    return STATUS_INVALID_INFO_CLASS;
}

// NtReadVirtualMemory / NtWriteVirtualMemory(Process, Base, Buffer, Size, PSIZE_T Done)
NTSTATUS NtReadVirtualMemory(Kernel&, NtThread&, SyscallArgs& a) {
    if (!IsSelf(a[0])) return STATUS_NOT_SUPPORTED;
    std::memmove(reinterpret_cast<void*>(a[2]), reinterpret_cast<void*>(a[1]), a[3]);
    if (auto* d = a.Ptr<uint64_t>(4)) *d = a[3];
    return STATUS_SUCCESS;
}
NTSTATUS NtWriteVirtualMemory(Kernel& k, NtThread&, SyscallArgs& a) {
    if (!IsSelf(a[0])) return STATUS_NOT_SUPPORTED;
    std::memmove(reinterpret_cast<void*>(a[1]), reinterpret_cast<void*>(a[2]), a[3]);
    k.Space().NotifyContentsReplaced(a[1], a[1] + a[3]);
    if (auto* d = a.Ptr<uint64_t>(4)) *d = a[3];
    return STATUS_SUCCESS;
}

NTSTATUS NtFlushInstructionCache(Kernel& k, NtThread&, SyscallArgs& a) {
    if (a[1] != 0 && a[2] != 0) k.Space().NotifyContentsReplaced(a[1], a[1] + a[2]);
    return STATUS_SUCCESS;
}

// ---- Sections ----------------------------------------------------------------------

// NtCreateSection(PHANDLE, Access, POBJECT_ATTRIBUTES, PLARGE_INTEGER MaxSize, ULONG PageProt, ULONG Attrs, HANDLE File)
NTSTATUS NtCreateSection(Kernel& k, NtThread&, SyscallArgs& a) {
    auto* out = a.Ptr<HANDLE>(0);
    const auto* oa = a.Ptr<OBJECT_ATTRIBUTES>(2);
    const auto* max_size = a.Ptr<int64_t>(3);
    const uint32_t prot = static_cast<uint32_t>(a[4]);
    const uint32_t attrs = static_cast<uint32_t>(a[5]);
    const HANDLE file_handle = a[6];

    auto section = std::make_shared<SectionObject>();
    section->protection = prot;
    section->attributes = attrs;
    if (oa != nullptr && oa->ObjectName != 0) {
        section->name = Utf16ToUtf8(ReadUnicodeString(reinterpret_cast<const UNICODE_STRING*>(oa->ObjectName)));
    }
    if (file_handle != 0) {
        section->file = k.Get<FileObject>(file_handle, ObjType::File);
        if (!section->file) return STATUS_INVALID_HANDLE;
        struct stat st;
        if (fstat(section->file->fd, &st) == 0) section->size = static_cast<uint64_t>(st.st_size);
    }
    if (max_size != nullptr && *max_size != 0) section->size = static_cast<uint64_t>(*max_size);

    if (attrs & SEC_IMAGE) {
        if (!section->file) return STATUS_INVALID_PARAMETER;
        std::string error;
        section->image = LoadPe(section->file->host_path, error);
        if (!section->image) {
            UT_WARN("NtCreateSection(SEC_IMAGE) %s: %s", section->file->host_path.c_str(), error.c_str());
            return STATUS_INVALID_IMAGE_FORMAT;
        }
        section->image->nt_path = section->file->nt_path;
    }
    *out = k.AddHandle(section);
    return STATUS_SUCCESS;
}

// NtOpenSection(PHANDLE, Access, POBJECT_ATTRIBUTES): only \KnownDlls style names, which do not exist here.
NTSTATUS NtOpenSection(Kernel&, NtThread&, SyscallArgs& a) {
    const auto* oa = a.Ptr<OBJECT_ATTRIBUTES>(2);
    if (oa != nullptr && oa->ObjectName != 0) {
        UT_DEBUG("NtOpenSection %s", Utf16ToUtf8(ReadUnicodeString(reinterpret_cast<const UNICODE_STRING*>(oa->ObjectName))).c_str());
    }
    return STATUS_OBJECT_NAME_NOT_FOUND;
}

// NtMapViewOfSection(Section, Process, PVOID* Base, ULONG_PTR ZeroBits, SIZE_T Commit,
//                    PLARGE_INTEGER Offset, PSIZE_T ViewSize, SECTION_INHERIT, ULONG AllocType, ULONG Protect)
NTSTATUS NtMapViewOfSection(Kernel& k, NtThread&, SyscallArgs& a) {
    auto section = k.Get<SectionObject>(a[0], ObjType::Section);
    if (!section) return STATUS_INVALID_HANDLE;
    if (!IsSelf(a[1])) return STATUS_NOT_SUPPORTED;
    auto* base = a.Ptr<uint64_t>(2);
    const auto* offset = a.Ptr<int64_t>(5);
    auto* view = a.Ptr<uint64_t>(6);
    const uint32_t protect = static_cast<uint32_t>(a[9]);

    if (section->image) {
        uint64_t b = *base, s = *view;
        const NTSTATUS rc = k.MapImageSection(*section, &b, &s);
        if (rc == STATUS_SUCCESS || rc == STATUS_IMAGE_NOT_AT_BASE) {
            *base = b;
            *view = s;
        }
        return rc;
    }

    // Data view: private memory initialised from the file (or zero for a pagefile-backed
    // section). Writable shared views are not written back.
    const uint64_t file_off = offset ? static_cast<uint64_t>(*offset) : 0;
    uint64_t length = *view;
    if (length == 0) length = section->size > file_off ? section->size - file_off : 0;
    if (length == 0) return STATUS_INVALID_PARAMETER;
    uint64_t b = *base, s = length;
    const uint32_t nt_prot = (protect & 0xFF) == PAGE_WRITECOPY ? PAGE_READWRITE : protect;
    NTSTATUS rc = k.Vm().Allocate(&b, &s, MEM_RESERVE | MEM_COMMIT, nt_prot == 0 ? PAGE_READWRITE : PAGE_READWRITE,
                                  RegionKind::Mapped, section->file ? section->file->nt_path : section->name);
    if (rc != STATUS_SUCCESS) return rc;
    {
        static std::atomic<int> shown {0};
        if (shown++ < 40) UT_INFO("vm: NtMapViewOfSection data view %s: %#llx + %#llx (offset %#llx, section size %#llx, protect %#x)",
                                  section->file ? section->file->nt_path.c_str() : "(pagefile)", (unsigned long long)b, (unsigned long long)s,
                                  (unsigned long long)file_off, (unsigned long long)section->size, protect);
    }
    if (section->file) {
        const uint64_t avail = section->size > file_off ? std::min<uint64_t>(length, section->size - file_off) : 0;
        uint64_t done = 0;
        while (done < avail) {
            const ssize_t n = pread(section->file->fd, reinterpret_cast<void*>(b + done), avail - done,
                                    static_cast<off_t>(file_off + done));
            if (n <= 0) break;
            done += static_cast<uint64_t>(n);
        }
    }
    // Apply the requested protection now that the contents are in.
    uint64_t pb = b, ps = s;
    k.Vm().Protect(&pb, &ps, nt_prot == 0 ? PAGE_READWRITE : nt_prot, nullptr);
    *base = b;
    *view = s;
    return STATUS_SUCCESS;
}

NTSTATUS NtUnmapViewOfSection(Kernel& k, NtThread&, SyscallArgs& a) {
    if (!IsSelf(a[0])) return STATUS_NOT_SUPPORTED;
    uint64_t b = a[1], s = 0;
    if (k.Vm().Query(b).state == MEM_FREE) return STATUS_NOT_MAPPED_VIEW;
    return k.Vm().Free(&b, &s, MEM_RELEASE) == STATUS_SUCCESS ? STATUS_SUCCESS : STATUS_NOT_MAPPED_VIEW;
}

// NtQuerySection(Section, Class, Buffer, Length, PSIZE_T Ret)
NTSTATUS NtQuerySection(Kernel& k, NtThread&, SyscallArgs& a) {
    auto section = k.Get<SectionObject>(a[0], ObjType::Section);
    if (!section) return STATUS_INVALID_HANDLE;
    const uint32_t cls = static_cast<uint32_t>(a[1]);
    auto* buf = a.Ptr<uint8_t>(2);
    const uint64_t len = a[3];
    auto* ret = a.Ptr<uint64_t>(4);
    if (cls == 0) {  // SectionBasicInformation: BaseAddress, Attributes, Size
        if (len < 24) return STATUS_INFO_LENGTH_MISMATCH;
        std::memset(buf, 0, 24);
        *reinterpret_cast<uint32_t*>(buf + 8) = section->attributes;
        *reinterpret_cast<uint64_t*>(buf + 16) = section->image ? section->image->size_of_image : section->size;
        if (ret) *ret = 24;
        return STATUS_SUCCESS;
    }
    if (cls == 2 && section->image) {  // SectionRelocationInformation: ULONG_PTR delta
        if (len < 8) return STATUS_INFO_LENGTH_MISMATCH;
        *reinterpret_cast<uint64_t*>(buf) = section->last_base - section->image->image_base;
        if (ret) *ret = 8;
        return STATUS_SUCCESS;
    }
    if (cls == 3 && section->image) {  // SectionOriginalBaseInformation
        if (len < 8) return STATUS_INFO_LENGTH_MISMATCH;
        *reinterpret_cast<uint64_t*>(buf) = section->image->image_base;
        if (ret) *ret = 8;
        return STATUS_SUCCESS;
    }
    if (cls == 1 && section->image) {  // SectionImageInformation
        if (len < 64) return STATUS_INFO_LENGTH_MISMATCH;
        std::memset(buf, 0, 64);
        const auto& pe = *section->image;
        *reinterpret_cast<uint64_t*>(buf + 0) = pe.image_base + pe.entry_rva;   // TransferAddress
        *reinterpret_cast<uint64_t*>(buf + 16) = pe.stack_reserve;
        *reinterpret_cast<uint64_t*>(buf + 24) = pe.stack_commit;
        *reinterpret_cast<uint16_t*>(buf + 32) = pe.subsystem;
        *reinterpret_cast<uint16_t*>(buf + 34) = pe.minor_subsystem;
        *reinterpret_cast<uint16_t*>(buf + 36) = pe.major_subsystem;
        *reinterpret_cast<uint16_t*>(buf + 44) = pe.dll_characteristics;
        *reinterpret_cast<uint16_t*>(buf + 46) = pe.machine;
        if (ret) *ret = 64;
        return STATUS_SUCCESS;
    }
    return STATUS_INVALID_INFO_CLASS;
}

} // namespace

void RegisterMemorySyscalls(std::unordered_map<std::string, SyscallFn>& t) {
    t["NtAllocateVirtualMemory"] = NtAllocateVirtualMemory;
    t["NtFreeVirtualMemory"] = NtFreeVirtualMemory;
    t["NtProtectVirtualMemory"] = NtProtectVirtualMemory;
    t["NtQueryVirtualMemory"] = NtQueryVirtualMemory;
    t["NtReadVirtualMemory"] = NtReadVirtualMemory;
    t["NtWriteVirtualMemory"] = NtWriteVirtualMemory;
    t["NtFlushInstructionCache"] = NtFlushInstructionCache;
    t["NtCreateSection"] = NtCreateSection;
    t["NtOpenSection"] = NtOpenSection;
    t["NtMapViewOfSection"] = NtMapViewOfSection;
    t["NtUnmapViewOfSection"] = NtUnmapViewOfSection;
    t["NtQuerySection"] = NtQuerySection;
}

} // namespace utsuro::nt
