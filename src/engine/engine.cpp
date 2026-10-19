#include <execinfo.h>
#include "engine.h"

#include "crash_handler.h"
#include "log.h"

#include <Common/Config.h>
#include <Common/HostFeatures.h>

#include <FEXCore/Config/Config.h>
#include <FEXCore/Core/Context.h>
#include <FEXCore/Core/CoreState.h>
#include <FEXCore/Core/SignalDelegator.h>
#include <FEXCore/Core/X86Enums.h>
#include <FEXCore/Debug/InternalThreadState.h>
#include <FEXCore/HLE/SyscallHandler.h>
#include <FEXCore/Utils/AllocatorHooks.h>
#include <FEXCore/Utils/ArchHelpers/Arm64.h>
#include <FEXCore/Utils/LogManager.h>
#include <FEXCore/Utils/LongJump.h>

#ifdef __APPLE__
// Darwin's <ucontext.h> hard-errors on the deprecated getcontext/setcontext declarations
// without this. Only the ucontext_t/mcontext_t types are wanted here, never those calls.
#define _XOPEN_SOURCE 1
#include <mach/arm/thread_status.h>
#include <mach/thread_act.h>
#include <ucontext.h>
#endif

#include <functional>
#include <string>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>
#include <condition_variable>
#include <vector>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

namespace utsuro {
namespace {

// FEXCore's own logging, routed into Fathom's single log path rather than stderr.
void ForwardFexMessage(LogMan::DebugLevels level, const char* message) {
    switch (level) {
    case LogMan::ASSERT:
    case LogMan::ERROR:
        UT_ERROR("fexcore: %s", message);
        break;
    case LogMan::DEBUG:
    case LogMan::INFO:
        UT_DEBUG("fexcore: %s", message);
        break;
    default:
        UT_INFO("fexcore: %s", message);
        break;
    }
}

void ForwardFexAssert(const char* message) {
    UT_ERROR("fexcore assertion: %s", message);
}

/// FEXCore's configuration is process-global, so it is set up once and torn down when
/// the last engine goes away rather than per session.
class ConfigLease {
public:
    static void Acquire(const EngineOptions& options) {
        std::scoped_lock lock {mutex_};
        if (users_ == 0) {
            FEX::Config::InitializeConfigs(FEX::Config::PortableInformation {});
            FEXCore::Config::Initialize();
            FEXCore::Config::Load();
            LogMan::Throw::InstallHandler(ForwardFexAssert);
            LogMan::Msg::InstallHandler(ForwardFexMessage);
        }
        ++users_;

        // The one setting that changes how every instruction is decoded.
        FEXCore::Config::Set(FEXCore::Config::CONFIG_IS64BIT_MODE, options.guest_is_32bit ? "0" : "1");
        FEXCore::Config::Set(FEXCore::Config::CONFIG_DISABLETELEMETRY, "1");
        FEXCore::Config::Set(FEXCore::Config::CONFIG_MULTIBLOCK, options.multiblock ? "1" : "0");
        FEXCore::Config::Set(FEXCore::Config::CONFIG_TSOENABLED, options.tso_enabled ? "1" : "0");
        FEXCore::Config::Set(FEXCore::Config::CONFIG_X87REDUCEDPRECISION,
                             options.reduced_precision_x87 ? "1" : "0");
        FEXCore::Config::Set(FEXCore::Config::CONFIG_X86DISASSEMBLE, options.disassemble ? "1" : "0");
        if (options.max_inst_per_block != 0) {
            FEXCore::Config::Set(FEXCore::Config::CONFIG_MAXINST,
                                 std::to_string(options.max_inst_per_block));
        }
    }

