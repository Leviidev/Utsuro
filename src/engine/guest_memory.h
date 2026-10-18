// guest_memory.h -- the guest's address space.
//
// FEXCore is a userspace emulator: a guest address *is* a host address, there is no
// translation layer. So the job here is to obtain host memory at addresses the guest
// can live at, and to remember what the guest believes about each range.
//
// Two host facts shape everything in this file:
//
//   * Apple Silicon's host page size is 16KB, while x86-64 guests assume 4KB. Guest
//     pages are therefore never mapped individually -- a whole region is mapped once
//     and sub-ranges are tracked logically. Asking the host for 4KB granularity would
//     fail, and silently assuming 4096 is exactly the bug FEX's own Darwin port had to
//     fix in half a dozen places.
//
//   * Guest "executable" is a bookkeeping property, not a host one. FEXCore reads guest
//     code as data and emits ARM64 for it, so guest code pages only need to be
//     host-*readable*. Nothing here ever asks for PROT_EXEC, which is what lets guest
//     memory work on iOS without any JIT entitlement at all (only FEXCore's own code
//     buffers need that).
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <utility>
#include <vector>

namespace utsuro {

/// Guest-side protection bits, matching Linux's PROT_* values.
enum GuestProtection : int {
    kGuestProtNone = 0,
    kGuestProtRead = 1,
    kGuestProtWrite = 2,
    kGuestProtExec = 4,
};

struct GuestRange {
    uint64_t begin {};
    uint64_t size {};
    int protection {};
    /// Which allocation this range belongs to. Two ranges at the same address with
    /// different epochs are different memory: the first was released and the address
    /// handed out again. A caller holding a copy of guest memory taken earlier can ask
    /// whether what is there now is still the same thing it copied.
    uint64_t epoch {};
    /// Whether the range has ever been writable, so may hold anything. A range that
    /// never was is a reservation the program has not used: nothing to copy for a fork.
    bool written {};

    uint64_t end() const { return begin + size; }
};

class GuestAddressSpace {
public:
    /// Reserves `size` bytes of contiguous address space without committing any of it.
    /// Returns nullptr and fills `error` on failure.
    static GuestAddressSpace* Reserve(uint64_t size, std::string& error, uint64_t alignment = 0);
    ~GuestAddressSpace();

    GuestAddressSpace(const GuestAddressSpace&) = delete;
    GuestAddressSpace& operator=(const GuestAddressSpace&) = delete;

    uint64_t Base() const { return base_; }
    uint64_t Size() const { return size_; }
    uint64_t HostPageSize() const { return page_size_; }

    /// Where the guest believes this arena starts.
    ///
    /// A 64-bit guest is mapped 1:1 and this is zero, so a guest address and a host
    /// address are the same number. A 32-bit guest cannot be: its pointers are 32 bits and
    /// the low 4GB of this process cannot be mapped, so the arena sits high in the host's
    /// address space while the guest sees it starting at zero. Everything crossing that
    /// boundary -- a pointer handed to a syscall, an address written into the auxiliary
    /// vector -- has to be converted.
    void SetGuestBase(uint64_t base) { guest_base_ = base; }
    /// Where an allocation that names no address starts looking. Below it is left for programs that ask for an
    /// address: 64-bit Wine reserves the low gigabytes for Windows' fixed addresses with MAP_FIXED, and anything
    /// placed there by default -- Wine's own executable, its libraries -- would be mapped over.
    void SetAllocationFloor(uint64_t address) { floor_ = address; }
    uint64_t GuestBase() const { return guest_base_; }

    uint64_t ToHost(uint64_t guest_address) const { return guest_address + guest_base_; }
    uint64_t ToGuest(uint64_t host_address) const { return host_address - guest_base_; }

    /// Reserves and commits `size` bytes, honouring `hint` when it is inside the arena
    /// and free. Returns 0 when the arena cannot satisfy it.
    uint64_t Allocate(uint64_t size, uint64_t hint, int protection);

    /// Commits an explicit range already known to be inside the arena -- used by the
    /// loader, which has to place segments at addresses the ELF dictates.
    bool CommitFixed(uint64_t address, uint64_t size, int protection);

    /// Takes ownership of a mapping the loader made *outside* the arena, which is the
    /// only way to honour a non-PIE guest's fixed load address. The range becomes guest
    /// memory for every purpose (validation, executable-range queries) and is unmapped
    /// when the address space is torn down.
    bool AdoptExternalMapping(uint64_t address, uint64_t size, int protection);
    /// Records a mapping made inside the arena by other means -- a copy-on-write remap
    /// of another process's memory -- so the allocator never hands the span out and the
    /// guest can protect and release it like anything else.
    bool AdoptCopiedMapping(uint64_t address, uint64_t size, int protection, bool written = true);

