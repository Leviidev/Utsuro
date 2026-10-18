// vm.h -- NT virtual memory on top of the guest arena.
//
// Windows reserves in 64KB units and commits in 4KB pages; the host page is 16KB and the
// arena only records protections (see engine/guest_memory.h). So this layer keeps NT's
// view -- allocation regions with a protection per 4KB page -- and tells the arena the
// coarse truth: which ranges are mapped, and which of them are executable or writable.
#pragma once

#include "engine/guest_memory.h"
#include "nt/nt_types.h"

#include <map>
#include <string>
#include <mutex>
#include <vector>

namespace utsuro::nt {

constexpr uint64_t kPage = 0x1000;
constexpr uint64_t kAllocGranularity = 0x10000;

enum class RegionKind { Private, Mapped, Image };

struct Region {
    uint64_t base {};
    uint64_t size {};  ///< Extent in the arena, a multiple of 64KB.
    uint64_t used {};  ///< Bytes the caller asked for (page rounded); the rest is padding.
    uint32_t alloc_protect {};
    RegionKind kind {RegionKind::Private};
    /// 0 = reserved only; otherwise the NT protection of a committed page.
    std::vector<uint32_t> pages;
    std::string name;  ///< Backing file for images/sections, for MemoryMappedFilenameInformation.
};

class VirtualMemory {
public:
    VirtualMemory(GuestAddressSpace& space, uint64_t arena_base, uint64_t arena_size);

    /// NtAllocateVirtualMemory. `base` is in/out, `size` in/out.
    NTSTATUS Allocate(uint64_t* base, uint64_t* size, uint32_t type, uint32_t protect,
                      RegionKind kind = RegionKind::Private, const std::string& name = {});
    /// NtFreeVirtualMemory.
    NTSTATUS Free(uint64_t* base, uint64_t* size, uint32_t type);
    /// NtProtectVirtualMemory.
    NTSTATUS Protect(uint64_t* base, uint64_t* size, uint32_t new_protect, uint32_t* old_protect);

    struct Info {
        uint64_t base {};
        uint64_t allocation_base {};
        uint32_t allocation_protect {};
        uint64_t size {};
        uint32_t state {MEM_FREE};
        uint32_t protect {PAGE_NOACCESS};
        uint32_t type {};
        std::string name;
        uint64_t alloc_size {};
        uint64_t commit_size {};
    };
    Info Query(uint64_t address) const;

    /// Places an already-mapped external range (an image in the fixed window) in the
    /// table so queries and frees understand it.
    void AdoptImage(uint64_t base, uint64_t size, const std::string& name, uint32_t protect = PAGE_EXECUTE_READ);

    /// Sets the NT protection of an image section range after the fact.
    void SetImagePages(uint64_t address, uint64_t size, uint32_t protect);

    GuestAddressSpace& Space() { return space_; }
    bool Contains(uint64_t address, uint64_t length) const;

    static int ToGuestProtection(uint32_t nt_protect);

private:
    uint64_t FindFree(uint64_t size, bool top_down) const;
    const Region* RegionAt(uint64_t address) const;
    Region* RegionAt(uint64_t address) { return const_cast<Region*>(static_cast<const VirtualMemory*>(this)->RegionAt(address)); }
    void SyncArena(Region& region, uint64_t first_page, uint64_t page_count);
    void ZeroPages(uint64_t address, uint64_t length);

    GuestAddressSpace& space_;
    mutable std::mutex mutex_;
    uint64_t arena_base_;
    uint64_t arena_end_;
    std::map<uint64_t, Region> regions_;  ///< Keyed by base; includes adopted images.
};

} // namespace utsuro::nt