    static void Release() {
        std::scoped_lock lock {mutex_};
        if (users_ == 0) {
            return;
        }
        if (--users_ == 0) {
            FEXCore::Config::Shutdown();
            LogMan::Msg::UnInstallHandler();
            LogMan::Throw::UnInstallHandler();
        }
    }

private:
    static inline std::mutex mutex_;
    static inline size_t users_ {};
};

/// FEXCore needs somewhere to keep its call/return prediction stack, per guest thread.
class CallRetStack {
public:
    CallRetStack() {
        const auto page_size = static_cast<size_t>(sysconf(_SC_PAGESIZE));
        allocation_size_ = FEXCore::Core::InternalThreadState::CALLRET_STACK_SIZE + 2 * page_size;
        page_size_ = page_size;
        address_ = mmap(nullptr, allocation_size_, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (address_ == MAP_FAILED) {
            error_ = errno;
            return;
        }
        // A guard page on each side: an overflow of this stack should fault here rather
        // than quietly scribble over whatever the allocator put next to it.
        if (mprotect(Base(), FEXCore::Core::InternalThreadState::CALLRET_STACK_SIZE,
                     PROT_READ | PROT_WRITE) != 0) {
            error_ = errno;
        }
    }

    ~CallRetStack() {
        if (address_ != MAP_FAILED && address_ != nullptr) {
            munmap(address_, allocation_size_);
        }
    }

    CallRetStack(const CallRetStack&) = delete;
    CallRetStack& operator=(const CallRetStack&) = delete;

    bool valid() const { return address_ != MAP_FAILED && error_ == 0; }
    int error() const { return error_; }

    void Attach(FEXCore::Core::InternalThreadState* thread) const {
        thread->CallRetStackBase = Base();
        thread->CurrentFrame->State.callret_sp =
            reinterpret_cast<uint64_t>(Base()) + FEXCore::Core::InternalThreadState::CALLRET_STACK_SIZE / 4;
    }

private:
    void* Base() const { return static_cast<uint8_t*>(address_) + page_size_; }

    void* address_ {MAP_FAILED};
    size_t allocation_size_ {};
    size_t page_size_ {};
    int error_ {};
};

/// How many GDT slots the guest gets. FEXCore indexes this array with a selector's top
/// 13 bits, and Linux only ever hands userspace entries 12 through 14.
constexpr int kGdtEntries = 32;

/// The guest's segment descriptors. x86-64 barely uses segmentation, but the JIT still
/// reads a cached CS base, and leaving it unset produces wrong addresses rather than an
/// obvious failure.
class GuestSegments {
public:
    /// `inherit` is the descriptor table of the thread this one is being made from.
    ///
    /// A thread's descriptor table is its own -- it must not share its parent's array,
    /// because set_thread_area on one would move the other's TLS -- but its *contents* are
    /// inherited. clone without CLONE_SETTLS means "keep using the thread pointer you were
    /// made with", which is how a program that starts a thread with a raw clone rather
    /// than pthread_create gets its thread-local storage at all. Starting that thread with
    /// an empty table gives it a %gs base of zero, and the first thing it reads out of its
    /// own control block -- the function it was created to run -- comes back as whatever
    /// is at guest address zero.
    void Initialise(FEXCore::Core::CPUState& state, bool guest_is_32bit = false,
                    const GuestSegments* inherit = nullptr) {
        if (inherit != nullptr) {
            gdt_ = inherit->gdt_;
        }
        state.segment_arrays[FEXCore::Core::CPUState::SEGMENT_ARRAY_INDEX_GDT] = gdt_.data();
        state.segment_arrays[FEXCore::Core::CPUState::SEGMENT_ARRAY_INDEX_LDT] = gdt_.data();
        state.cs_idx = FEXCore::Core::CPUState::DEFAULT_USER_CS << 3;
        auto* code_segment = FEXCore::Core::CPUState::GetSegmentFromIndex(state, state.cs_idx);
        FEXCore::Core::CPUState::SetGDTBase(code_segment, 0);
        FEXCore::Core::CPUState::SetGDTLimit(code_segment, 0xF'FFFFU);
        state.cs_cached = FEXCore::Core::CPUState::CalculateGDTBase(*code_segment);
        // These two bits are how the guest's word size is actually decided. FEXCore's
        // decoder reads the mode straight off this descriptor -- `Is64BitMode = CSSegment->L`
        // -- and not from any configuration value, so leaving L set here makes a 32-bit
        // program get decoded as x86-64 no matter what else has been configured. The
        // symptom is an `int 0x80` refused as "32-bit syscall from a 64-bit process".
        code_segment->L = guest_is_32bit ? 0 : 1;
        code_segment->D = guest_is_32bit ? 1 : 0;
    }

private:
    std::array<FEXCore::Core::CPUState::gdt_segment, kGdtEntries> gdt_ {};
};

/// Serialises every creation and destruction of a FEXCore thread, across every context.
///
/// FEXCore's own CreateThread takes no locks, and the JIT core it builds claims a code
/// buffer from a pool shared by the whole process. Two of them at once leave one thread
/// holding a half-built dispatcher, and the first thing that thread does is call through
/// it -- a jump to address zero, on a thread whose only frame is the one that started it.
/// Fathom reaches this from three directions at once (a program starting, a thread
/// cloning, a process forking) and only one of those was ever under a lock.
std::mutex g_thread_lifecycle;

/// The FEXCore thread executing on this host thread, if any.
///
/// The alignment-fault handler needs it, and a signal handler cannot be passed context
/// any other way. Thread-local because FEXCore binds a guest thread to the host thread
/// running it, so this is exactly as wide as it needs to be.
struct ActiveExecution {
    FEXCore::Context::Context* context {};
    FEXCore::Core::InternalThreadState* thread {};
    /// The context's own signal delegator: its dispatcher addresses are what an in-place
    /// signal delivery jumps back through, and every process has its own context now.
    FEXCore::SignalDelegator* delegator {};
};
thread_local ActiveExecution g_active;

/// Counted so the log can say whether this mechanism is doing anything at all. A run
/// with thousands of these is working correctly; a run with zero means either the guest
/// never touched an odd address or the handler is not being reached.
std::atomic<uint64_t> g_alignment_fixups {0};

/// The guest's arena, so that a signal handler can tell a guest address from any other.
std::atomic<utsuro::GuestAddressSpace*> g_arena_space {nullptr};
std::atomic<uint64_t> g_arena_begin {0};

/// Declared here and defined below: the fault handler wants to name the guest process
/// it is reporting on, and the definition sits with the rest of the syscall plumbing.
extern thread_local SyscallSink* g_current_syscalls;
size_t DescribeGuestState(char* buffer, size_t capacity);

/// How many times this exact instruction has faulted, roughly.
///
/// A few hundred slots, indexed by address, with no locking and no exactness: two sites
/// can land in the same slot and the count then belongs to neither of them, which costs
/// a site the cheaper patch it might have kept. That is the right trade for something on
/// the fault path, where a mutex would be both a deadlock risk and the cost being
/// measured.
uint32_t RepeatedFaultSite(uintptr_t pc) {
    constexpr size_t kSlots = 512;
    struct Slot {
        std::atomic<uintptr_t> pc {0};
        std::atomic<uint32_t> count {0};
    };
    static Slot slots[kSlots];
    auto& slot = slots[(pc >> 2) % kSlots];
    if (slot.pc.load(std::memory_order_relaxed) != pc) {
        slot.pc.store(pc, std::memory_order_relaxed);
        slot.count.store(1, std::memory_order_relaxed);
        return 1;
    }
    return slot.count.fetch_add(1, std::memory_order_relaxed) + 1;
}

/// Fixes up a guest alignment fault and resumes, rather than letting it kill the app.
///
/// x86 lets a program read or write at any address. ARM64 mostly does too -- but not for
/// the acquire/release and atomic instructions FEXCore emits to reproduce x86's memory
/// ordering, which fault unless the address is naturally aligned. So an ordinary
/// unaligned store in ordinary guest code arrives here as SIGBUS, and this is the
/// difference between "runs x86 programs" and "runs x86 programs that never touch an odd
/// address". FEXCore provides the fixup; what is needed here is to recognise the fault,
/// call it, and resume at the instruction it says to resume at.
bool RecoverAlignmentFault(int signal, siginfo_t* info, void* raw_context) {
#if defined(__aarch64__) && defined(__APPLE__)
    if ((signal != SIGBUS && signal != SIGSEGV) || info == nullptr || raw_context == nullptr) {
        return false;
    }
    if (g_active.context == nullptr || g_active.thread == nullptr) {
        return false;
    }
    // The call/return stack ran off either end. Inside compiled code the host stack
    // pointer is FEXCore's call/return stack -- a cache of return addresses that a CALL
    // pushes and a RET pops -- and a program whose calls and returns do not pair up
    // (steamcmd's download threads, for one) walks it into a guard page. It is only a
    // cache: a RET that finds the wrong entry falls back to a lookup, so the pointer can
    // simply be put back in the middle and the push run again. FEX's own front end does
    // the same.
    {
        auto* base = static_cast<uint8_t*>(g_active.thread->CallRetStackBase);
        const auto* fault = static_cast<const uint8_t*>(info->si_addr);
        constexpr size_t size = FEXCore::Core::InternalThreadState::CALLRET_STACK_SIZE;
        const size_t page = static_cast<size_t>(getpagesize());
        if (base != nullptr && ((fault >= base - page && fault < base) || (fault >= base + size && fault < base + size + page))) {
            // x25 is REG_CALLRET_SP in FEXCore's ARM64 emitter; the middle is where FEX's
            // own front end puts it back.
            auto* context = static_cast<ucontext_t*>(raw_context);
            context->uc_mcontext->__ss.__x[25] = reinterpret_cast<uint64_t>(base) + size / 2;
            static std::atomic<uint64_t> resets {0};
            const auto seen = resets.fetch_add(1, std::memory_order_relaxed) + 1;
            if (seen <= 4 || (seen & 0xFF) == 0) {
                UT_INFO("pid %d tid %d: call/return stack %s, reset (%llu so far)",
                            g_current_syscalls == nullptr ? -1 : g_current_syscalls->Pid(),
                            g_current_syscalls == nullptr ? -1 : g_current_syscalls->Tid(),
                            fault < base ? "overflowed" : "underflowed", static_cast<unsigned long long>(seen));
            }
            return true;
        }
    }
    if (signal != SIGBUS) {
        return false;
    }
    if (info->si_code != BUS_ADRALN) {
        return false; // A real bad-address fault, not an alignment one.
    }
    // And the address has to be memory the guest actually has. Darwin reports some
    // bad-address faults with the alignment code, so the code alone does not distinguish
    // "the guest touched an odd address" from "the guest followed a wild pointer" -- and
    // the two must not be treated alike. FEXCore's fixup rewrites the guest's registers
    // and steps over the instruction, which for a wild pointer means carrying on with
    // nonsense: the 32-bit Steam client did exactly that, at a rate of three hundred
    // thousand faults a second, ending in "stack smashing detected".
    // Asked of a lock-free map, not of the address space's range list. This runs inside
    // a signal handler hundreds of thousands of times a second, and the range list is
    // guarded by a shared_mutex whose internals are not safe to touch from one. Asking
    // the kernel instead does not work either: the arena's whole span is reserved and
    // readable whether or not the guest has been given any of it, so every address looks
    // valid to a write() probe.
    bool wild = false;
    if (auto* space = g_current_syscalls != nullptr ? g_current_syscalls->Space() : nullptr) {
        wild = !space->MaybeCommitted(reinterpret_cast<uint64_t>(info->si_addr));
    }
    if (wild) {
        // Stepping over the instruction is not a fix -- the guest carries on with a
        // register it never loaded -- but it is what the program has been surviving on,
        // and refusing outright ends it here instead. So: allowed, counted, and named,
        // and once there have been enough of them to say this is a loop rather than a
        // stumble, allowed no longer. Without the cap this reached a hundred and
        // twenty-seven million signals in five minutes, which is most of the run.
        static std::atomic<uint64_t> wild_fixups {0};
        const auto seen = wild_fixups.fetch_add(1, std::memory_order_relaxed) + 1;
        if (seen <= 8 || (seen & 0x3FF) == 0) {
            UT_WARN("pid %d tid %d read %p, which is not mapped, from an instruction "
                        "this can step over (%llu so far, guest rip %#llx)",
                        g_current_syscalls == nullptr ? -1 : g_current_syscalls->Pid(),
                        g_current_syscalls == nullptr ? -1 : g_current_syscalls->Tid(),
                        info->si_addr, static_cast<unsigned long long>(seen),
                        static_cast<unsigned long long>(g_active.thread->CurrentFrame->State.rip));
        }
        // Generous, because this is noise rather than a symptom on its own: the arena's
        // span is reserved and readable whether or not the guest was given it, so a read
        // just past what the guest asked for finds zeroes rather than a fault on Linux
        // too. A million of them, though, is a thread that has lost its way and is
        // running in a loop, and that is worth ending.
        if (seen > (1u << 20)) {
            return false;
        }
    }

    auto* context = static_cast<ucontext_t*>(raw_context);
    auto& state = context->uc_mcontext->__ss;
    const auto pc = static_cast<uintptr_t>(arm_thread_state64_get_pc(state));

    // Only faults inside FEXCore's own generated code are ours to fix. Anything else is
    // a genuine bug in Fathom and must stay fatal.
    if (!g_active.context->IsAddressInCodeBuffer(g_active.thread, pc)) {
        return false;
    }

    // x0-x28 are plain integers in Darwin's thread state; fp and lr are separate
    // pointer-authentication-opaque fields. FEXCore wants all 31 flat, indexed by the
    // instruction's register field.
    std::array<uint64_t, 31> registers;
    std::memcpy(registers.data(), state.__x, sizeof(state.__x));
    registers[29] = static_cast<uint64_t>(arm_thread_state64_get_fp(state));
    registers[30] = static_cast<uint64_t>(arm_thread_state64_get_lr(state));

    // The fixup rewrites the faulting instruction in place. On iOS the executing address
    // is the execute-only half of a dual mapping and can never be written through, so the
    // writable alias has to be found first -- writing through `pc` would fault inside the
    // handler that exists to prevent exactly this, with the original signal still masked.
    FEXCore::Allocator::ScopedJITWriteProtect write_guard;
    const auto writable_pc =
        reinterpret_cast<uintptr_t>(FEXCore::Allocator::GetWritableAddress(reinterpret_cast<void*>(pc)));

    // Half-barrier first, and non-atomic for a site that will not stop faulting.
    //
    // A half-barrier patch is still an atomic instruction, so where the address really
    // is unaligned the patched instruction faults again -- every execution, for the life
    // of the process. Steam reached a hundred and twenty-seven million signals in five
    // minutes that way, and a signal costs about ninety seconds on a device with a
    // debugger attached, which JIT requires. Patching to non-atomic instead cannot fault
    // again, at the cost of atomicity through that site, but FEXCore declines some
    // instructions outright in that mode and a declined fixup kills the guest process.
    // So: the ordinary patch for everything, and the lossy one only for the handful of
    // sites that have proved they need it.
    auto type = FEXCore::ArchHelpers::Arm64::UnalignedHandlerType::HalfBarrier;
    if (RepeatedFaultSite(pc) > 64) {
        type = FEXCore::ArchHelpers::Arm64::UnalignedHandlerType::NonAtomic;
    }
    auto adjustment = FEXCore::ArchHelpers::Arm64::HandleUnalignedAccess(
        g_active.thread, type, writable_pc, registers.data());
    if (!adjustment.has_value() &&
        type == FEXCore::ArchHelpers::Arm64::UnalignedHandlerType::NonAtomic) {
        adjustment = FEXCore::ArchHelpers::Arm64::HandleUnalignedAccess(
            g_active.thread, FEXCore::ArchHelpers::Arm64::UnalignedHandlerType::HalfBarrier,
            writable_pc, registers.data());
    }
    if (!adjustment.has_value()) {
        return false;
    }

    // The patch went in through the writable alias; execution resumes through the
    // executable one. The instruction cache is tagged by virtual address, so invalidating
    // one alias does not invalidate the other -- without this the CPU can re-fetch the
    // pre-patch instruction and run off into whatever happens to be cached.
    __builtin___clear_cache(reinterpret_cast<char*>(pc - 4), reinterpret_cast<char*>(pc + 8));

    std::memcpy(state.__x, registers.data(), sizeof(state.__x));
    arm_thread_state64_set_fp(state, registers[29]);
    arm_thread_state64_set_lr_fptr(state, reinterpret_cast<void*>(registers[30]));
    arm_thread_state64_set_pc_fptr(state, reinterpret_cast<void*>(pc + *adjustment));
    // Counted, and reported every so often. Each faulting site is meant to be patched
    // once and never fault again; a count that keeps climbing says the patches are not
    // sticking, and that is the difference between memory-ordering emulation costing
    // nothing and costing most of the run.
    const auto fixups = g_alignment_fixups.fetch_add(1, std::memory_order_relaxed) + 1;
    if ((fixups & 0xFFFFF) == 0) {
        UT_INFO("%llu unaligned accesses emulated so far; the last was guest rip %#llx "
                    "at address %p, from generated code at %#llx (%u faults there, %s)",
                    static_cast<unsigned long long>(fixups),
                    static_cast<unsigned long long>(g_active.thread->CurrentFrame->State.rip),
                    info->si_addr, static_cast<unsigned long long>(pc),
                    RepeatedFaultSite(pc),
                    type == FEXCore::ArchHelpers::Arm64::UnalignedHandlerType::NonAtomic
                        ? "non-atomic"
                        : "half-barrier");
    }
    return true;
#else
    (void)signal;
    (void)info;
    (void)raw_context;
    return false;
#endif
}

/// The guest thread this host thread is running, for the code that has only a signal
/// context to go on.
thread_local GuestThread* g_current_guest_thread = nullptr;


std::atomic<uint64_t> g_arena_end {0};

/// Ends the guest process whose code just faulted, instead of the whole session.
///
/// A fault in guest code is the guest's own bug, and on Linux it kills that process and
/// nothing else: the shell prints "Segmentation fault" and carries on, and Steam notices
/// a helper died and starts another. Here every guest process is a thread of one host
/// process, so the default action takes the emulator down with it -- one crashing helper
/// and the whole session is gone.
///
struct LiveThread {
    FEXCore::Context::Context* context {};
    FEXCore::Core::InternalThreadState* thread {};
    uint64_t guest_base {};
    /// How far the guest's space reaches above guest_base: 4GB for a 32-bit guest, the arena for a relocated 64-bit one.
    uint64_t guest_span {};
    /// The host thread running it, recorded once that thread is actually running. Needed
    /// because throwing compiled code away has to stop everything that might be executing
    /// it: FEXCore takes its invalidation lock to keep anything from *compiling*, but a
    /// thread running an already-compiled block never takes that lock at all, and a block
    /// unlinked out from under such a thread is a wild pointer in its link list.
    uint32_t port {};
};

std::mutex g_live_threads_mutex;
std::vector<LiveThread> g_live_threads;
std::atomic<FEXCore::SignalDelegator*> g_any_delegator {nullptr};

/// Mach port of the host thread running each live guest thread, for the watchdog.


/// The same threads again, in a form a signal handler can read.
///
/// Deciding whether a fault happened in generated code means asking each live thread, and
/// a handler must not wait for a lock -- the thread it interrupted may be holding it. A
/// fixed array of slots, published with release stores, can be read at any time; a slot
/// that is stale only costs one wrong answer about a thread that has just gone.
constexpr size_t kLiveSlots = 512;
struct LiveSlot {
    std::atomic<FEXCore::Context::Context*> context {nullptr};
    std::atomic<FEXCore::Core::InternalThreadState*> thread {nullptr};
};
LiveSlot g_live_slots[kLiveSlots];

void PublishLiveThread(FEXCore::Context::Context* context, FEXCore::Core::InternalThreadState* thread) {
    for (auto& slot : g_live_slots) {
        FEXCore::Core::InternalThreadState* empty = nullptr;
        if (slot.thread.compare_exchange_strong(empty, thread, std::memory_order_acq_rel)) {
            slot.context.store(context, std::memory_order_release);
            return;
        }
    }
}

void RetireLiveThread(FEXCore::Core::InternalThreadState* thread) {
    for (auto& slot : g_live_slots) {
        if (slot.thread.load(std::memory_order_acquire) == thread) {
            slot.context.store(nullptr, std::memory_order_release);
            slot.thread.store(nullptr, std::memory_order_release);
            return;
        }
    }
}
/// How many sweeps are using a copy of that list. A thread may not be destroyed while any
/// of them is, because the sweep reaches into it.
///
/// On a lock of its own, and deliberately not on the one guarding the list. A sweep can
/// take a long time, and a thread waiting for one to finish must not be holding the lock
/// that every *starting* thread needs -- that is a guest process that forks while
/// another is exiting and does not execute a single instruction until the sweep ends.
std::mutex g_invalidators_mutex;
size_t g_invalidators {0};
std::condition_variable g_invalidations_done;

/// Unwinding out of a signal handler is only safe because of the check below: the fault
/// must have happened inside FEXCore's generated code, where the guest holds no lock of
/// ours and owns nothing that has to be put back. A fault anywhere else is a bug in
/// Fathom itself and stays fatal, because there the process really is in no state to
/// continue.
struct FaultRegs {
    bool valid {false};
    uint64_t rip {};
    uint64_t gregs[16] {};
};
thread_local FaultRegs g_fault_regs;
std::function<void(uint64_t, const uint64_t*)> g_fault_reporter;

bool EndFaultedGuestThread(int signal, siginfo_t* info, void* raw_context) {
#if defined(__aarch64__) && defined(__APPLE__)
    if (raw_context == nullptr || g_current_guest_thread == nullptr) {
        return false;
    }
    auto* context = static_cast<ucontext_t*>(raw_context);
    const auto pc = static_cast<uintptr_t>(arm_thread_state64_get_pc(context->uc_mcontext->__ss));
    g_fault_regs.valid = false;
    if (g_active.context != nullptr && g_active.thread != nullptr && g_active.delegator != nullptr &&
        g_active.context->IsAddressInCodeBuffer(g_active.thread, pc)) {
        // Guest registers live in host registers inside generated code; read them from the
        // faulting context rather than from the frame, which is only current at syscalls.
        const auto& config = g_active.delegator->GetConfig();
        auto& ss = context->uc_mcontext->__ss;
        g_fault_regs.rip = g_active.context->RestoreRIPFromHostPC(g_active.thread, pc);
        for (size_t i = 0; i < config.SRAGPRCount && i < 16; ++i) g_fault_regs.gregs[i] = ss.__x[config.SRAGPRMapping[i]];
        g_fault_regs.valid = true;
        if (g_fault_reporter) g_fault_reporter(g_fault_regs.rip, g_fault_regs.gregs);
    }
    bool in_generated_code = g_active.context != nullptr && g_active.thread != nullptr &&
                             g_active.context->IsAddressInCodeBuffer(g_active.thread, pc);
    if (!in_generated_code) {
        // A block compiled into a buffer another thread owns is still generated code, and
        // still this guest's fault -- FEXCore hands a thread whatever buffer had room.
        // Read without a lock: a handler that waits for a lock the thread it interrupted
        // is holding never returns.
        // Every live thread, in any context: a fault in generated code is the guest's
        // whichever buffer it landed in, and the thread that faulted may not be the one
        // bound to this host thread's record any more.
        for (const auto& slot : g_live_slots) {
            auto* thread = slot.thread.load(std::memory_order_acquire);
            auto* owner = slot.context.load(std::memory_order_acquire);
            if (thread == nullptr || owner == nullptr) {
                continue;
            }
            if (owner->IsAddressInCodeBuffer(thread, pc)) {
                in_generated_code = true;
                break;
            }
        }
    }
    if (!in_generated_code) {
        // Not in generated code -- but a guest that jumps to an address with nothing at it
        // faults inside FEXCore's instruction decoder rather than in the code it was
        // about to run, because the decoder is what reads guest memory first. The address
        // being inside the guest's own arena is what says this is still the guest's fault
        // and not ours.
        const auto address = info == nullptr ? 0 : reinterpret_cast<uint64_t>(info->si_addr);
        // This thread's own arena. The globals name whichever arena was created last, which
        // is often a finished process's that has already been released.
        auto* own = g_current_syscalls != nullptr ? g_current_syscalls->Space() : nullptr;
        const uint64_t begin = own != nullptr ? own->Base() : 0;
        const uint64_t end = own != nullptr ? own->Base() + own->Size() : 0;
        if (begin == 0 || address < begin || address >= end) {
            // Neither generated code nor an address the guest owns -- but this host thread
            // is inside ExecuteThread, so it is running a guest program and nothing else.
            // A guest that jumps into memory holding no instruction FEXCore knows arrives
            // here rather than at either check above: the trap is emitted by the JIT but
            // reached through FEXCore's own frames, and the address it names is the
            // instruction, not the data. Ending the one guest process is right whichever
            // of the two it turns out to be, and it is much better than ending the
            // session -- an X server and a browser die with it for another program's bug.
            if (g_active.thread == nullptr) {
                return false;
            }
            UT_WARN("fault (signal %d) inside the emulator at %p while running guest "
                        "code; ending this guest process",
                        signal, info == nullptr ? nullptr : info->si_addr);
            if (std::getenv("UTSURO_HOSTBT") != nullptr) {
                // Debugging aid (not signal-safe): where in the host this happened.
                void* frames[40];
                const int n = backtrace(frames, 40);
                backtrace_symbols_fd(frames, n, 2);
            }
            // The dispatcher has spilled the guest registers to the frame by the time the
            // decoder runs, so they are current here.
            if (g_fault_reporter) {
                const auto& st = g_active.thread->CurrentFrame->State;
                g_fault_regs.rip = st.rip;
                for (size_t i = 0; i < 16; ++i) g_fault_regs.gregs[i] = st.gregs[i];
                g_fault_regs.valid = true;
                g_fault_reporter(g_fault_regs.rip, g_fault_regs.gregs);
            }
        }
    }
    // What the guest was reaching for, and what the address space thinks is there. A fault
    // on memory the arena says is mapped and writable is a different bug from a fault on
    // memory nothing is mapped at, and from outside they look identical.
    if (info != nullptr) {
        const auto address = reinterpret_cast<uint64_t>(info->si_addr);
        // The lock-free map only: this is a signal handler, and the range list's shared_mutex
        // blocks inside libc++ even when asked to try -- on a destroyed one it throws.
        auto* space = g_current_syscalls != nullptr ? g_current_syscalls->Space() : nullptr;
        UT_WARN("guest fault at %#llx: %s", static_cast<unsigned long long>(address),
                    space != nullptr && space->MaybeCommitted(address) ? "inside memory the guest was given"
                                                                       : "nothing is mapped there");
    }
    // The registers, before the thread goes. Formatting only -- no guest memory is read
    // here -- so it is safe from a handler, and it is the only record of what the thread
    // was doing: by the time anything else looks, the thread is gone.
    char registers_text[768] = {};
    if (DescribeGuestState(registers_text, sizeof(registers_text)) > 0) {
        UT_WARN("the faulting thread's registers (last syscall %llu, made from guest "
                    "rip %#llx):\n%s",
                    static_cast<unsigned long long>(LastSyscallHere()),
                    static_cast<unsigned long long>(LastGuestRipHere()), registers_text);
    }
    return g_current_guest_thread->EndOnFault(signal);
#else
    (void)signal;
    (void)info;
    (void)raw_context;
    return false;
#endif
}

/// Whether FEXCore keeps the vector registers in its converged AVX layout (32 bytes each,
/// only with 256-bit SVE) rather than sse.data plus avx_high. Read by the nudge below.
std::atomic<bool> g_avx_layout {false};
class UtsuroSignalDelegator final : public FEXCore::SignalDelegator {
public:
    uintptr_t GetThunkCallbackRET() const override {
        // Fathom never calls back into guest code from the host, so no veneer address is
        // ever needed. Anything that reached this would be a FEXCore path Fathom does
        // not use, and zero makes that fail loudly instead of jumping somewhere random.
        return 0;
    }
};

/// Whose syscalls to answer. FEXCore has one syscall handler per context, but Fathom has
/// one SyscallSink per process, so the handler has to route to whichever guest thread
/// is currently executing on this host thread.
thread_local SyscallSink* g_current_syscalls = nullptr;


/// Writes the executing guest thread's registers out for a crash record. Signal-handler
/// context: no allocation, no locks, and g_active is thread-local so it describes the
/// thread that actually faulted.
/// Where this host thread's guest address space starts, for turning a guest address into
/// something this process can read. Zero for a 64-bit guest, whose addresses are already
/// this process's.
thread_local uint64_t g_current_guest_base = 0;


/// Appends text to a fixed buffer, and says where the next append should start.
///
/// Hand-rolled rather than snprintf, because this runs in a signal handler. snprintf is
/// not async-signal-safe: formatting reaches into the C library's locale machinery,
/// which takes a lock, and a thread interrupted while holding that lock never gets it
/// back. That deadlock is worse than the crash it is trying to describe -- the faulting
/// thread stops inside FEXCore's decoder still holding the code-invalidation lock
/// shared, and every other guest thread in the session queues behind it forever.
struct Appender {
    char* buffer;
    size_t capacity;
    size_t used {};