    /// The parts of [begin, end) that are committed right now. What a fixed mapping is
    /// about to be placed over asks this first: those parts hold the old contents and
    /// have to be zeroed by hand, where the free parts come back from the kernel as
    /// fresh zero pages and must not be touched -- touching them is what makes a
    /// gigabyte reservation cost a gigabyte.
    std::vector<std::pair<uint64_t, uint64_t>> CommittedIn(uint64_t begin, uint64_t end) const;

    /// Makes a committed, host-page-aligned range read as zero by mapping it fresh, so
    /// the pages it held are given back rather than written over -- an anonymous mapping
    /// placed over memory the program never touched costs nothing this way, where
    /// zeroing it by hand made every page real. Left readable and writable; the caller
    /// applies the protection it wants afterwards, as it does after a commit.
    void DropPages(uint64_t address, uint64_t size);

    /// Releases a guest range. Host pages are returned to the kernel; the address range
    /// goes back on the free list.
    bool Release(uint64_t address, uint64_t size);

    /// Records a new guest protection for a range without changing host protections.
    /// See this file's header for why the host side deliberately stays readable/writable.
    bool Protect(uint64_t address, uint64_t size, int protection);


    bool Contains(uint64_t address, uint64_t size) const;

    /// True when every byte of [address, address+size) is committed and satisfies `required`.
    bool Validate(uint64_t address, uint64_t size, int required) const;

    /// The committed range containing `address`, or nullptr. Used to answer FEXCore's
    /// QueryGuestExecutableRange, which it asks before compiling a block.
    bool RangeFor(uint64_t address, GuestRange* out) const;

    /// The same, but gives up rather than waiting for the lock. For a signal handler,
    /// which may be running on a thread that is already inside this class and must never
    /// block on a lock its own thread holds.
    /// `known` is set to false when the lock was busy and no answer could be given; the
    /// caller must not read "nothing is mapped there" into that.
    bool RangeForNoWait(uint64_t address, GuestRange* out, bool* known = nullptr) const;

    /// Every committed range the guest can write to. A fork uses this to know what it has
    /// to preserve on the parent's behalf: the image and the stack are only part of it,
    /// and a libc that allocates through mmap rather than brk keeps its heap somewhere
    /// this is the only way to find.
    std::vector<GuestRange> WritableRanges() const;

    /// The writable parts of [begin, end), clipped to it. A caller that wants to copy a
    /// span of guest memory has to ask, because the span it names may be partly read-only
    /// and partly not mapped at all, and only the writable parts can be written back.
    std::vector<GuestRange> WritableRangesIn(uint64_t begin, uint64_t end) const;

    /// Told whenever a range stops being what it was, so that anything caching something
    /// derived from guest memory -- compiled code, most of all -- can drop it. Called
    /// with the address space's own lock released.
    using ReleaseObserver = void (*)(uint64_t host_begin, uint64_t host_end);
    void SetReleaseObserver(ReleaseObserver observer) { release_observer_ = observer; }

    /// Says that guest code is about to be compiled from [begin, end), and answers
    /// whether any part of it had already been. Called when the JIT asks what it may
    /// compile, which is the only moment this is actually knowable: the guest's own
    /// protection bits do not decide it, because FEXCore will compile from any mapping
    /// this address space admits to having.
    ///
    /// Throwing compiled code away is the most expensive thing the runtime asks FEXCore
    /// to do -- it takes a lock that gives writers priority, so one of them stops every
    /// thread in the session that wants to compile -- and arena space nothing has ever
    /// compiled from has nothing to throw away. Coarse and one-way on purpose: a bit set
    /// that need not be costs an invalidation that was not needed, which is merely slow,
    /// while the reverse runs stale code.
    bool NoteExecutable(uint64_t begin, uint64_t end);

    /// The same question without marking anything.
    bool HasHeldCode(uint64_t begin, uint64_t end) const;

    /// Whether anything has ever been committed near `address`. Lock-free and coarse,
    /// for a signal handler: a false yes costs nothing, and a no is certain. The arena's
    /// whole span is reserved and host-readable whether or not the guest has been given
    /// any of it, so asking the kernel cannot tell a guest pointer from a wild one --
    /// only this can.
    bool MaybeCommitted(uint64_t address) const;

