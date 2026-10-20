#include "nt/vm.h"

#include "engine/log.h"

#include <algorithm>
#include <cstring>

namespace utsuro::nt {

namespace {
uint64_t AlignUp(uint64_t v, uint64_t a) { return (v + a - 1) & ~(a - 1); }
uint64_t AlignDown(uint64_t v, uint64_t a) { return v & ~(a - 1); }
} // namespace

VirtualMemory::VirtualMemory(GuestAddressSpace& space, uint64_t arena_base, uint64_t arena_size)
    : space_ {space}, arena_base_ {AlignUp(arena_base, kAllocGranularity)}, arena_end_ {arena_base + arena_size} {}

int VirtualMemory::ToGuestProtection(uint32_t p) {
    p &= 0xFF;
    switch (p) {
    case PAGE_READONLY: return kGuestProtRead;
    case PAGE_READWRITE:
    case PAGE_WRITECOPY: return kGuestProtRead | kGuestProtWrite;
    case PAGE_EXECUTE: return kGuestProtExec | kGuestProtRead;
    case PAGE_EXECUTE_READ: return kGuestProtRead | kGuestProtExec;
    case PAGE_EXECUTE_READWRITE:
    case PAGE_EXECUTE_WRITECOPY: return kGuestProtRead | kGuestProtWrite | kGuestProtExec;
    default: return kGuestProtNone;
    }
}

const Region* VirtualMemory::RegionAt(uint64_t address) const {
    auto it = regions_.upper_bound(address);
    if (it == regions_.begin()) return nullptr;
    --it;
    if (address < it->second.base + it->second.size) return &it->second;
    return nullptr;
}

bool VirtualMemory::Contains(uint64_t address, uint64_t length) const {
    std::scoped_lock lock {mutex_};
    const Region* r = RegionAt(address);
    return r != nullptr && address + length <= r->base + r->used;
}

uint64_t VirtualMemory::FindFree(uint64_t size, bool top_down) const {
    // Gaps between regions inside the arena. The arena itself is not in regions_, only
    // what has been handed out, so walk the sorted regions and look at the holes.
    std::vector<std::pair<uint64_t, uint64_t>> holes;
    uint64_t cursor = arena_base_;
    for (const auto& [base, r] : regions_) {
        if (base < arena_base_ || base >= arena_end_) continue;
        if (base > cursor) holes.emplace_back(cursor, base);
        cursor = std::max(cursor, base + r.size);
    }
    if (cursor < arena_end_) holes.emplace_back(cursor, arena_end_);
    if (top_down) {
        for (auto it = holes.rbegin(); it != holes.rend(); ++it) {
            if (it->second - it->first >= size) return AlignDown(it->second - size, kAllocGranularity);
        }
    } else {
        for (auto& h : holes) {
            const uint64_t start = AlignUp(h.first, kAllocGranularity);
            if (start < h.second && h.second - start >= size) return start;
        }
    }
    return 0;
}

void VirtualMemory::SyncArena(Region& region, uint64_t first_page, uint64_t page_count) {
    // Coalesce runs of equal protection so the arena sees few, large ranges.
    uint64_t i = first_page;
    const uint64_t end = first_page + page_count;
    while (i < end) {
        const int prot = region.pages[i] == 0 ? kGuestProtNone : ToGuestProtection(region.pages[i]);
        uint64_t j = i + 1;
        while (j < end && (region.pages[j] == 0 ? kGuestProtNone : ToGuestProtection(region.pages[j])) == prot) ++j;
        space_.Protect(region.base + i * kPage, (j - i) * kPage, prot);
        i = j;
    }
}

void VirtualMemory::ZeroPages(uint64_t address, uint64_t length) {
    constexpr uint64_t kHostPage = 0x4000;
    const uint64_t first = AlignUp(address, kHostPage);
    const uint64_t last = AlignDown(address + length, kHostPage);
    if (last > first + kHostPage * 4) {
        std::memset(reinterpret_cast<void*>(address), 0, first - address);
        space_.DropPages(first, last - first);
        std::memset(reinterpret_cast<void*>(last), 0, address + length - last);
    } else {
        std::memset(reinterpret_cast<void*>(address), 0, length);
    }
}

NTSTATUS VirtualMemory::Allocate(uint64_t* base_io, uint64_t* size_io, uint32_t type, uint32_t protect,
                                 RegionKind kind, const std::string& name) {
    std::scoped_lock lock {mutex_};
    uint64_t base = *base_io;
    uint64_t size = *size_io;
    if (size == 0 && base == 0) return STATUS_INVALID_PARAMETER;
    if ((type & (MEM_COMMIT | MEM_RESERVE | MEM_RESET)) == 0) return STATUS_INVALID_PARAMETER;

    if (type & MEM_RESET) {
        return STATUS_SUCCESS;
    }

    const bool reserve = (type & MEM_RESERVE) != 0;
    const bool commit = (type & MEM_COMMIT) != 0;
    const uint32_t nt_prot = protect == 0 ? PAGE_READWRITE : protect;

    // A reservation at a fixed address that is outside the arena can never be given. Titles ask for the console's own
    // addresses (Forza Horizon 2: 0x800000000.. and 0x60000000000..) and only need the memory, so it goes anywhere.
    // Only for the console's own allocation types (MEM_LARGE_PAGES / MEM_64K_PAGES / MEM_4MB_PAGES style bits): plain VirtualAlloc callers
    // treat a refused fixed address as a signal to try another one.
    if (base != 0 && reserve && (type & 0x70000000u) != 0 && (base >= arena_end_ || base + size <= arena_base_)) {
        UT_DEBUG("vm: fixed reservation %#llx+%#llx is outside the arena; placed freely", (unsigned long long)base, (unsigned long long)size);
        base = 0;
        *base_io = 0;
    }

    if (base == 0 || reserve) {
        // A new region.
        uint64_t start = base;
        uint64_t used;
        if (start == 0) {
            used = AlignUp(size, kPage);
        } else {
            // Reserving at a requested address: it rounds down to the granularity and
            // the size grows by the part it was rounded off.
            const uint64_t aligned = AlignDown(start, kAllocGranularity);
            used = AlignUp(size + (start - aligned), kPage);
            start = aligned;
        }
        const uint64_t extent = AlignUp(used, kAllocGranularity);
        if (start == 0) {
            start = FindFree(extent, (type & MEM_TOP_DOWN) != 0);
            if (start == 0) {
                UT_WARN("vm: arena exhausted for %#llx bytes", static_cast<unsigned long long>(extent));
                return STATUS_NO_MEMORY;
            }
        } else {
            // Must be free.
            auto it = regions_.upper_bound(start);
            if (it != regions_.begin()) {
                auto prev = std::prev(it);
                if (prev->second.base + prev->second.size > start) return STATUS_CONFLICTING_ADDRESSES;
            }
            if (it != regions_.end() && it->first < start + extent) return STATUS_CONFLICTING_ADDRESSES;
            if (start < arena_base_ || start + extent > arena_end_) return STATUS_CONFLICTING_ADDRESSES;
        }
        if (!space_.CommitFixed(start, extent, kGuestProtNone)) {
            UT_WARN("vm: arena refused %#llx..+%#llx", static_cast<unsigned long long>(start),
                    static_cast<unsigned long long>(extent));
            return STATUS_NO_MEMORY;
        }
        Region r;
        r.base = start;
        r.size = extent;
        r.used = extent;
        r.alloc_protect = nt_prot;
        r.kind = kind;
        r.name = name;
        r.pages.assign(extent / kPage, 0);
        if (commit) {
            const uint64_t first = (base == 0 ? 0 : (*base_io - start)) / kPage;
            const uint64_t count = used / kPage - first;
            for (uint64_t i = 0; i < count; ++i) r.pages[first + i] = nt_prot;
            SyncArena(r, first, count);
        }
        *base_io = base == 0 ? start : *base_io;
        *size_io = base == 0 ? used : AlignUp(size + (*base_io & (kPage - 1)), kPage);
        if (base != 0) *base_io = AlignDown(*base_io, kPage);
        regions_[start] = std::move(r);
        return STATUS_SUCCESS;
    }

    // Commit inside an existing reservation.
    Region* r = RegionAt(base);
    if (r == nullptr) return STATUS_ACCESS_DENIED;
    const uint64_t page_begin = AlignDown(base, kPage);
    const uint64_t page_end = AlignUp(base + size, kPage);
    if (page_end > r->base + r->size) return STATUS_CONFLICTING_ADDRESSES;
    const uint64_t first = (page_begin - r->base) / kPage;
    const uint64_t count = (page_end - page_begin) / kPage;
    for (uint64_t i = 0; i < count; ++i) {
        if (r->pages[first + i] == 0) r->pages[first + i] = nt_prot;
        else r->pages[first + i] = nt_prot;
    }
    SyncArena(*r, first, count);
    *base_io = page_begin;
    *size_io = page_end - page_begin;
    return STATUS_SUCCESS;
}

NTSTATUS VirtualMemory::Free(uint64_t* base_io, uint64_t* size_io, uint32_t type) {
    std::scoped_lock lock {mutex_};
    uint64_t base = *base_io;
    uint64_t size = *size_io;
    Region* r = RegionAt(base);
    if (r == nullptr) return STATUS_UNABLE_TO_FREE_VM;

    if ((type & MEM_RELEASE) && size != 0 && !(r->base == base && AlignUp(size, kPage) >= r->used)) {
        // Releasing part of a reservation, which heaps do to align their base: the region
        // splits around the released range.
        const uint64_t start = AlignDown(base, kPage);
        const uint64_t end = AlignUp(base + size, kPage);
        if (end > r->base + r->size) return STATUS_UNABLE_TO_FREE_VM;
        const uint64_t first = (start - r->base) / kPage;
        const uint64_t count = (end - start) / kPage;
        for (uint64_t i = 0; i < count; ++i)
            if (r->pages[first + i] != 0) return STATUS_UNABLE_TO_FREE_VM;  // committed pages cannot be released this way
        Region old = std::move(*r);
        regions_.erase(old.base);
        auto make = [&](uint64_t from_page, uint64_t to_page) {
            if (to_page <= from_page) return;
            Region piece;
            piece.base = old.base + from_page * kPage;
            piece.size = (to_page - from_page) * kPage;
            piece.used = std::min(piece.size, old.used > from_page * kPage ? old.used - from_page * kPage : 0);
            if (piece.used == 0) piece.used = piece.size;
            piece.alloc_protect = old.alloc_protect;
            piece.kind = old.kind;
            piece.name = old.name;
            piece.pages.assign(old.pages.begin() + static_cast<long>(from_page), old.pages.begin() + static_cast<long>(to_page));
            regions_[piece.base] = std::move(piece);
        };
        make(0, first);
        make(first + count, old.size / kPage);
        space_.Release(start, end - start);
        *base_io = start;
        *size_io = end - start;
        return STATUS_SUCCESS;
    }
    if (type & MEM_RELEASE) {
        if (r->base != base || size != 0) return STATUS_FREE_VM_NOT_AT_BASE;
        const uint64_t rbase = r->base;
        const uint64_t rsize = r->size;
        if (r->kind == RegionKind::Image) {
            UT_WARN("vm: release of image region %#llx ignored", static_cast<unsigned long long>(rbase));
            regions_.erase(rbase);
            return STATUS_SUCCESS;
        }
        regions_.erase(rbase);
        // Mapping fresh pages over the range returns them to the kernel and zeroes them.
        space_.Release(rbase, rsize);
        *base_io = rbase;
        *size_io = rsize;
        return STATUS_SUCCESS;
    }
    if (type & MEM_DECOMMIT) {
        const uint64_t page_begin = AlignDown(base, kPage);
        const uint64_t page_end = size == 0 ? r->base + r->used : AlignUp(base + size, kPage);
        if (page_end > r->base + r->size) return STATUS_UNABLE_TO_FREE_VM;
        const uint64_t first = (page_begin - r->base) / kPage;
        const uint64_t count = (page_end - page_begin) / kPage;
        // The host protects 16 KB pages: open the whole span to zero it, then put every page in it
        // (including neighbours that share a host page) back to what its NT state says.
        constexpr uint64_t kHost = 0x4000;
        const uint64_t lo = std::max(r->base, AlignDown(page_begin, kHost));
        const uint64_t hi = std::min(r->base + r->size, AlignUp(page_end, kHost));
        space_.Protect(lo, hi - lo, (kGuestProtRead | kGuestProtWrite));
        ZeroPages(page_begin, page_end - page_begin);
        for (uint64_t i = 0; i < count; ++i) r->pages[first + i] = 0;
        SyncArena(*r, (lo - r->base) / kPage, (hi - lo) / kPage);
        *base_io = page_begin;
        *size_io = page_end - page_begin;
        return STATUS_SUCCESS;
    }
    return STATUS_INVALID_PARAMETER;
}

NTSTATUS VirtualMemory::Protect(uint64_t* base_io, uint64_t* size_io, uint32_t new_protect, uint32_t* old_protect) {
    std::scoped_lock lock {mutex_};
    const uint64_t base = *base_io;
    const uint64_t size = *size_io;
    Region* r = RegionAt(base);
    if (r == nullptr) return STATUS_CONFLICTING_ADDRESSES;
    const uint64_t page_begin = AlignDown(base, kPage);
    const uint64_t page_end = AlignUp(base + std::max<uint64_t>(size, 1), kPage);
    if (page_end > r->base + r->size) return STATUS_CONFLICTING_ADDRESSES;
    const uint64_t first = (page_begin - r->base) / kPage;
    const uint64_t count = (page_end - page_begin) / kPage;
    if (old_protect != nullptr) *old_protect = r->pages[first] == 0 ? PAGE_NOACCESS : r->pages[first];
    for (uint64_t i = 0; i < count; ++i) {
        if (r->pages[first + i] == 0) return STATUS_NOT_COMMITTED;
    }
    for (uint64_t i = 0; i < count; ++i) r->pages[first + i] = new_protect;
    SyncArena(*r, first, count);
    // Code that was compiled from these pages may be stale once they become writable
    // and then executable again; tell the JIT the bytes are no longer the ones it saw.
    if (new_protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) {
        space_.NotifyContentsReplaced(page_begin, page_end);
    }
    *base_io = page_begin;
    *size_io = page_end - page_begin;
    return STATUS_SUCCESS;
}

VirtualMemory::Info VirtualMemory::Query(uint64_t address) const {
    std::scoped_lock lock {mutex_};
    Info info;
    const uint64_t page = AlignDown(address, kPage);
    const Region* r = RegionAt(address);
    if (r == nullptr || address >= r->base + r->used) {
        // Free: runs up to the next region.
        auto it = regions_.upper_bound(address);
        uint64_t next = it == regions_.end() ? 0x7FFFFFFF0000ull : it->first;
        if (r != nullptr) next = std::min(next, it == regions_.end() ? next : it->first);
        info.base = page;
        info.size = next > page ? next - page : kPage;
        info.state = MEM_FREE;
        info.protect = PAGE_NOACCESS;
        return info;
    }
    const uint64_t index = (page - r->base) / kPage;
    const uint32_t first = r->pages[index];
    uint64_t end = index + 1;
    const uint64_t limit = r->used / kPage;
    while (end < limit && r->pages[end] == first) ++end;
    info.base = page;
    info.allocation_base = r->base;
    info.allocation_protect = r->alloc_protect;
    info.size = (end - index) * kPage;
    info.state = first == 0 ? MEM_RESERVE : MEM_COMMIT;
    info.protect = first == 0 ? 0 : first;
    info.type = r->kind == RegionKind::Image ? MEM_IMAGE : r->kind == RegionKind::Mapped ? MEM_MAPPED : MEM_PRIVATE;
    info.name = r->name;
    info.alloc_size = r->used;
    for (uint32_t p : r->pages) info.commit_size += p != 0 ? kPage : 0;
    return info;
}

void VirtualMemory::AdoptImage(uint64_t base, uint64_t size, const std::string& name, uint32_t protect) {
    std::scoped_lock lock {mutex_};
    Region r;
    r.base = base;
    r.size = AlignUp(size, kAllocGranularity);
    r.used = AlignUp(size, kPage);
    r.alloc_protect = PAGE_EXECUTE_WRITECOPY;
    r.kind = RegionKind::Image;
    r.name = name;
    r.pages.assign(r.size / kPage, protect);
    regions_[base] = std::move(r);
}

void VirtualMemory::SetImagePages(uint64_t address, uint64_t size, uint32_t protect) {
    std::scoped_lock lock {mutex_};
    Region* r = RegionAt(address);
    if (r == nullptr) return;
    const uint64_t first = (AlignDown(address, kPage) - r->base) / kPage;
    const uint64_t count = AlignUp(size, kPage) / kPage;
    for (uint64_t i = 0; i < count && first + i < r->pages.size(); ++i) r->pages[first + i] = protect;
    SyncArena(*r, first, std::min<uint64_t>(count, r->pages.size() - first));
}

} // namespace utsuro::nt

namespace utsuro::nt {

void VirtualMemory::ForEachReadable(const std::function<void(uint64_t, const uint8_t*, uint64_t)>& fn) const {
    std::scoped_lock lock {mutex_};
    for (const auto& [base, r] : regions_) {
        if (r.kind != RegionKind::Private) continue;
        const uint64_t page = kPage;
        for (uint64_t i = 0; i < r.pages.size();) {
            if (r.pages[i] == 0 || (r.pages[i] & 0xFF) == PAGE_NOACCESS) { ++i; continue; }
            uint64_t j = i;
            while (j < r.pages.size() && r.pages[j] != 0 && (r.pages[j] & 0xFF) != PAGE_NOACCESS) ++j;
            fn(base + i * page, reinterpret_cast<const uint8_t*>(base + i * page), (j - i) * page);
            i = j;
        }
    }
}

} // namespace utsuro::nt