    void Text(const char* text) {
        while (*text != '\0' && used + 1 < capacity) {
            buffer[used++] = *text++;
        }
        buffer[used] = '\0';
    }

    void Hex(uint64_t value, int digits) {
        static const char kDigits[] = "0123456789abcdef";
        char scratch[17] = {};
        for (int index = digits - 1; index >= 0; --index) {
            scratch[digits - 1 - index] = kDigits[(value >> (index * 4)) & 0xF];
        }
        Text(scratch);
    }
};

size_t DescribeGuestState(char* buffer, size_t capacity) {
    if (g_active.thread == nullptr || buffer == nullptr || capacity == 0) {
        return 0;
    }
    const auto& state = g_active.thread->CurrentFrame->State;
    // FEXCore numbers the registers the way the instruction encoding does.
    static const char* kNames[] = {"rax", "rcx", "rdx", "rbx", "rsp", "rbp", "rsi", "rdi",
                                   "r8 ", "r9 ", "r10", "r11", "r12", "r13", "r14", "r15"};
    Appender out {buffer, capacity};
    out.Text("  rip ");
    out.Hex(g_fault_regs.valid ? g_fault_regs.rip : state.rip, 16);
    out.Text(g_fault_regs.valid ? "  (from the faulting host pc)\n" : "  (as of the last syscall)\n");
    for (size_t index = 0; index < 16; ++index) {
        out.Text("  ");
        out.Text(kNames[index]);
        out.Text(" ");
        out.Hex(g_fault_regs.valid ? g_fault_regs.gregs[index] : state.gregs[index], 16);
        out.Text((index % 2 == 1) ? "\n" : "");
    }

    // Registers only, deliberately. Reading the bytes at the guest's rip and the words
    // under its stack pointer is exactly what a crash report wants, and exactly what a
    // signal handler must not do: the addresses worth printing are the ones that are not
    // there, and a load that faults here faults with the signal already blocked, leaving
    // the thread stopped inside FEXCore's decoder holding the code-invalidation lock
    // shared -- which stops every other guest thread in the session for good. Probing
    // first does not help either; the probe needs a descriptor, and a descriptor in a
    // process that hands descriptors to a guest is not a thing a handler can rely on.
    return out.used;
}

class UtsuroSyscallHandler final : public FEXCore::HLE::SyscallHandler {
public:
    UtsuroSyscallHandler(GuestAddressSpace& space, bool guest_is_32bit)
        : space_ {space} {
        // Which ABI the JIT hands syscall arguments over in, so it passes them in
        // registers rather than spilling the whole CPU state on every call. It also
        // decides whether `int 0x80` is legal: with OS_LINUX64 declared, a 32-bit guest's
        // first syscall is refused with "trying to execute 32-bit syscall from a 64-bit
        // process" and the guest goes no further.
        OSABI = guest_is_32bit ? FEXCore::HLE::SyscallOSABI::OS_LINUX32
                               : FEXCore::HLE::SyscallOSABI::OS_LINUX64;
    }