    /// Says that the bytes in this range are not the bytes that were there before, even
    /// though it was never released. A loader mapping a library's segments over a span it
    /// had already reserved does exactly this.
    void NotifyContentsReplaced(uint64_t begin, uint64_t end) const {
        if (release_observer_ != nullptr && end > begin) {
            release_observer_(begin, end);
        }
    }

    /// How much of the arena is still free, and the largest single run of it. Only for
    /// reporting: an allocation that fails wants to say whether the arena is full or
    /// merely shredded.
    void FreeSpace(uint64_t* total, uint64_t* largest) const;

    /// Puts a copy of guest memory back, if that memory is still the memory it was taken
    /// from. The check and the write happen together under this address space's own lock,
    /// which is the point: checking first and writing afterwards leaves a window in which
    /// another guest process -- which is not stopped, whatever the parent's own threads
    /// are doing -- releases the range and the write lands on nothing.
    /// `held_code`, when given, is set if the range that was overwritten had ever held
    /// guest code. The caller invalidates rather than this function, because the caller
    /// is holding the process table while it restores and throwing compiled code away
    /// waits on FEXCore -- which is a lock order that stops the whole session.
    bool RestoreIfUnchanged(uint64_t address, const void* bytes, uint64_t size, uint64_t epoch,
                            const char** refusal = nullptr, bool* held_code = nullptr);

    /// The allocation epoch of whatever covers `address`, or 0 if nothing does.
    uint64_t EpochAt(uint64_t address) const;

    std::vector<GuestRange> Snapshot() const;

    uint64_t CommittedBytes() const;

private:
    GuestAddressSpace(uint64_t base, uint64_t size, uint64_t page_size);

    struct Extent {
        uint64_t begin {};
        uint64_t size {};
        uint64_t end() const { return begin + size; }
    };

    uint64_t AlignDown(uint64_t value) const { return value & ~(page_size_ - 1); }
    uint64_t AlignUp(uint64_t value) const { return AlignDown(value + page_size_ - 1); }

    bool TakeFreeExtent(uint64_t address, uint64_t size);
    void ReturnFreeExtent(uint64_t address, uint64_t size);
    void RecordCommitted(uint64_t address, uint64_t size, int protection);
    void TakeFreeSpan(uint64_t begin, uint64_t end);
    size_t FirstRangeEndingAfter(uint64_t address) const;
    void Coalesce();
    std::vector<std::pair<uint64_t, uint64_t>> UncommittedIn(uint64_t begin, uint64_t end) const;

    bool ProtectLocked(uint64_t address, uint64_t size, int protection, bool& gained_exec);

    /// Marks [begin, end) as having held guest code, and says whether any part of it
    /// already had. Throwing compiled code away is the most expensive thing the runtime
    /// asks FEXCore to do -- it takes a lock that gives writers priority, so one of them
    /// stops every thread in the session that wants to compile -- and a library mapped
    /// into arena space nothing has ever executed from has no compiled code to throw
    /// away. Coarse and one-way on purpose: a bit that is set and should not be costs an
    /// invalidation that was not needed, which is merely slow, while the reverse runs
    /// stale code. Lock-free because it sits on the mmap path.

    std::unique_ptr<std::atomic<uint64_t>[]> code_words_;
    std::unique_ptr<std::atomic<uint64_t>[]> committed_words_;
    uint64_t code_grain_ {};
    uint64_t code_word_count_ {};


    ReleaseObserver release_observer_ {};
    uint64_t next_epoch_ {1};

    /// Shared, because nearly every use of this class is a question rather than a change:
    /// a pointer handed to a syscall is checked against it, and with thirty guest threads
    /// making syscalls at once an exclusive lock here is the whole session's speed limit.
    /// An X server sharing a session with Steam simply stopped answering.
    mutable std::shared_mutex mutex_;
    uint64_t base_ {};
    uint64_t size_ {};
    uint64_t page_size_ {};
    std::vector<Extent> free_;        ///< Sorted, coalesced, never overlapping.
    uint64_t guest_base_ {};            ///< 0 for a 1:1 (64-bit) guest.
    uint64_t floor_ {};                 ///< See SetAllocationFloor; 0 means anywhere.
    std::vector<GuestRange> committed_; ///< Sorted by begin.
    std::vector<Extent> external_;    ///< Mappings outside the arena, unmapped on destruction.
};

} // namespace utsuro
