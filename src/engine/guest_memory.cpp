#include "guest_memory.h"

#include "log.h"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <shared_mutex>
#include <mach/vm_statistics.h>
#include <sys/mman.h>
#include <unistd.h>

namespace utsuro {

/// Every mapping that backs guest memory carries this tag, so a listing of the host's
/// regions (the memory watchdog's, or vmmap's) tells guest pages from the JIT's own.
constexpr int kArenaTag = VM_MAKE_TAG(242);
namespace {

int ToHostProtection(int guest_protection) {
    // Deliberately drops kGuestProtExec -- see guest_memory.h's header comment. A guest
    // page marked executable is mapped host-readable and FEXCore compiles from it.
    int host = 0;
    if ((guest_protection & kGuestProtRead) != 0) {
        host |= PROT_READ;
    }
    if ((guest_protection & kGuestProtWrite) != 0) {
        host |= PROT_WRITE;
    }
    return host == 0 ? PROT_NONE : host;
}

} // namespace

GuestAddressSpace::GuestAddressSpace(uint64_t base, uint64_t size, uint64_t page_size)
    : base_ {base}
    , size_ {size}
    , page_size_ {page_size} {
    // Everything except the lowest 64KB, which is never handed out. Linux keeps that
    // region unmappable (mmap_min_addr) for one reason: it is what makes a null pointer
    // fault. An arena that starts at guest address zero gives the first anonymous
    // mapping the address zero, and from then on the guest's null dereferences quietly
    // read and write real memory, its null function pointers execute whatever data is
    // there, and nothing reports a fault -- which is how a thread ends up running at
    // guest address 0x35 with a stack pointer of zero and no explanation anywhere.
    constexpr uint64_t kLowestUsable = 64 * 1024;
    if (size > kLowestUsable) {
        free_.push_back(Extent {base + kLowestUsable, size - kLowestUsable});
    }

    // One bit per grain of the arena, sized so the map stays under half a megabyte
    // whatever the arena's size is. 64KB grains for a 4GB arena.
    code_grain_ = 64 * 1024;
    while (size / code_grain_ / 64 > 65536) {
        code_grain_ *= 2;
    }
    code_word_count_ = size / code_grain_ / 64 + 1;
    code_words_ = std::make_unique<std::atomic<uint64_t>[]>(code_word_count_);
    committed_words_ = std::make_unique<std::atomic<uint64_t>[]>(code_word_count_);
    for (uint64_t i = 0; i < code_word_count_; ++i) {
        code_words_[i].store(0, std::memory_order_relaxed);
        committed_words_[i].store(0, std::memory_order_relaxed);
    }
}

static bool WalkCodeMap(std::atomic<uint64_t>* words, uint64_t word_count, uint64_t base,
                        uint64_t grain, uint64_t begin, uint64_t end, bool set) {
    bool had = false;
    const uint64_t first = (begin - base) / grain;
    const uint64_t last = (end - 1 - base) / grain;
    for (uint64_t index = first; index <= last; ++index) {
        const uint64_t word = index / 64;
        if (word >= word_count) {
            return true;
        }
        const uint64_t bit = 1ull << (index % 64);
        const uint64_t before = set ? words[word].fetch_or(bit, std::memory_order_relaxed)
                                    : words[word].load(std::memory_order_relaxed);
        if ((before & bit) != 0) {
            had = true;
        }
    }
    return had;
}

bool GuestAddressSpace::NoteExecutable(uint64_t begin, uint64_t end) {
    // Outside the arena there is no map, so answer the safe way.
    if (code_words_ == nullptr || end <= begin || begin < base_ || end > base_ + size_) {
        return true;
    }
    return WalkCodeMap(code_words_.get(), code_word_count_, base_, code_grain_, begin, end, true);
}

bool GuestAddressSpace::MaybeCommitted(uint64_t address) const {
    if (committed_words_ == nullptr || address < base_ || address >= base_ + size_) {
        return true;
    }
    const uint64_t grain = (address - base_) / code_grain_;
    const uint64_t word = grain / 64;
    if (word >= code_word_count_) {
        return true;
    }
    return (committed_words_[word].load(std::memory_order_relaxed) & (1ull << (grain % 64))) != 0;
}

bool GuestAddressSpace::HasHeldCode(uint64_t begin, uint64_t end) const {
    if (code_words_ == nullptr || end <= begin || begin < base_ || end > base_ + size_) {
        return true;
    }
    return WalkCodeMap(code_words_.get(), code_word_count_, base_, code_grain_, begin, end, false);
}

GuestAddressSpace::~GuestAddressSpace() {
    for (const auto& mapping : external_) {
        if (munmap(reinterpret_cast<void*>(mapping.begin), mapping.size) != 0) {
            UT_WARN("external mapping munmap at %#llx failed: %s",
                        static_cast<unsigned long long>(mapping.begin), std::strerror(errno));
        }
    }
    if (base_ != 0 && munmap(reinterpret_cast<void*>(base_), size_) != 0) {
        UT_WARN("guest arena munmap failed: %s", std::strerror(errno));
    }
}

bool GuestAddressSpace::AdoptCopiedMapping(uint64_t address, uint64_t size, int protection, bool written) {
    if (size == 0 || address < base_ || address + size > base_ + size_) {
        return false;
    }
    std::unique_lock lock {mutex_};
    if (!TakeFreeExtent(address, size)) {
        return false;
    }
    RecordCommitted(address, size, protection);
    if (!written) {
        for (auto& range : committed_) {
            if (range.begin == address && range.size == size) {
                range.written = false;
            }
        }
    }
    return true;
}

bool GuestAddressSpace::AdoptExternalMapping(uint64_t address, uint64_t size, int protection) {
    if (size == 0) {
        return false;
    }
    std::unique_lock lock {mutex_};
    external_.push_back(Extent {address, size});
    RecordCommitted(address, size, protection);
    return true;
}

GuestAddressSpace* GuestAddressSpace::Reserve(uint64_t size, std::string& error, uint64_t alignment) {
    const auto page_size = static_cast<uint64_t>(sysconf(_SC_PAGESIZE));
    const uint64_t aligned = (size + page_size - 1) & ~(page_size - 1);

    // PROT_NONE keeps this a pure address-space reservation: nothing is dirtied, so it
    // does not count against the process's memory limit until a range is committed.
    //
    // With an alignment, more is reserved and the ends trimmed off: a 32-bit guest's base has to sit on a 4GB boundary,
    // because FEXCore relocates its addresses by putting the base's upper half in front of them, not by adding it.
    const uint64_t over = alignment > page_size ? aligned + alignment : aligned;
    void* address = mmap(nullptr, over, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, kArenaTag, 0);
    if (address != MAP_FAILED && over != aligned) {
        const uint64_t raw = reinterpret_cast<uint64_t>(address);
        const uint64_t start = (raw + alignment - 1) & ~(alignment - 1);
        if (start > raw) munmap(address, start - raw);
        if (raw + over > start + aligned) munmap(reinterpret_cast<void*>(start + aligned), raw + over - start - aligned);
        address = reinterpret_cast<void*>(start);
    }
    if (address == MAP_FAILED) {
        error = std::string {"could not reserve a "} + std::to_string(aligned >> 20) +
                "MB guest address space: " + std::strerror(errno);
        return nullptr;
    }

    UT_INFO("reserved guest arena: %llu MB at %p (host page size %llu)",
                static_cast<unsigned long long>(aligned >> 20), address,
                static_cast<unsigned long long>(page_size));
    auto* space = new GuestAddressSpace {reinterpret_cast<uint64_t>(address), aligned, page_size};
    // A way to make a program load where it would load in a busier session. Whether a bug
    // depends on the addresses a program happens to get is otherwise very hard to ask:
    // reproducing it means reproducing everything that ran before it.
    if (const char* skip = getenv("FATHOM_ARENA_SKIP")) {
        const uint64_t bytes = std::strtoull(skip, nullptr, 0);
        if (bytes > 0 && bytes < aligned) {
            space->Allocate(bytes, 0, kGuestProtRead);
            UT_INFO("arena: the first %llu MB are taken, for testing",
                        static_cast<unsigned long long>(bytes >> 20));
        }
    }
    return space;
}

bool GuestAddressSpace::TakeFreeExtent(uint64_t address, uint64_t size) {
    const uint64_t end = address + size;
    for (size_t index = 0; index < free_.size(); ++index) {
        auto& extent = free_[index];
        if (address < extent.begin || end > extent.end()) {
            continue;
        }

        const uint64_t head = address - extent.begin;
        const uint64_t tail = extent.end() - end;
        if (head == 0 && tail == 0) {
            free_.erase(free_.begin() + static_cast<long>(index));
        } else if (head == 0) {
            extent.begin = end;
            extent.size = tail;
        } else if (tail == 0) {
            extent.size = head;
        } else {
            const Extent remainder {end, tail};
            extent.size = head;
            free_.insert(free_.begin() + static_cast<long>(index) + 1, remainder);
        }
        return true;
    }
    return false;
}

namespace {

/// Returns a range of the arena to the state it was reserved in: unreadable, unwritable,
/// backed by nothing, and able to be made writable again.
///
/// Not mprotect: a guest that mapped a file here left a mapping whose *maximum* protection
/// is whatever the file allowed, and a read-only file leaves a range that can never be
/// made writable again -- so the next program placed there fails to commit, with EACCES, a
/// long way from the mmap that caused it. Only ever used on memory nothing holds any more:
/// mapping over a range also zeroes it, and the guest's 4KB pages share this device's 16KB
/// ones, so doing it to a range that is merely being re-protected would wipe up to three
/// live neighbours -- which reads, much later, as a thread whose stack pointer is zero.
bool MapFreshAnonymous(uint64_t address, uint64_t length) {
    void* placed = mmap(reinterpret_cast<void*>(address), length, PROT_NONE,
                        MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS, kArenaTag, 0);
    return placed != MAP_FAILED && reinterpret_cast<uint64_t>(placed) == address;
}



/// Makes [begin, end) usable at `host_protection`, with anything not already spoken for
/// reading as zero.
///
/// Linux hands out zeroed pages, and a dynamic loader depends on that completely. The
/// arena is reused as processes come and go, so a range handed out again still holds the
/// last owner's bytes unless something clears it -- and mapping over it is how that is
/// done, because it also restores the protection ceiling a mapped file left behind.
///
/// Only over pages nothing else is living in, though. The guest's pages are 4KB and this
/// device's are 16KB, so a span rounded outward to host pages can cover up to three
/// neighbouring guest pages that are still in use; mapping over those wipes them, and it
/// reads much later as a thread whose stack pointer and saved registers are all zero.
bool CommitRange(uint64_t begin, uint64_t end, uint64_t page_size, int host_protection,
                 const std::vector<GuestRange>& committed) {
    // Zeroed only where nothing lives. Walking the sorted list of what is committed once,
    // rather than asking it about every host page in turn, is what keeps committing a
    // 128MB heap from costing a scan of the whole arena per page.
    uint64_t cursor = begin;
    const auto fill = [&](uint64_t gap_begin, uint64_t gap_end) {
        const uint64_t low = (gap_begin + page_size - 1) & ~(page_size - 1);
        const uint64_t high = gap_end & ~(page_size - 1);
        if (high > low) {
            MapFreshAnonymous(low, high - low);
        }
    };
    for (const auto& range : committed) {
        if (range.end() <= cursor) {
            continue;
        }
        if (range.begin >= end) {
            break;
        }
        if (range.begin > cursor) {
            fill(cursor, range.begin);
        }
        cursor = std::max(cursor, range.end());
        if (cursor >= end) {
            break;
        }
    }
    if (cursor < end) {
        fill(cursor, end);
    }
    // Whole host pages: a fixed mapping's free part starts where its committed
    // neighbour stops, at a guest page, and Darwin refuses a protection change that
    // does not start on one of its own pages -- which a browser committing its reserved
    // heap saw as ENOMEM, and died of. The neighbour sharing the page gets this
    // protection too, as it does from every other protection change here.
    const uint64_t low = begin & ~(page_size - 1);
    const uint64_t high = (end + page_size - 1) & ~(page_size - 1);
    return mprotect(reinterpret_cast<void*>(low), high - low, host_protection) == 0;
}

} // namespace

void GuestAddressSpace::ReturnFreeExtent(uint64_t address, uint64_t size) {
    if (size == 0) {
        return;
    }
    // Part of the range may already be free (a heap that gave up its tail, a guest that unmapped a piece of a library):
    // free the rest of it. Giving up on the whole range instead leaves it reserved for good -- an exec then keeps the old
    // program's heap, and the new program cannot have those addresses.
    for (const auto& extent : free_) {
        if (extent.begin < address + size && address < extent.end()) {
            std::vector<std::pair<uint64_t, uint64_t>> pieces;
            uint64_t cursor = address;
            for (const auto& other : free_) {
                if (other.end() <= cursor || other.begin >= address + size) {
                    continue;
                }
                if (other.begin > cursor) {
                    pieces.emplace_back(cursor, other.begin - cursor);
                }
                cursor = std::max(cursor, other.end());
            }
            if (cursor < address + size) {
                pieces.emplace_back(cursor, address + size - cursor);
            }
            for (const auto& [begin, length] : pieces) {
                ReturnFreeExtent(begin, length);
            }
            return;
        }
    }
    Extent freed {address, size};
    auto position = std::lower_bound(free_.begin(), free_.end(), freed,
                                     [](const Extent& lhs, const Extent& rhs) { return lhs.begin < rhs.begin; });
    position = free_.insert(position, freed);

    // Coalesce with the neighbour on each side so a long-running guest that mmaps and
    // munmaps repeatedly does not shred the arena into unusable slivers.
    if (position + 1 != free_.end() && position->end() == (position + 1)->begin) {
        position->size += (position + 1)->size;
        free_.erase(position + 1);
    }
    if (position != free_.begin() && (position - 1)->end() == position->begin) {
        (position - 1)->size += position->size;
        free_.erase(position);
    }
}

/// Takes whatever of [begin, end) is still free, without requiring all of it to be.
///
/// TakeFreeExtent is all-or-nothing, which is right for an allocation but wrong for a
/// fixed mapping: a loader reserves a library's whole span and then maps each segment
/// over the top, so the second mapping lands partly on its own memory and partly on free
/// space. Left in the free list, that free part is handed out again later -- to another
/// process -- and two programs end up living at the same address.
void GuestAddressSpace::TakeFreeSpan(uint64_t begin, uint64_t end) {
    for (size_t index = 0; index < free_.size();) {
        auto& extent = free_[index];
        if (extent.end() <= begin || extent.begin >= end) {
            ++index;
            continue;
        }
        const uint64_t head = begin > extent.begin ? begin - extent.begin : 0;
        const uint64_t tail = extent.end() > end ? extent.end() - end : 0;
        if (head == 0 && tail == 0) {
            free_.erase(free_.begin() + static_cast<long>(index));
            continue;
        }
        if (head == 0) {
            extent.begin = end;
            extent.size = tail;
            ++index;
            continue;
        }
        if (tail == 0) {
            extent.size = head;
            ++index;
            continue;
        }
        const Extent remainder {end, tail};
        extent.size = head;
        free_.insert(free_.begin() + static_cast<long>(index) + 1, remainder);
        index += 2;
    }
}

/// The parts of [begin, end) nothing is committed over, host page by host page.
std::vector<std::pair<uint64_t, uint64_t>> GuestAddressSpace::CommittedIn(uint64_t begin,
                                                                         uint64_t end) const {
    std::vector<std::pair<uint64_t, uint64_t>> pieces;
    std::shared_lock lock {mutex_};
    for (size_t index = FirstRangeEndingAfter(begin); index < committed_.size(); ++index) {
        const auto& range = committed_[index];
        if (range.begin >= end) {
            break;
        }
        pieces.emplace_back(std::max(range.begin, begin), std::min(range.end(), end));
    }
    return pieces;
}

void GuestAddressSpace::DropPages(uint64_t address, uint64_t size) {
    const uint64_t low = (address + page_size_ - 1) & ~(page_size_ - 1);
    const uint64_t high = (address + size) & ~(page_size_ - 1);
    if (high <= low || low < base_ || high > base_ + size_) {
        return;
    }
    std::unique_lock lock {mutex_};
    if (!MapFreshAnonymous(low, high - low) ||
        mprotect(reinterpret_cast<void*>(low), high - low, PROT_READ | PROT_WRITE) != 0) {
        UT_WARN("dropping pages at %#llx (%llu KB) failed: %s", static_cast<unsigned long long>(low),
                    static_cast<unsigned long long>((high - low) / 1024), std::strerror(errno));
    }
}

std::vector<std::pair<uint64_t, uint64_t>> GuestAddressSpace::UncommittedIn(uint64_t begin,
                                                                           uint64_t end) const {
    std::vector<std::pair<uint64_t, uint64_t>> gaps;
    uint64_t cursor = begin;
    for (size_t index = FirstRangeEndingAfter(cursor); index < committed_.size(); ++index) {
        const auto& range = committed_[index];
        if (range.begin >= end) {
            break;
        }
        if (range.begin > cursor) {
            gaps.emplace_back(cursor, std::min(range.begin, end));
        }
        cursor = std::max(cursor, range.end());
        if (cursor >= end) {
            return gaps;
        }
    }
    if (cursor < end) {
        gaps.emplace_back(cursor, end);
    }
    return gaps;
}

/// Records a committed range, and takes that address away from anything that claimed it
/// before.
///
/// Overlapping entries are not merely untidy. Everything that asks this list a question
/// about an address -- which mapping is here, is it writable, is it still the one a copy
/// was taken from -- gets a different answer depending on which entry it happens to reach
/// first, and a fork's snapshot ends up holding the same page four times over, each with
/// a different idea of when it was mapped. So the newcomer wins, and whatever was there
/// is clipped around it.
/// The first committed range that ends after `address`, found by bisection.
///
/// Every question this class answers about an address walks this list, and a long-running
/// guest has tens of thousands of ranges in it -- a loader maps and re-protects every
/// segment of every library. Walking from the front made a single pointer check cost more
/// than the syscall it was checking, with the address space's lock held for all of it, so
/// every other thread stopped too.
size_t GuestAddressSpace::FirstRangeEndingAfter(uint64_t address) const {
    size_t low = 0;
    size_t high = committed_.size();
    while (low < high) {
        const size_t middle = low + (high - low) / 2;
        if (committed_[middle].end() <= address) {
            low = middle + 1;
        } else {
            high = middle;
        }
    }
    return low;
}

void GuestAddressSpace::RecordCommitted(uint64_t address, uint64_t size, int protection) {
    if (committed_words_ != nullptr && size != 0 && address >= base_ &&
        address + size <= base_ + size_) {
        // Remembered one way only: a grain that has ever held guest memory stays marked,
        // so that a signal handler asking "could this address be the guest's?" is never
        // told no about memory that was simply freed.
        const uint64_t first = (address - base_) / code_grain_;
        const uint64_t last = (address + size - 1 - base_) / code_grain_;
        for (uint64_t grain = first; grain <= last && grain / 64 < code_word_count_; ++grain) {
            committed_words_[grain / 64].fetch_or(1ull << (grain % 64), std::memory_order_relaxed);
        }
    }
    const uint64_t end = address + size;
    std::vector<GuestRange> updated;
    updated.reserve(committed_.size() + 2);
    for (const auto& range : committed_) {
        if (range.end() <= address || range.begin >= end) {
            updated.push_back(range);
            continue;
        }
        // Something was already living here. The arena only hands out what its free list
        // says is free, so this means the free list and this one disagree -- and the
        // program that was here is about to find its memory belongs to somebody else.
        UT_WARN("arena: committing %#llx..%#llx over %#llx..%#llx, which was already "
                    "committed (epoch %llu, prot %d)",
                    static_cast<unsigned long long>(address), static_cast<unsigned long long>(end),
                    static_cast<unsigned long long>(range.begin),
                    static_cast<unsigned long long>(range.end()),
                    static_cast<unsigned long long>(range.epoch), range.protection);
        if (range.begin < address) {
            updated.push_back(GuestRange {range.begin, address - range.begin, range.protection,
                                          range.epoch, range.written});
        }
        if (range.end() > end) {
            updated.push_back(GuestRange {end, range.end() - end, range.protection, range.epoch, range.written});
        }
    }
    updated.push_back(GuestRange {address, size, protection, next_epoch_++, (protection & kGuestProtWrite) != 0});
    std::sort(updated.begin(), updated.end(),
              [](const GuestRange& lhs, const GuestRange& rhs) { return lhs.begin < rhs.begin; });
    committed_ = std::move(updated);
    Coalesce();
}

/// Joins neighbours that are the same mapping in two pieces.
///
/// Protecting part of a range splits it, and a loader protects every segment of every
/// library it maps, so without this the list only ever grows -- and everything that reads
/// it slows down together. Two ranges are only joined when they touch and agree on both
/// their protection and which mapping they came from, so nothing that reads an epoch can
/// tell the difference.
void GuestAddressSpace::Coalesce() {
    size_t out = 0;
    for (size_t index = 0; index < committed_.size(); ++index) {
        if (out > 0 && committed_[out - 1].end() == committed_[index].begin &&
            committed_[out - 1].protection == committed_[index].protection &&
            committed_[out - 1].written == committed_[index].written &&
            committed_[out - 1].epoch == committed_[index].epoch) {
            committed_[out - 1].size += committed_[index].size;
            continue;
        }
        committed_[out] = committed_[index];
        ++out;
    }
    committed_.resize(out);
}

uint64_t GuestAddressSpace::Allocate(uint64_t size, uint64_t hint, int protection) {
    if (size == 0) {
        return 0;
    }

    // Anything this hands out is memory with new contents, whoever had it before. Blocks
    // compiled from what used to be there have to go, or the next program to run at that
    // address runs the last one's code -- which does not look like a stale cache from the
    // outside, it looks like a jump into nonsense. Collected here and done once the lock
    // is dropped, because the observer goes into FEXCore and FEXCore asks this class
    // questions.
    uint64_t fresh_begin = 0;
    uint64_t fresh_end = 0;
    struct Notify {
        GuestAddressSpace* space;
        uint64_t* begin;
        uint64_t* end;
        ~Notify() {
            if (*end > *begin && space->release_observer_ != nullptr) {
                space->release_observer_(*begin, *end);
            }
        }
    } notify {this, &fresh_begin, &fresh_end};

    std::unique_lock lock {mutex_};
    const uint64_t length = AlignUp(size);

    if (hint != 0) {
        const uint64_t aligned_hint = AlignDown(hint);
        if (aligned_hint >= base_ && aligned_hint + length <= base_ + size_ &&
            TakeFreeExtent(aligned_hint, length)) {
            if (!CommitRange(aligned_hint, aligned_hint + length, page_size_,
                             ToHostProtection(protection), committed_)) {
                UT_WARN("commit at hint %#llx failed: %s",
                            static_cast<unsigned long long>(aligned_hint), std::strerror(errno));
                ReturnFreeExtent(aligned_hint, length);
                return 0;
            }
            RecordCommitted(aligned_hint, length, protection);
            if (HasHeldCode(aligned_hint, aligned_hint + length)) {
                fresh_begin = aligned_hint;
                fresh_end = aligned_hint + length;
            }
            lock.unlock();
            return aligned_hint;
        }
    }

    // First fit. The arena is large and guest allocation patterns are not adversarial,
    // so the extra bookkeeping of best fit is not worth it here. Above the floor if there
    // is room there, anywhere otherwise.
    for (int pass = floor_ != 0 ? 0 : 1; pass < 2; ++pass)
    for (const auto& extent : free_) {
        const uint64_t from = pass == 0 ? std::max(extent.begin, AlignUp(floor_)) : extent.begin;
        if (from >= extent.end() || extent.end() - from < length) {
            continue;
        }
        const uint64_t address = from;
        if (!TakeFreeExtent(address, length)) {
            continue;
        }
        if (!CommitRange(address, address + length, page_size_,
                         ToHostProtection(protection), committed_)) {
            UT_WARN("commit of %llu bytes at %#llx failed: %s (arena %#llx..%#llx)",
                        static_cast<unsigned long long>(length),
                        static_cast<unsigned long long>(address), std::strerror(errno),
                        static_cast<unsigned long long>(base_),
                        static_cast<unsigned long long>(base_ + size_));
            ReturnFreeExtent(address, length);
            return 0;
        }
        RecordCommitted(address, length, protection);
        if (HasHeldCode(address, address + length)) {
            fresh_begin = address;
            fresh_end = address + length;
        }
        lock.unlock();
        return address;
    }

    uint64_t total = 0;
    uint64_t largest = 0;
    for (const auto& extent : free_) {
        total += extent.size;
        largest = std::max(largest, extent.size);
    }
    UT_WARN("guest arena exhausted: no free extent for %llu KB; %llu KB free in %llu "
                "pieces, largest %llu KB",
                static_cast<unsigned long long>(length / 1024),
                static_cast<unsigned long long>(total / 1024),
                static_cast<unsigned long long>(free_.size()),
                static_cast<unsigned long long>(largest / 1024));
    for (const auto& extent : free_) {
        UT_INFO("arena:   free %#llx..%#llx (%llu KB)", static_cast<unsigned long long>(extent.begin),
                    static_cast<unsigned long long>(extent.end()), static_cast<unsigned long long>(extent.size / 1024));
    }
    return 0;
}

bool GuestAddressSpace::CommitFixed(uint64_t address, uint64_t size, int protection) {
    if (size == 0) {
        return false;
    }

    std::unique_lock lock {mutex_};
    const uint64_t begin = AlignDown(address);
    const uint64_t end = AlignUp(address + size);
    if (begin < base_ || end > base_ + size_) {
        return false;
    }
    if (!TakeFreeExtent(begin, end - begin)) {
        // Partly committed already, which is the ordinary case: a loader reserves a
        // library's whole span and then maps each of its segments over the top. The parts
        // that are still free are taken and committed here; the parts that are not keep
        // what they have, widened -- ELF segments routinely share a host page with the
        // segment before them, and taking access away from a neighbour that was never
        // mentioned is how a loader's own data goes read-only underneath it.
        for (const auto& [gap_begin, gap_end] : UncommittedIn(begin, end)) {
            TakeFreeSpan(gap_begin, gap_end);
            if (!CommitRange(gap_begin, gap_end, page_size_, ToHostProtection(protection),
                             committed_)) {
                UT_WARN("fixed commit of the free part at %#llx failed: %s",
                            static_cast<unsigned long long>(gap_begin), std::strerror(errno));
                continue;
            }
            RecordCommitted(gap_begin, gap_end - gap_begin, protection);
        }
        bool gained_exec = false;
        return ProtectLocked(begin, end - begin, protection, gained_exec);
    }
    if (!CommitRange(begin, end, page_size_, ToHostProtection(protection), committed_)) {
        UT_ERROR("fixed commit at %#llx (%llu bytes) failed: %s",
                     static_cast<unsigned long long>(begin),
                     static_cast<unsigned long long>(end - begin), std::strerror(errno));
        ReturnFreeExtent(begin, end - begin);
        return false;
    }
    RecordCommitted(begin, end - begin, protection);
    lock.unlock();
    // As in Allocate: only where the guest can execute.
    if (HasHeldCode(begin, end) && release_observer_ != nullptr) {
        release_observer_(begin, end);
    }
    return true;
}

bool GuestAddressSpace::Release(uint64_t address, uint64_t size) {
    if (size == 0) {
        return false;
    }

    std::unique_lock lock {mutex_};
    // In the guest's pages, not the host's. Committed ranges are recorded in host pages, so
    // a 4KB mapping owns the 16KB it was committed in, and an unmap that starts in the
    // middle of that host page -- 64-bit Wine commits its heap's first page at 0x10000 and
    // then unmaps from 0x11000 up -- must leave the part below it alone. Rounded out to the
    // host's pages it took the heap with it.
    constexpr uint64_t kGuestPage = 0x1000;
    const uint64_t begin = address & ~(kGuestPage - 1);
    const uint64_t end = (address + size + kGuestPage - 1) & ~(kGuestPage - 1);
    if (begin < base_ || end > base_ + size_) {
        return false;
    }

    std::vector<GuestRange> survivors;
    survivors.reserve(committed_.size());
    // Whether anything being given up here was ever something the guest could execute.
    // Only then is there compiled code to throw away, and throwing it away stops every
    // thread in the session for the duration -- Chromium's allocator unmaps constantly,
    // and doing it for each of those unmaps is most of what the session spends its time
    // on.
    bool had_code = false;
    for (const auto& range : committed_) {
        if (range.end() <= begin || range.begin >= end) {
            survivors.push_back(range);
            continue;
        }
        had_code = true;
        if (range.begin < begin) {
            survivors.push_back(GuestRange {range.begin, begin - range.begin, range.protection, range.epoch, range.written});
        }
        if (range.end() > end) {
            survivors.push_back(GuestRange {end, range.end() - end, range.protection, range.epoch, range.written});
        }
    }
    // Installed before anything below asks what is still committed -- both the sweep for
    // pages nothing lives in and the decision about what goes back on the free list read
    // this list, and reading it with the range still in it means nothing is ever dropped
    // and the free list ends up holding memory the committed list also holds.
    committed_ = std::move(survivors);
    Coalesce();

    // Only the host pages nothing else is still living in.
    //
    // The guest's pages are 4KB and the host's are 16KB, so one host page can hold four
    // guest mappings. Protecting the whole rounded span takes a neighbour's memory with
    // it, and the neighbour finds out by faulting on an ordinary store -- a thread stack
    // sharing its last host page with something being freed dies sixteen bytes below its
    // own stack pointer. So each host page in the span is protected only if nothing that
    // survived this release still overlaps it.
    // In runs, not page by page. Each mprotect splits the kernel's map of this process,
    // and a 128MB heap released one 16KB page at a time is eight thousand of them; a few
    // of those and Darwin stops accepting mprotect at all, with EACCES, which surfaces
    // much later as a program that cannot be loaded. Consecutive pages that are all free
    // go in one call, which is almost always the whole span.
    // Page by page would mean scanning every committed range for every 16KB of a 128MB
    // heap, with this lock held; the gaps are computed once instead. Over the host pages the
    // range touches, so a host page whose other guest pages were already free goes too.
    for (const auto& [gap_begin, gap_end] : UncommittedIn(AlignDown(begin), AlignUp(end))) {
        // Inward: a host page only half of which is free still holds somebody's memory,
        // and mapping over it would take theirs away. The guest's pages are 4KB and the
        // host's 16KB, so this happens constantly.
        const uint64_t low = AlignUp(gap_begin);
        const uint64_t high = AlignDown(gap_end);
        if (high <= low) {
            continue;
        }
        // Mapped fresh rather than protected, which returns the range to exactly the state
        // the arena was reserved in. Protecting is not enough: a guest that mapped a file
        // here left a mapping whose *maximum* protection is the file's, and a read-only
        // file gives a range that can never be made writable again -- so the next program
        // loaded at that address fails to commit, with EACCES, a long way from the mmap
        // that caused it. This also drops the pages, which is what the madvise was for.
        if (!MapFreshAnonymous(low, high - low)) {
            UT_WARN("release at %#llx (%llu KB) failed: %s",
                        static_cast<unsigned long long>(low),
                        static_cast<unsigned long long>((high - low) / 1024), std::strerror(errno));
        }
    }

    // And only the parts of it nothing else is still living in. A guest page is 4KB and a
    // host page 16KB, so the rounding above reaches up to three neighbouring guest pages
    // that are still in use; handing those back as free lets the arena allocate them to
    // somebody else while their owner is still running there.
    // Whole host pages only: the arena hands memory out in host pages, and a piece of one
    // whose other part is still committed cannot be handed out on its own. It goes back
    // when the rest of its host page does.
    for (const auto& [free_begin, free_end] : UncommittedIn(AlignDown(begin), AlignUp(end))) {
        const uint64_t low = AlignUp(free_begin);
        const uint64_t high = AlignDown(free_end);
        if (high > low) {
            ReturnFreeExtent(low, high - low);
        }
    }

    // Outside the lock: the observer goes into FEXCore, which asks this address space
    // about ranges while it invalidates, and would deadlock on the lock just released.
    lock.unlock();
    if (had_code && release_observer_ != nullptr) {
        release_observer_(begin, end);
    }
    return true;
}

bool GuestAddressSpace::Protect(uint64_t address, uint64_t size, int protection) {
    bool gained_exec = false;
    bool changed = false;
    {
        std::unique_lock lock {mutex_};
        changed = ProtectLocked(address, size, protection, gained_exec);
    }
    // Code can arrive at an address by being mapped there and then made executable, and
    // the blocks compiled from whatever used to be there have to go with it. Outside the
    // lock, because the observer goes into FEXCore and FEXCore asks this class questions.
    if (changed && HasHeldCode(AlignDown(address), AlignUp(address + size)) &&
        release_observer_ != nullptr) {
        release_observer_(AlignDown(address), AlignUp(address + size));
    }
    return changed;
}

// Callers already holding the lock use this directly. The list of committed ranges is
// rebuilt here, so a guest mprotect running while another thread commits or releases
// memory frees the vector's storage out from under whoever is walking it -- which shows
// up as a malloc abort a long way from the thread that caused it.
bool GuestAddressSpace::ProtectLocked(uint64_t address, uint64_t size, int protection,
                                     bool& gained_exec) {
    const uint64_t begin = AlignDown(address);
    const uint64_t end = AlignUp(address + size);

    // The guest's pages are 4KB and this device's are 16KB, so a guest range rarely lands
    // on a host page boundary. Rounding outward and applying the new protection to the
    // whole of it would take access away from up to three neighbouring guest pages that
    // were never mentioned -- which is exactly what happens when a dynamic loader marks
    // its relocated data read-only and quietly strips write access from the start of the
    // segment that follows.
    //
    // Only host pages lying wholly inside the request get the protection as asked. The
    // partial pages at either end keep whatever they already allowed, widened by the new
    // protection, because a page shared by two guest pages has to satisfy both.
    const uint64_t strict_begin = AlignUp(address);
    const uint64_t strict_end = AlignDown(address + size);
    const auto protection_for = [&](uint64_t range_begin, uint64_t range_end, int existing) {
        const bool wholly_inside = range_begin >= strict_begin && range_end <= strict_end;
        return wholly_inside ? protection : (existing | protection);
    };

    std::vector<GuestRange> updated;
    updated.reserve(committed_.size() + 2);
    bool touched = false;
    for (const auto& range : committed_) {
        if (range.end() <= begin || range.begin >= end) {
            updated.push_back(range);
            continue;
        }
        touched = true;
        if (range.begin < begin) {
            updated.push_back(GuestRange {range.begin, begin - range.begin, range.protection, range.epoch, range.written});
        }
        const uint64_t overlap_begin = std::max(range.begin, begin);
        const uint64_t overlap_end = std::min(range.end(), end);
        updated.push_back(GuestRange {overlap_begin, overlap_end - overlap_begin,
                                      protection_for(overlap_begin, overlap_end, range.protection),
                                      range.epoch, range.written || (protection & kGuestProtWrite) != 0});
        if (range.end() > end) {
            updated.push_back(GuestRange {end, range.end() - end, range.protection, range.epoch, range.written});
        }
    }
    if (!touched) {
        return false;
    }

    std::sort(updated.begin(), updated.end(),
              [](const GuestRange& lhs, const GuestRange& rhs) { return lhs.begin < rhs.begin; });
    committed_ = std::move(updated);
    Coalesce();

    // The host mapping keeps read/write regardless; only the guest's view changes.
    // A guest mprotect that *adds* write access still needs the host to allow it.
    if ((protection & kGuestProtWrite) != 0) {
        mprotect(reinterpret_cast<void*>(begin), end - begin, PROT_READ | PROT_WRITE);
    }
    gained_exec = (protection & kGuestProtExec) != 0;
    return true;
}

std::vector<GuestRange> GuestAddressSpace::WritableRanges() const {
    std::shared_lock lock {mutex_};
    std::vector<GuestRange> writable;
    writable.reserve(committed_.size());
    for (const auto& range : committed_) {
        if ((range.protection & kGuestProtWrite) != 0) {
            writable.push_back(range);
        }
    }
    return writable;
}

std::vector<GuestRange> GuestAddressSpace::WritableRangesIn(uint64_t begin, uint64_t end) const {
    std::shared_lock lock {mutex_};
    std::vector<GuestRange> writable;
    for (size_t index = FirstRangeEndingAfter(begin); index < committed_.size(); ++index) {
        const auto& range = committed_[index];
        if (range.begin >= end) {
            break;
        }
        if ((range.protection & kGuestProtWrite) == 0) {
            continue;
        }
        const uint64_t low = std::max(range.begin, begin);
        const uint64_t high = std::min(range.end(), end);
        if (high > low) {
            writable.push_back(GuestRange {low, high - low, range.protection, range.epoch, range.written});
        }
    }
    return writable;
}

void GuestAddressSpace::FreeSpace(uint64_t* total, uint64_t* largest) const {
    std::shared_lock lock {mutex_};
    uint64_t sum = 0;
    uint64_t biggest = 0;
    for (const auto& extent : free_) {
        sum += extent.size;
        biggest = std::max(biggest, extent.size);
    }
    if (total != nullptr) {
        *total = sum;
    }
    if (largest != nullptr) {
        *largest = biggest;
    }
}

bool GuestAddressSpace::RestoreIfUnchanged(uint64_t address, const void* bytes, uint64_t size,
                                           uint64_t epoch, const char** refusal,
                                           bool* held_code) {
    static thread_local char why[192];
    const auto refuse = [&](const char* what, const GuestRange* range) {
        if (refusal != nullptr) {
            if (range != nullptr) {
                std::snprintf(why, sizeof(why), "%s (wanted epoch %llu, %#llx..%#llx is epoch %llu prot %d)",
                              what, static_cast<unsigned long long>(epoch),
                              static_cast<unsigned long long>(range->begin),
                              static_cast<unsigned long long>(range->end()),
                              static_cast<unsigned long long>(range->epoch), range->protection);
            } else {
                std::snprintf(why, sizeof(why), "%s", what);
            }
            *refusal = why;
        }
        return false;
    };
    std::shared_lock lock {mutex_};
    uint64_t cursor = address;
    const uint64_t end = address + size;
    for (size_t index = FirstRangeEndingAfter(cursor); index < committed_.size(); ++index) {
        const auto& range = committed_[index];
        if (range.begin > cursor) {
            return refuse("nothing is mapped there any more", &range);
        }
        if (range.epoch != epoch) {
            return refuse("a different mapping is there now", &range);
        }
        if ((range.protection & kGuestProtWrite) == 0) {
            return refuse("it is no longer writable", &range);
        }
        cursor = range.end();
        if (cursor >= end) {
            // The arena saying a range is writable is not the host saying so. A guest that
            // mapped a file here left a mapping whose maximum protection the file decided,
            // and a guest mprotect that widened it afterwards failed quietly. Writing
            // anyway is a protection fault inside this memcpy, which kills the session --
            // so the host is asked first, and told no is a reason to leave the copy alone.
            const uint64_t page_begin = AlignDown(address);
            const uint64_t page_end = AlignUp(end);
            if (mprotect(reinterpret_cast<void*>(page_begin), page_end - page_begin,
                         PROT_READ | PROT_WRITE) != 0) {
                return refuse("the host will not allow writing there", nullptr);
            }
            std::memcpy(reinterpret_cast<void*>(address), bytes, size);
            // The bytes here are no longer the bytes that were here a moment ago, and the
            // child whose memory this overwrites has been running -- and compiling -- in
            // it. Compiled code from this span is stale, and nothing else on this path
            // releases or reprotects the range to say so; missing it shows up much later
            // as FEXCore refusing to decode an instruction at an address that looked
            // fine, in a process that has already moved on.
            if (held_code != nullptr) {
                *held_code = HasHeldCode(page_begin, page_end);
            }
            return true;
        }
    }
    return refuse("the mapping ends short of the copy", nullptr);
}

uint64_t GuestAddressSpace::EpochAt(uint64_t address) const {
    std::shared_lock lock {mutex_};
    const size_t index = FirstRangeEndingAfter(address);
    if (index < committed_.size() && committed_[index].begin <= address) {
        return committed_[index].epoch;
    }
    return 0;
}

bool GuestAddressSpace::Contains(uint64_t address, uint64_t size) const {
    return address >= base_ && size <= size_ && address + size <= base_ + size_;
}

bool GuestAddressSpace::Validate(uint64_t address, uint64_t size, int required) const {
    if (size == 0) {
        return true;
    }
    if (address > UINT64_MAX - size) {
        return false;
    }

    std::shared_lock lock {mutex_};
    uint64_t cursor = address;
    const uint64_t end = address + size;
    for (size_t index = FirstRangeEndingAfter(cursor); index < committed_.size(); ++index) {
        const auto& range = committed_[index];
        if (range.begin > cursor) {
            return false; // Hole.
        }
        if ((range.protection & required) != required) {
            return false;
        }
        cursor = range.end();
        if (cursor >= end) {
            return true;
        }
    }
    return false;
}

bool GuestAddressSpace::RangeFor(uint64_t address, GuestRange* out) const {
    std::shared_lock lock {mutex_};
    for (const auto& range : committed_) {
        if (address >= range.begin && address < range.end()) {
            if (out != nullptr) {
                *out = range;
            }
            return true;
        }
    }
    return false;
}

bool GuestAddressSpace::RangeForNoWait(uint64_t address, GuestRange* out, bool* known) const {
    if (known != nullptr) {
        *known = true;
    }
    std::shared_lock lock {mutex_, std::try_to_lock};
    if (!lock.owns_lock()) {
        if (known != nullptr) {
            *known = false;
        }
        return false;
    }
    const size_t index = FirstRangeEndingAfter(address);
    if (index >= committed_.size() || committed_[index].begin > address) {
        // Not mapped -- but the caller often wants to know by how much it missed, so the
        // neighbouring range is handed back anyway. A read four bytes past the end of a
        // mapping and a read through a pointer made of text are both "not mapped", and
        // they are completely different bugs.
        if (out != nullptr && index > 0) {
            *out = committed_[index - 1];
        } else if (out != nullptr && index < committed_.size()) {
            *out = committed_[index];
        }
        return false;
    }
    if (out != nullptr) {
        *out = committed_[index];
    }
    return true;
}

std::vector<GuestRange> GuestAddressSpace::Snapshot() const {
    std::shared_lock lock {mutex_};
    return committed_;
}

uint64_t GuestAddressSpace::CommittedBytes() const {
    std::shared_lock lock {mutex_};
    uint64_t total = 0;
    for (const auto& range : committed_) {
        total += range.size;
    }
    return total;
}

} // namespace utsuro