    uint64_t HandleSyscall(FEXCore::Core::CpuStateFrame* frame, FEXCore::HLE::SyscallArguments* args) override {
        // Under OS_LINUX64 the argument array is {RAX, RDI, RSI, RDX, R10, R8, R9}:
        // the syscall number first, then x86-64 Linux's six argument registers in order.
        //
        // A syscall is the one place the guest's RIP is reliably settled, so it is also
        // the only honest place to record it for the crash handler.
        if (frame != nullptr) {
            NoteGuestRip(frame->State.rip);
        }
        auto* syscalls = g_current_syscalls;
        if (syscalls == nullptr) {
            UT_ERROR("syscall on a host thread with no guest thread bound");
            return static_cast<uint64_t>(-38); // -ENOSYS
        }
        // The syscall instruction ends its block, and the syscall layer places rip: past
        // the instruction, or at a signal handler with a frame pushed on the guest stack.
        return syscalls->Handle(args->Argument[0], args->Argument[1], args->Argument[2],
                                args->Argument[3], args->Argument[4], args->Argument[5],
                                args->Argument[6]);
    }

    FEXCore::HLE::ExecutableRangeInfo QueryGuestExecutableRange(FEXCore::Core::InternalThreadState*,
                                                               uint64_t address) override {
        GuestRange range {};
        if (!space_.RangeFor(address, &range)) {
            return {address, 1, false};
        }
        // Noted, because this is the moment the address space learns that code will be
        // compiled from here -- and it decides on that basis whether reusing this memory
        // later has to throw anything away.
        space_.NoteExecutable(range.begin, range.end());
        return {range.begin, range.size, (range.protection & kGuestProtWrite) != 0};
    }

    std::optional<FEXCore::ExecutableFileSectionInfo>
    LookupExecutableFileSection(FEXCore::Core::InternalThreadState*, uint64_t) override {
        // Only used to name regions in FEX's own profiling output.
        return std::nullopt;
    }

private:
    GuestAddressSpace& space_;
};

} // namespace

bool LastFaultRegisters(uint64_t gregs[16], uint64_t* rip) {
    if (!g_fault_regs.valid) return false;
    for (int i = 0; i < 16; ++i) gregs[i] = g_fault_regs.gregs[i];
    *rip = g_fault_regs.rip;
    return true;
}

// ---------------------------------------------------------------------------
// The context
// ---------------------------------------------------------------------------

class FexEngine::Impl {
public:
    explicit Impl(GuestAddressSpace& space)
        : space {space} {}

    uint64_t guest_base {};
    uint64_t guest_span {};

    ~Impl() {
        context.reset();
        if (config_held) {
            ConfigLease::Release();
        }
    }

    GuestAddressSpace& space;

    FEXCore::HostFeatures host_features {};
    fextl::unique_ptr<FEXCore::Context::Context> context;
    bool guest_is_32bit {};
    std::unique_ptr<UtsuroSignalDelegator> signals;
    std::unique_ptr<UtsuroSyscallHandler> handler;
    bool config_held {};
};

// ---------------------------------------------------------------------------
// Compiled code that is no longer the code it was compiled from
// ---------------------------------------------------------------------------

/// Every live guest thread, with the context it runs in and where that context's guest
/// address space starts.
///
/// FEXCore caches compiled blocks by guest address. Guest addresses are handed out again
/// -- an exec releases a program's image and the next one is loaded over it, a library is
/// unmapped and another is mapped where it was -- and nothing in FEXCore notices, because
/// nothing tells it. The new program then runs the old program's compiled code: the same
/// addresses, the same block entries, the wrong instructions. It does not look like a
/// stale cache from the outside. It looks like a program jumping to a nonsense address
/// with a nonsense stack pointer, deterministically, in a place that makes no sense.
void RegisterLiveThread(FEXCore::Context::Context* context, FEXCore::Core::InternalThreadState* thread,
                        uint64_t guest_base, uint64_t guest_span) {
    if (context == nullptr || thread == nullptr) {
        return;
    }
    PublishLiveThread(context, thread);
    std::scoped_lock lock {g_live_threads_mutex};
    g_live_threads.push_back(LiveThread {context, thread, guest_base, guest_span});
}

void ForgetLiveThread(FEXCore::Core::InternalThreadState* thread) {
    RetireLiveThread(thread);
    std::scoped_lock lock {g_live_threads_mutex};
    std::erase_if(g_live_threads, [thread](const LiveThread& live) { return live.thread == thread; });
}

/// Drops every compiled block covering [host_begin, host_end) in every live thread.
///
/// The range is a host one, because that is what the address space deals in; FEXCore
/// keeps its cache in the guest's numbering, so each context's own base comes off first.
std::atomic<uint64_t> g_invalidations {0};

/// How many guest processes have threads suspended, and what could not be thrown away
/// while they were.
std::atomic<int> g_freezes {0};
std::mutex g_deferred_mutex;
std::vector<std::pair<uint64_t, uint64_t>> g_deferred;

void InvalidateCompiledCode(uint64_t host_begin, uint64_t host_end) {
    if (host_end <= host_begin) {
        return;
    }
    // A way to ask what throwing compiled code away is costing. Everything that waits on
    // FEXCore's invalidation lock waits here, and that lock gives writers priority, so a
    // single one of these stops every thread that wants to compile. With this set nothing
    // is thrown away and stale code runs -- which is wrong, and tells us whether the
    // waiting is what matters.
    static const bool disabled = getenv("FATHOM_NO_INVALIDATE") != nullptr;
    if (disabled) {
        return;
    }
    if (g_freezes.load(std::memory_order_acquire) > 0) {
        std::scoped_lock lock {g_deferred_mutex};
        if (g_freezes.load(std::memory_order_acquire) > 0) {
            g_deferred.emplace_back(host_begin, host_end);
            return;
        }
    }
    // Counted because throwing compiled code away is not free and it is easy to do far
    // more often than intended: every block dropped is recompiled from scratch, and every
    // unaligned access inside it faults again, because the patch that stopped it faulting
    // was part of the code that was just discarded.
    const auto count = g_invalidations.fetch_add(1, std::memory_order_relaxed) + 1;
    if ((count & 0x3FF) == 0) {
        UT_INFO("%llu code invalidations so far",
                    static_cast<unsigned long long>(count));
    }
    // The list is copied, and a thread is kept from ending while the copy is in use. A
    // thread that ends mid-sweep takes its lookup cache with it, and the invalidation
    // walks into freed memory -- a null dereference inside FEXCore, reached from an
    // ordinary guest mmap. Holding the lock for the whole sweep would do it too, but the
    // sweep waits on FEXCore's own lock and that wait can be long, so a counter is used
    // instead and thread destruction waits on that.
    std::vector<LiveThread> live;
    {
        std::scoped_lock lock {g_live_threads_mutex, g_invalidators_mutex};
        live = g_live_threads;
        ++g_invalidators;
    }
    struct Finished {
        ~Finished() {
            {
                std::scoped_lock lock {g_invalidators_mutex};
                --g_invalidators;
            }
            g_invalidations_done.notify_all();
        }
    } finished;
    // Contexts first: a code buffer's lookup table is shared by every thread using it.
    const auto self = pthread_mach_thread_np(pthread_self());
    std::set<FEXCore::Context::Context*> contexts;
    for (const auto& entry : live) {
        // A 32-bit context sees its own 4GB space and nothing else; a change outside it is not its business, and taken
        // as one of its own addresses it would throw away code that is still good.
        if (entry.guest_base != 0 &&
            (host_end <= entry.guest_base || host_begin >= entry.guest_base + entry.guest_span)) {
            continue;
        }
        if (!contexts.insert(entry.context).second) {
            continue;
        }
        const uint64_t begin = host_begin - entry.guest_base;
        // Timed, because this wait is the one that can stop a guest mmap for minutes and
        // there is no other way to see it happening: the thread is inside FEXCore, its
        // guest rip has not moved, and from the outside it looks like a hung syscall.
        const auto wanted = std::chrono::steady_clock::now();
        std::unique_lock guard {entry.context->GetCodeInvalidationMutex()};
        const auto waited = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - wanted)
                                .count();
        if (waited > 1000) {
            UT_WARN("waited %lld ms for the code invalidation lock to throw away "
                        "%#llx..%#llx",
                        static_cast<long long>(waited),
                        static_cast<unsigned long long>(host_begin),
                        static_cast<unsigned long long>(host_end));
        }

        // Deliberately not stopping the threads that might be *running* a block being
        // thrown away, tempting as it is: a thread looking a block up holds its own lookup
        // cache's read lock while it does, and the invalidation below takes that same
        // cache's write lock -- so a thread stopped at the wrong instant is a lock nothing
        // can take, and every mmap in the process waits on it forever. Unlinking a block
        // under a thread standing in it is a much rarer accident than that.
        entry.context->InvalidateCodeBuffersCodeRange(begin, host_end - host_begin);
        for (const auto& thread : live) {
            if (thread.context == entry.context) {
                entry.context->InvalidateThreadCachedCodeRange(thread.thread, begin,
                                                               host_end - host_begin);
            }
        }
        const auto swept = std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::steady_clock::now() - wanted)
                               .count();
        if (swept > 1000) {
            UT_WARN("throwing away %#llx..%#llx across %zu threads took %lld ms",
                        static_cast<unsigned long long>(host_begin),
                        static_cast<unsigned long long>(host_end), live.size(),
                        static_cast<long long>(swept));
        }
    }
}

namespace {

/// The runtime's own invalidator. Everything queued here is thrown away on this thread
/// and not on the guest's, because the callers that queue are holding something a guest
/// thread needs -- the process table, most of all -- and waiting for FEXCore's lock with
/// it held stops the session.
std::mutex g_queue_mutex;
std::condition_variable g_queue_signal;
std::vector<std::pair<uint64_t, uint64_t>> g_queue;
std::once_flag g_queue_started;

void RunInvalidator() {
    for (;;) {
        std::vector<std::pair<uint64_t, uint64_t>> batch;
        {
            std::unique_lock lock {g_queue_mutex};
            g_queue_signal.wait(lock, [] { return !g_queue.empty(); });
            batch.swap(g_queue);
        }
        for (const auto& [begin, end] : batch) {
            InvalidateCompiledCode(begin, end);
        }
    }
}

void Queue(std::vector<std::pair<uint64_t, uint64_t>> ranges) {
    if (ranges.empty()) {
        return;
    }
    std::call_once(g_queue_started, [] { std::thread {RunInvalidator}.detach(); });
    {
        std::scoped_lock lock {g_queue_mutex};
        g_queue.insert(g_queue.end(), ranges.begin(), ranges.end());
    }
    g_queue_signal.notify_one();
}

} // namespace

void InvalidateCompiledCodeLater(uint64_t host_begin, uint64_t host_end) {
    Queue({{host_begin, host_end}});
}

void DescribeCodeLocks(char* buffer, size_t capacity) {
    if (buffer == nullptr || capacity == 0) {
        return;
    }
    buffer[0] = '\0';
    std::unique_lock lock {g_live_threads_mutex, std::try_to_lock};
    if (!lock.owns_lock()) {
        std::snprintf(buffer, capacity, "busy");
        return;
    }
    std::set<FEXCore::Context::Context*> seen;
    size_t written = 0;
    for (const auto& entry : g_live_threads) {
        if (entry.context == nullptr || !seen.insert(entry.context).second) {
            continue;
        }
        const uint32_t state = entry.context->GetCodeInvalidationMutex().State();
        const int added = std::snprintf(buffer + written, capacity - written,
                                        "%s%u readers, %u writers waiting%s",
                                        written == 0 ? "" : "; ", state & 0xFFFFu,
                                        (state >> 16) & 0x7FFFu,
                                        (state & 0x80000000u) != 0 ? ", write-owned" : "");
        if (added <= 0 || written + static_cast<size_t>(added) >= capacity) {
            return;
        }
        written += static_cast<size_t>(added);
    }
}

void HoldInvalidations() {
    g_freezes.fetch_add(1, std::memory_order_acq_rel);
}

void ReleaseInvalidations() {
    if (g_freezes.fetch_sub(1, std::memory_order_acq_rel) != 1) {
        return;
    }
    std::vector<std::pair<uint64_t, uint64_t>> pending;
    {
        std::scoped_lock lock {g_deferred_mutex};
        pending.swap(g_deferred);
    }
    // Not here: a thaw happens with the process table held, and this is exactly the wait
    // that must not happen with it held.
    Queue(std::move(pending));
}

// ---------------------------------------------------------------------------
// A thread
// ---------------------------------------------------------------------------

class GuestThread::Impl {
public:
    Impl(FEXCore::Context::Context* context, FEXCore::SignalDelegator* engine_signals, SyscallSink& syscalls,
         bool guest_is_32bit)
        : context {context}
        , engine_signals {engine_signals}
        , syscalls {syscalls}
        , guest_is_32bit {guest_is_32bit} {}

    ~Impl() {
        if (context != nullptr && thread != nullptr) {
            ForgetLiveThread(thread);
            {
                // Nothing may be halfway through a sweep that names this thread.
                std::unique_lock wait {g_invalidators_mutex};
                g_invalidations_done.wait(wait, [] { return g_invalidators == 0; });
            }
            std::scoped_lock guard {g_thread_lifecycle};
            context->DestroyThread(thread);
            thread = nullptr;
        }
    }

    FEXCore::Context::Context* context {};
    FEXCore::SignalDelegator* engine_signals {};
    SyscallSink& syscalls;
    bool guest_is_32bit {};
    uint64_t guest_base {};
    uint64_t guest_span {};

    // Per thread, all of it. The call/return stack and the segment table are pointed at
    // from the register file, so a forked child needs its own rather than the copies it
    // would otherwise inherit from its parent.
    std::unique_ptr<CallRetStack> callret;
    GuestSegments segments;
    FEXCore::Core::InternalThreadState* thread {};

    FEXCore::UncheckedLongJump::JumpBuf exit_jump {};
    /// The signal that killed this guest thread, if one did.
    int fatal_signal {};
    bool exit_jump_armed {};
    int exit_status {};
    bool exec_requested {};
};

GuestThread::GuestThread(std::unique_ptr<Impl> impl)
    : impl_ {std::move(impl)} {}

GuestThread::~GuestThread() = default;

RunResult GuestThread::Run() {
    RunResult result;
    if (impl_->thread == nullptr) {
        result.message = "no guest thread";
        return result;
    }

    // Bound for the duration of the run: the syscall handler is shared by every thread in
    // the context and finds this thread's syscall state through it.
    g_current_syscalls = &impl_->syscalls;
    g_current_guest_thread = this;
    g_current_guest_base = impl_->guest_base;
    impl_->syscalls.SetHostThread(pthread_self());
    // Timed, because a guest thread that takes seconds to reach its first instruction is
    // invisible from outside -- the process exists, its rip never moves -- and a forked
    // child that starts late is a child still standing in its parent's memory when the
    // fork gives up waiting and lets the parent run there again.
    const auto entered = std::chrono::steady_clock::now();
    const auto elapsed = [&] {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now() - entered)
            .count();
    };
    {
        // This host thread is the one that will be running compiled code for this guest
        // thread, and only it can say which it is.
        std::scoped_lock lock {g_live_threads_mutex};
        for (auto& live : g_live_threads) {
            if (live.thread == impl_->thread) {
                live.port = pthread_mach_thread_np(pthread_self());
                break;
            }
        }
    }

    // The guest leaves the JIT one of two ways. A clean HLT returns from ExecuteThread
    // normally; exit_group happens deep inside a syscall with JIT frames still on the
    // host stack, and the only way out of there is to unwind past them. FEX's own thread
    // exit does exactly this, which is why FEXCore ships the jump buffer used here.
    if (elapsed() > 200) {
        UT_WARN("guest thread waited %lld ms to be listed before it could start",
                    static_cast<long long>(elapsed()));
    }
    if (FEXCore::UncheckedLongJump::SetJump(impl_->exit_jump) == 0) {
        impl_->exit_jump_armed = true;
        g_active = ActiveExecution {impl_->context, impl_->thread, impl_->engine_signals};
        impl_->context->ExecuteThread(impl_->thread);
        g_active = ActiveExecution {};
        impl_->exit_jump_armed = false;

        result.outcome = RunOutcome::Halted;
        result.status = 0;
        result.message = "guest halted";
    } else {
        // Reached by the long jump out of exit_group, which skips the clear above.
        g_active = ActiveExecution {};
        impl_->exit_jump_armed = false;
        if (impl_->exec_requested) {
            impl_->exec_requested = false;
            result.outcome = RunOutcome::Execed;
            result.status = 0;
            result.message = "execve";
        } else if (impl_->fatal_signal != 0) {
            result.outcome = RunOutcome::Faulted;
            result.status = 128 + impl_->fatal_signal;
            result.message = "killed by a fault in guest code";
            impl_->fatal_signal = 0;
        } else if (impl_->syscalls.StopRequested()) {
            result.outcome = RunOutcome::Stopped;
            result.status = -1;
            result.message = "stopped";
        } else {
            result.outcome = RunOutcome::Exited;
            result.status = impl_->exit_status;
            result.message = "guest exited";
        }
    }

    g_current_syscalls = nullptr;
    g_current_guest_thread = nullptr;

    const auto& state = impl_->thread->CurrentFrame->State;
    result.rip = state.rip;
    result.rsp = state.gregs[FEXCore::X86State::REG_RSP];
    return result;
}

bool GuestThread::EndOnFault(int signal) {
    if (!impl_->exit_jump_armed) {
        return false;
    }
    impl_->fatal_signal = signal;
    impl_->exit_jump_armed = false;
    // Never returns. The frames between here and Run() are FEXCore's generated code and
    // its dispatcher, which is exactly what exit_group already unwinds past.
    FEXCore::UncheckedLongJump::LongJump(impl_->exit_jump, 1);
    return true;
}

void GuestThread::ResetTo(uint64_t rip, uint64_t rsp) {
    // A register file, cleared wholesale: CPUState has no copy assignment, and byte-wise
    // is what "a fresh set of registers" actually means here.
    auto& state = impl_->thread->CurrentFrame->State;
    std::memset(&state, 0, sizeof(state));
    impl_->segments.Initialise(state, impl_->guest_is_32bit);
    impl_->callret->Attach(impl_->thread);
    state.rip = rip;
    state.gregs[FEXCore::X86State::REG_RSP] = rsp;
    impl_->context->SetFlagsFromCompactedEFLAGS(impl_->thread, 1U << 1);
}

uint64_t GuestThread::Rip() const {
    return impl_->thread == nullptr ? 0 : impl_->thread->CurrentFrame->State.rip;
}

uint64_t GuestThread::Rsp() const {
    return impl_->thread == nullptr ? 0
                                    : impl_->thread->CurrentFrame->State.gregs[FEXCore::X86State::REG_RSP];
}

// Only meaningful from inside a syscall on this thread: the JIT spills every register
// into the frame before calling the syscall handler and reloads them after, and the
// syscall instruction ends its block, so a changed rip is where the thread goes next.
bool GuestThread::CaptureCpuState(GuestCpuState* out) const {
    if (impl_->thread == nullptr || impl_->context == nullptr || out == nullptr) {
        return false;
    }
    auto& state = impl_->thread->CurrentFrame->State;
    std::memcpy(out->gregs, state.gregs, sizeof(out->gregs));
    out->rip = state.rip;
    out->eflags = impl_->context->ReconstructCompactedEFLAGS(impl_->thread, false, nullptr, 0);
    out->mxcsr = state.mxcsr;
    out->fs_base = state.fs_cached;
    out->gs_base = state.gs_cached;
    // Through FEXCore, which knows its own layout: avx.data only when the host has 256-bit
    // SVE, otherwise sse.data with the upper halves in avx_high. Guessing from SupportsAVX
    // alone picked the wrong one on every Apple chip and scrambled half the registers.
    __uint128_t low[16] {};
    __uint128_t high[16] {};
    impl_->context->ReconstructXMMRegisters(impl_->thread, low, high);
    std::memcpy(out->xmm, low, sizeof(out->xmm));
    std::memcpy(out->ymm_high, high, sizeof(out->ymm_high));
    std::memcpy(out->mm, state.mm, sizeof(out->mm));
    out->fcw = state.FCW;
    out->ftw = state.AbridgedFTW;
    return true;
}

bool GuestThread::RestoreCpuState(const GuestCpuState& in) {
    if (impl_->thread == nullptr || impl_->context == nullptr) {
        return false;
    }
    auto& state = impl_->thread->CurrentFrame->State;
    std::memcpy(state.gregs, in.gregs, sizeof(state.gregs));
    state.rip = in.rip;
    impl_->context->SetFlagsFromCompactedEFLAGS(impl_->thread, in.eflags);
    state.mxcsr = in.mxcsr;
    __uint128_t low[16] {};
    __uint128_t high[16] {};
    std::memcpy(low, in.xmm, sizeof(low));
    std::memcpy(high, in.ymm_high, sizeof(high));
    impl_->context->SetXMMRegistersFromState(impl_->thread, low, high);
    std::memcpy(state.mm, in.mm, sizeof(state.mm));
    state.FCW = in.fcw;
    state.AbridgedFTW = in.ftw;
    return true;
}

void GuestThread::AdvanceGuestRip(int64_t bytes) {
    if (impl_->thread != nullptr) {
        impl_->thread->CurrentFrame->State.rip += static_cast<uint64_t>(bytes);
    }
}

uint64_t GuestThread::Rax() const {
    return impl_->thread == nullptr ? 0 : impl_->thread->CurrentFrame->State.gregs[FEXCore::X86State::REG_RAX];
}

void GuestThread::SetFsBase(uint64_t base) {
    if (impl_->thread != nullptr) {
        impl_->thread->CurrentFrame->State.fs_cached = base;
    }
}

void GuestThread::SetGsBase(uint64_t base) {
    if (impl_->thread != nullptr) {
        impl_->thread->CurrentFrame->State.gs_cached = base;
    }
}

uint64_t GuestThread::GetGsBase() const {
    return impl_->thread == nullptr ? 0 : impl_->thread->CurrentFrame->State.gs_cached;
}

uint64_t GuestThread::GetFsBase() const {
    return impl_->thread == nullptr ? 0 : impl_->thread->CurrentFrame->State.fs_cached;
}

void GuestThread::SetTlsDescriptor(int entry, uint32_t base, uint32_t limit,
                                   bool point_gs_at_it) {
    if (impl_->thread == nullptr || entry < 0 || entry >= kGdtEntries) {
        return;
    }
    auto& state = impl_->thread->CurrentFrame->State;
    auto* descriptor = &state.segment_arrays[FEXCore::Core::CPUState::SEGMENT_ARRAY_INDEX_GDT][entry];
    FEXCore::Core::CPUState::SetGDTBase(descriptor, base);
    FEXCore::Core::CPUState::SetGDTLimit(descriptor, limit);
    descriptor->S = 1;      // A code/data descriptor rather than a system one.
    descriptor->Type = 3;   // Data, read/write, accessed.
    descriptor->DPL = 3;    // Userspace.
    descriptor->P = 1;      // Present.
    descriptor->D = 1;      // 32-bit.
    descriptor->G = 1;      // Limit counted in pages.
    // The guest loads %gs from this entry immediately afterwards, and that load recomputes
    // the cached base itself. Refreshing it here matters only when the entry it is
    // rewriting is the one %gs already points at, which is what a second thread does.
    //
    // A thread created by clone never does that load at all. Linux hands it a register
    // file copied from its parent, with %gs already holding the TLS selector, and sets
    // the base behind that selector to the one the clone asked for; the thread resumes
    // at the instruction after the clone and simply uses it. Here the register file is
    // new and zeroed, so %gs points at entry zero with a base of zero -- and every
    // thread-local read in that thread lands a hundred or so bytes below address zero,
    // which is the top of the address space. glibc's stack canary lives in thread-local
    // storage, so this reads as "*** stack smashing detected ***" a long way from the
    // thread that has the problem.
    if (point_gs_at_it) {
        state.gs_idx = static_cast<uint16_t>((entry << 3) | 3);
    }
    if ((state.gs_idx >> 3) == entry) {
        state.gs_cached = FEXCore::Core::CPUState::CalculateGDTBase(*descriptor);
    }
}

void GuestThread::ExecGuest() {
    impl_->exec_requested = true;
    if (impl_->exit_jump_armed) {
        FEXCore::UncheckedLongJump::LongJump(impl_->exit_jump, 1);
    }
    UT_ERROR("execve requested with no unwind point; aborting");
    std::abort();
}

void GuestThread::ExitGuest(int status) {
    impl_->exit_status = status;
    if (impl_->exit_jump_armed) {
        FEXCore::UncheckedLongJump::LongJump(impl_->exit_jump, 1);
    }
    // Unreachable in practice: Run() arms the jump before the guest can execute a single
    // instruction, so there is no window where a guest syscall arrives without it.
    UT_ERROR("guest exit requested with no unwind point; aborting");
    std::abort();
}

// ---------------------------------------------------------------------------
// Creating them
// ---------------------------------------------------------------------------

FexEngine::FexEngine(std::unique_ptr<Impl> impl)
    : impl_ {std::move(impl)} {}

FexEngine::~FexEngine() = default;

GuestThread* FexEngine::Current() {
    return g_current_guest_thread;
}

void SetFaultReporter(std::function<void(uint64_t, const uint64_t*)> reporter) { g_fault_reporter = std::move(reporter); }

void SampleGuestRips(std::vector<uint64_t>& out, std::vector<std::array<uint64_t, 16>>* regs_out) {
#if defined(__aarch64__) && defined(__APPLE__)
    std::vector<LiveThread> live;
    {
        std::scoped_lock lock {g_live_threads_mutex};
        live = g_live_threads;
    }
    for (const auto& entry : live) {
        if (entry.port == 0) continue;
        thread_act_t port = entry.port;
        if (thread_suspend(port) != KERN_SUCCESS) continue;
        arm_thread_state64_t ss;
        mach_msg_type_number_t count = ARM_THREAD_STATE64_COUNT;
        if (thread_get_state(port, ARM_THREAD_STATE64, reinterpret_cast<thread_state_t>(&ss), &count) == KERN_SUCCESS) {
            const uintptr_t pc = static_cast<uintptr_t>(arm_thread_state64_get_pc(ss));
            if (entry.context->IsAddressInCodeBuffer(entry.thread, pc)) {
                out.push_back(entry.context->RestoreRIPFromHostPC(entry.thread, pc));
                if (regs_out != nullptr) {
                    std::array<uint64_t, 16> r {};
                    if (auto* d = g_any_delegator.load()) {
                        const auto& config = d->GetConfig();
                        for (size_t i = 0; i < config.SRAGPRCount && i < 16; ++i) r[i] = ss.__x[config.SRAGPRMapping[i]];
                    }
                    regs_out->push_back(r);
                }
            }
        }
        thread_resume(port);
    }
#endif
}

void DumpGuestThreads(const std::function<std::string(uint64_t)>& describe,
                      const std::function<void(uint64_t, const uint64_t*)>& with_state) {
#if defined(__aarch64__) && defined(__APPLE__)
    std::vector<LiveThread> live;
    {
        std::scoped_lock lock {g_live_threads_mutex};
        live = g_live_threads;
    }
    for (const auto& entry : live) {
        if (entry.port == 0) continue;
        thread_act_t port = entry.port;
        if (thread_suspend(port) != KERN_SUCCESS) continue;
        arm_thread_state64_t ss;
        mach_msg_type_number_t count = ARM_THREAD_STATE64_COUNT;
        if (thread_get_state(port, ARM_THREAD_STATE64, reinterpret_cast<thread_state_t>(&ss), &count) == KERN_SUCCESS) {
            const uintptr_t pc = static_cast<uintptr_t>(arm_thread_state64_get_pc(ss));
            if (entry.context->IsAddressInCodeBuffer(entry.thread, pc)) {
                const uint64_t rip = entry.context->RestoreRIPFromHostPC(entry.thread, pc);
                UT_WARN("watchdog: guest thread %u in JIT code, guest rip %#llx %s (host pc %#llx)", port,
                        static_cast<unsigned long long>(rip), describe ? describe(rip).c_str() : "", static_cast<unsigned long long>(pc));
                if (auto* d = g_any_delegator.load()) {
                    const auto& config = d->GetConfig();
                    static const char* kNames[] = {"rax", "rcx", "rdx", "rbx", "rsp", "rbp", "rsi", "rdi",
                                                   "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15"};
                    uint64_t regs[16] = {};
                    for (size_t i = 0; i < config.SRAGPRCount && i < 16; ++i) regs[i] = ss.__x[config.SRAGPRMapping[i]];
                    if (with_state) with_state(rip, regs);
                    for (size_t i = 0; i < config.SRAGPRCount && i < 16; i += 4) {
                        UT_WARN("  %s=%#llx %s=%#llx %s=%#llx %s=%#llx", kNames[i], (unsigned long long)ss.__x[config.SRAGPRMapping[i]],
                                kNames[i + 1], (unsigned long long)ss.__x[config.SRAGPRMapping[i + 1]], kNames[i + 2],
                                (unsigned long long)ss.__x[config.SRAGPRMapping[i + 2]], kNames[i + 3],
                                (unsigned long long)ss.__x[config.SRAGPRMapping[i + 3]]);
                    }
                }
            } else {
                UT_WARN("watchdog: guest thread %u is in host code, pc %#llx lr %#llx", port, static_cast<unsigned long long>(pc),
                        static_cast<unsigned long long>(arm_thread_state64_get_lr(ss)));
            }
        }
        thread_resume(port);
    }
#endif
}

void FexEngine::PauseCompilation() {
    impl_->context->GetCodeInvalidationMutex().lock();
}

void FexEngine::ResumeCompilation() {
    impl_->context->GetCodeInvalidationMutex().unlock();
}

const char* FexEngine::FexRevision() {
#ifdef GIT_DESCRIBE_STRING
    return GIT_DESCRIBE_STRING;
#else
    return "unknown";
#endif
}

std::unique_ptr<FexEngine> FexEngine::Create(GuestAddressSpace& space, const EngineOptions& options,
                                             std::string& error) {
    // One engine at a time, from setting FEXCore's global configuration until the new
    // context has read it. The 64-bit flag is process-global and every context copies it
    // when it is built: a 32-bit and a 64-bit process starting together (a 32-bit Steam
    // spawning a 64-bit helper) otherwise got a 64-bit context built in 32-bit mode, which
    // fails at the new program's first instructions.
    static std::mutex create_mutex;
    std::unique_lock create_lock {create_mutex};
    auto impl = std::make_unique<Impl>(space);

    ConfigLease::Acquire(options);
    impl->config_held = true;

    // Reads the real CPU through sysctl rather than the ID_AA64* system registers Linux
    // exposes -- those fault on Darwin, which is one of the fixes that made this FEXCore
    // tree work on Apple hardware at all.
    impl->host_features = FEX::FetchHostFeatures();
    if (options.disable_avx) {
        impl->host_features.SupportsAVX = false;
        UT_INFO("AVX disabled: the guest will be told this CPU has none");
    }

    impl->context = FEXCore::Context::Context::CreateNewContext(impl->host_features);
    if (impl->context == nullptr) {
        error = "FEXCore refused to create a context";
        return nullptr;
    }

    // Told before any code is compiled, because it changes every address the JIT emits.
    g_arena_space.store(&space, std::memory_order_release);
    g_arena_begin.store(space.Base(), std::memory_order_release);
    g_arena_end.store(space.Base() + space.Size(), std::memory_order_release);
    impl->guest_base = options.guest_memory_base;
    impl->guest_span = options.guest_is_32bit ? uint64_t {1} << 32 : space.Size();
    impl->context->SetGuestMemoryBase(options.guest_memory_base);
    if (options.guest_memory_base != 0) {
        UT_INFO("%d-bit guest: its address space is placed at %#llx in this process",
                    options.guest_is_32bit ? 32 : 64, static_cast<unsigned long long>(options.guest_memory_base));
    }

    impl->signals = std::make_unique<UtsuroSignalDelegator>();
    g_any_delegator.store(impl->signals.get());
    g_avx_layout.store(impl->host_features.SupportsAVX && impl->host_features.SupportsSVE256, std::memory_order_release);
    impl->guest_is_32bit = options.guest_is_32bit;
    impl->handler = std::make_unique<UtsuroSyscallHandler>(space, options.guest_is_32bit);
    impl->context->SetSignalDelegator(impl->signals.get());
    impl->context->SetSyscallHandler(impl->handler.get());

    // A guest that executes HLT should stop the run, not trap. Normal exits come through
    // exit_group instead, but a jump into unmapped-but-zeroed memory tends to land here.
    impl->context->EnableExitOnHLT();

    if (!impl->context->InitCore()) {
        error = "FEXCore could not initialise its JIT. On iOS this is what a missing JIT "
                "permission looks like: the code buffers cannot be made executable.";
        return nullptr;
    }
    create_lock.unlock();

    // From here on, a guest alignment fault is recoverable rather than fatal.
    SetFaultRecovery(RecoverAlignmentFault);
    SetGuestFaultEnder(EndFaultedGuestThread);
    SetGuestStateDescriber(DescribeGuestState);

    UT_INFO("FEXCore context ready (AVX=%d, SVE128=%d, cache line %u)",
                impl->host_features.SupportsAVX ? 1 : 0, impl->host_features.SupportsSVE128 ? 1 : 0,
                impl->host_features.DCacheLineSize);
    return std::unique_ptr<FexEngine> {new FexEngine {std::move(impl)}};
}

std::unique_ptr<GuestThread> FexEngine::StartThread(uint64_t rip, uint64_t rsp, SyscallSink& syscalls,
                                                    std::string& error) {
    auto impl = std::make_unique<GuestThread::Impl>(impl_->context.get(), impl_->signals.get(), syscalls, impl_->guest_is_32bit);

    impl->callret = std::make_unique<CallRetStack>();
    if (!impl->callret->valid()) {
        error = std::string {"could not reserve the call/return stack: "} +
                std::strerror(impl->callret->error());
        return nullptr;
    }

    impl->guest_base = impl_->guest_base;
    impl->guest_span = impl_->guest_span;
    {
        std::scoped_lock guard {g_thread_lifecycle};
        impl->thread = impl_->context->CreateThread(rip, rsp);
    }
    if (impl->thread == nullptr) {
        error = "FEXCore could not create the guest thread";
        return nullptr;
    }
    RegisterLiveThread(impl->context, impl->thread, impl->guest_base, impl->guest_span);

    auto& state = impl->thread->CurrentFrame->State;
    impl->segments.Initialise(state, impl->guest_is_32bit);
    impl->callret->Attach(impl->thread);
    state.rip = rip;
    state.gregs[FEXCore::X86State::REG_RSP] = rsp;

    // A freshly executed Linux program starts with a defined flag state: bit 1 is
    // reserved and always set, everything else clear.
    impl_->context->SetFlagsFromCompactedEFLAGS(impl->thread, 1U << 1);

    std::array<__uint128_t, FEXCore::Core::CPUState::NUM_XMMS> xmm {};
    std::array<__uint128_t, FEXCore::Core::CPUState::NUM_XMMS> ymm_high {};
    impl_->context->SetXMMRegistersFromState(impl->thread, xmm.data(),
                                             impl_->host_features.SupportsAVX ? ymm_high.data() : nullptr);

    UT_INFO("guest thread ready: rip=%#llx rsp=%#llx", static_cast<unsigned long long>(rip),
                static_cast<unsigned long long>(rsp));
    return std::unique_ptr<GuestThread> {new GuestThread {std::move(impl)}};
}

std::unique_ptr<GuestThread> FexEngine::ForkThread(const GuestThread& parent, SyscallSink& syscalls,
                                                   std::string& error, uint64_t new_rsp) {
    const auto& parent_state = parent.impl_->thread->CurrentFrame->State;

    auto impl = std::make_unique<GuestThread::Impl>(impl_->context.get(), impl_->signals.get(), syscalls, impl_->guest_is_32bit);
    impl->callret = std::make_unique<CallRetStack>();
    if (!impl->callret->valid()) {
        error = std::string {"could not reserve the child's call/return stack: "} +
                std::strerror(impl->callret->error());
        return nullptr;
    }

    // Where the child resumes is not the parent's rip. During a syscall FEX sets
    // State.rip to the address *of* the syscall instruction, so starting a child there
    // would execute it a second time -- with RAX cleared, that is a read(), which is
    // precisely the wrong thing and looks maddeningly like the child ignoring the fork.
    //
    // The x86-64 syscall instruction puts its own return address in RCX, and FEX honours
    // that, so RCX is the instruction after the syscall: exactly where a returning fork
    // belongs. sysret does the same thing on real hardware.
    //
    // An i386 guest has no such thing: it arrives through `int 0x80` or `sysenter`,
    // neither of which records a return address, and RCX is that ABI's *third syscall
    // argument*. Both instructions are two bytes, so the instruction after is all there
    // is to go on -- and reading RCX there would resume at whatever the caller happened
    // to pass, which for clone is the new thread's stack pointer.
    const uint64_t resume = impl_->guest_is_32bit || parent_state.gregs[FEXCore::X86State::REG_RCX] == 0
                                ? parent_state.rip + 2  // CD 80, or 0F 05
                                : parent_state.gregs[FEXCore::X86State::REG_RCX];

    // Handed to CreateThread rather than memcpy'd in afterwards. FEX copies the state
    // first and *then* does its own per-thread setup on top -- InitializeCompiler, and
    // clearing DeferredSignalRefCount. Copying over the register file after the fact
    // undoes exactly that work and leaves the child holding its parent's signal
    // bookkeeping, which is how a second running thread ends up dereferencing null.
    FEXCore::Core::CPUState child_state;
    std::memcpy(&child_state, &parent_state, sizeof(child_state));
    child_state.rip = resume;
    child_state.gregs[FEXCore::X86State::REG_RAX] = 0;
    if (new_rsp != 0) {
        child_state.gregs[FEXCore::X86State::REG_RSP] = new_rsp;
    }

    impl->guest_base = impl_->guest_base;
    impl->guest_span = impl_->guest_span;
    {
        std::scoped_lock guard {g_thread_lifecycle};
        impl->thread = impl_->context->CreateThread(resume, child_state.gregs[FEXCore::X86State::REG_RSP],
                                                    &child_state);
    }
    if (impl->thread == nullptr) {
        error = "FEXCore could not create the child guest thread";
        return nullptr;
    }
    RegisterLiveThread(impl->context, impl->thread, impl->guest_base, impl->guest_span);

    // The call/return stack and the segment table are reached through the register file,
    // and the child must not share its parent's -- but the descriptors in it carry over,
    // the way they do across a real clone or fork.
    auto& state = impl->thread->CurrentFrame->State;
    impl->segments.Initialise(state, impl->guest_is_32bit, &parent.impl_->segments);
    impl->callret->Attach(impl->thread);

    UT_INFO("forked guest thread: rip=%#llx rsp=%#llx",
                static_cast<unsigned long long>(state.rip),
                static_cast<unsigned long long>(state.gregs[FEXCore::X86State::REG_RSP]));
    return std::unique_ptr<GuestThread> {new GuestThread {std::move(impl)}};
}

} // namespace utsuro
