// engine.h -- the FEXCore embedding.
//
// This is the only file that knows FEXCore exists. It owns the context, the guest
// threads, the syscall handler FEXCore calls into, and the mechanism that unwinds out of
// the JIT when a guest thread exits.
//
// The split between the two classes here is the process model. FEXCore's *context* is the
// JIT and its code cache, and there is one of those. A *guest thread* is a register file,
// a stack and an unwind point, and there is one per thread of guest execution -- which,
// in Fathom, is also one per guest process, because processes are guest threads that do
// not share a file descriptor table.
#pragma once

#include "guest_memory.h"
#include <array>
#include <functional>
#include <vector>
#include <pthread.h>

#include <cstdint>
#include <memory>
#include <string>

namespace utsuro {

/// A guest thread's registers, as a context switch has to carry them.
struct GuestCpuState {
    uint64_t gregs[16] {};  ///< x86 encoding order: rax, rcx, rdx, rbx, rsp, rbp, rsi, rdi, r8..r15.
    uint64_t rip {};
    uint32_t eflags {};
    uint32_t mxcsr {};
    uint64_t fs_base {};
    uint64_t gs_base {};
    uint64_t xmm[16][2] {};
    uint64_t ymm_high[16][2] {};
    uint64_t mm[8][2] {};
    uint16_t fcw {};
    uint8_t ftw {};
};

/// What the syscall layer needs from whatever is driving the CPU.
class GuestThreadControl {
public:
    virtual ~GuestThreadControl() = default;
    virtual bool CaptureCpuState(GuestCpuState* out) const { (void)out; return false; }
    /// Where a thread that is running right now is (a signal samples it): its rip and the sixteen general registers. For debugging.
    virtual bool SampleRunning(uint64_t* rip, uint64_t gregs[16]) { (void)rip; (void)gregs; return false; }
    virtual bool RestoreCpuState(const GuestCpuState& state) { (void)state; return false; }
    virtual void AdvanceGuestRip(int64_t bytes) { (void)bytes; }
    virtual void SetFsBase(uint64_t base) = 0;
    virtual uint64_t GetFsBase() const = 0;
    /// Windows x64 keeps the TEB at gs:[0x30]; this is where it is set.
    virtual void SetGsBase(uint64_t base) = 0;
    virtual uint64_t GetGsBase() const = 0;
    virtual void SetTlsDescriptor(int entry, uint32_t base, uint32_t limit, bool point_gs_at_it = false) = 0;
    virtual uint64_t GuestRip() const = 0;
    [[noreturn]] virtual void ExitGuest(int status) = 0;
    [[noreturn]] virtual void ExecGuest() = 0;
};

/// Answers a guest thread's `syscall` instructions. `Handle` receives FEXCore's
/// OS_LINUX64 argument order: the number from RAX, then RDI, RSI, RDX, R10, R8, R9.
class SyscallSink {
public:
    virtual ~SyscallSink() = default;
    virtual uint64_t Handle(uint64_t number, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5,
                            uint64_t a6) = 0;
    virtual void SetHostThread(pthread_t) {}
    virtual bool StopRequested() const { return false; }
    virtual int Pid() const { return 1; }
    virtual int Tid() const { return 1; }
    virtual class GuestAddressSpace* Space() const { return nullptr; }
};

struct EngineOptions {
    uint32_t max_inst_per_block {};  ///< 0 keeps FEXCore's own default.
    bool multiblock {true};
    bool tso_enabled {true};
    bool reduced_precision_x87 {false};
    bool disassemble {false};
    /// Reports no AVX through CPUID. glibc picks its string routines by IFUNC from what
    /// CPUID advertises, so this is the switch that decides whether it runs the AVX2
    /// strcmp or the SSE2 one -- and therefore whether a JIT bug in the wide paths is
    /// reachable at all.
    bool disable_avx {false};

    /// True when the guest is an i386 binary. Its pointers are 32 bits, so it cannot be
    /// mapped 1:1 -- `guest_memory_base` is where its address space really lives, and
    /// FEXCore adds it to every address the guest computes.
    bool guest_is_32bit {false};
    uint64_t guest_memory_base {0};
};

enum class RunOutcome {
    Exited,   ///< The guest called exit/exit_group.
    Halted,   ///< The guest executed HLT, or ran off the end of its code.
    Stopped,  ///< fathom_session_request_stop unwound it.
    Execed,   ///< The guest called execve; the caller runs the new image on a new thread.
    Faulted,
};

struct RunResult {
    RunOutcome outcome {RunOutcome::Faulted};
    int status {};
    uint64_t rip {};
    uint64_t rsp {};
    std::string message;
};

class FexEngine;

/// One thread of guest execution, with its own registers, call/return stack and unwind
/// point. Its syscalls are answered by the SyscallSink it was created with, which is
/// what gives a forked child its own file descriptor table.
class GuestThread final : public GuestThreadControl {
public:
    ~GuestThread() override;

    GuestThread(const GuestThread&) = delete;
    GuestThread& operator=(const GuestThread&) = delete;

    /// Runs until this thread stops. Blocking, and valid only on the host thread that
    /// owns it -- FEXCore binds a guest thread to the host thread executing it.
    RunResult Run();

    uint64_t Rip() const;
    uint64_t Rsp() const;
    uint64_t Rax() const;

    /// Points this thread at a freshly loaded program image, which is what execve does:
    /// same thread, same pid, entirely different program.
    void ResetTo(uint64_t rip, uint64_t rsp);

    /// Unwinds out of the JIT so the process can be restarted on a freshly loaded image.
    /// execve cannot simply rewrite this thread's registers: the syscall that asked for it
    /// is still several JIT frames deep, and the only way out of there is the same unwind
    /// that exit uses.
    [[noreturn]] void ExecGuest() override;

    // GuestThreadControl
    void SetFsBase(uint64_t base) override;
    uint64_t GetFsBase() const override;
    void SetGsBase(uint64_t base) override;
    uint64_t GetGsBase() const override;
    void SetTlsDescriptor(int entry, uint32_t base, uint32_t limit,
                          bool point_gs_at_it = false) override;
    uint64_t GuestRip() const override { return Rip(); }
    bool CaptureCpuState(GuestCpuState* out) const override;
    bool SampleRunning(uint64_t* rip, uint64_t gregs[16]) override;
    bool RestoreCpuState(const GuestCpuState& state) override;
    void AdvanceGuestRip(int64_t bytes) override;

    /// Unwinds this thread out of the JIT with a fatal signal recorded, so that the guest
    /// process it belongs to ends rather than the emulator. Returns false, without
    /// unwinding, if the thread is not currently executing guest code.
    bool EndOnFault(int signal);
    [[noreturn]] void ExitGuest(int status) override;

private:
    friend class FexEngine;
    class Impl;
    explicit GuestThread(std::unique_ptr<Impl> impl);

    std::unique_ptr<Impl> impl_;
};

/// Drops every compiled block covering a host address range, in every live guest thread.
///
/// Guest addresses get handed out again -- an exec releases a program's image and the
/// next one is loaded over it -- and FEXCore's block cache is keyed by guest address, so
/// without this the new program runs the old program's compiled code.
void InvalidateCompiledCode(uint64_t host_begin, uint64_t host_end);

/// Called on a guest fault with the guest rip and rax..r15, to print more than the engine knows.
void SetFaultReporter(std::function<void(uint64_t, const uint64_t*)> reporter);

/// Appends the guest rip of every thread currently inside compiled code.
void SampleGuestRips(std::vector<uint64_t>& out, std::vector<std::array<uint64_t, 16>>* regs_out = nullptr);

/// Suspends every guest thread in turn and logs where it is. For hangs.
void DumpGuestThreads(const std::function<std::string(uint64_t)>& describe = {},
                      const std::function<void(uint64_t, const uint64_t*)>& with_state = {});

/// Puts invalidation off until every suspended guest thread is running again.
///
/// A fork suspends the parent's other threads so the child can borrow its memory, and a
/// thread stopped in the middle of the JIT is holding FEXCore's code-invalidation lock
/// shared. Nothing can take that lock exclusively until it runs again -- and the thaw
/// that would let it run is on the far side of the child's exec, which is itself waiting
/// for the lock. Every other guest process waits there too, because there is one lock
/// for the session. So while anything is suspended, invalidation is recorded rather than
/// performed, and the ranges are dropped together when the last thread is thawed.
void HoldInvalidations();
void ReleaseInvalidations();

/// Queues a range to be thrown away by a thread of the runtime's own, rather than by the
/// caller. For callers that are holding something: the process table, or a guest thread
/// that must not stop here. The range is dropped as soon as nothing is suspended.
void InvalidateCompiledCodeLater(uint64_t host_begin, uint64_t host_end);

/// The code-invalidation lock's raw state, for the watchdog. Readers in the low bits,
/// waiting writers above them, and the top bit set while a writer owns it. A session
/// that has stopped with a writer waiting and a reader count that never falls is a
/// reader that went away without letting go.
void DescribeCodeLocks(char* buffer, size_t capacity);

class FexEngine {
public:
    static std::unique_ptr<FexEngine> Create(GuestAddressSpace& space, const EngineOptions& options,
                                             std::string& error);
    ~FexEngine();

    FexEngine(const FexEngine&) = delete;
    FexEngine& operator=(const FexEngine&) = delete;

    /// A program's first thread, entering at its entry point with a defined register file.
    std::unique_ptr<GuestThread> StartThread(uint64_t rip, uint64_t rsp, SyscallSink& syscalls,
                                             std::string& error);

    /// A thread whose registers are a copy of `parent`'s, except that RAX is zero: that
    /// difference is the whole of what fork returns to a child. A non-zero `new_rsp` puts
    /// it on a stack of its own, which is what clone does for a thread and fork does not.
    std::unique_ptr<GuestThread> ForkThread(const GuestThread& parent, SyscallSink& syscalls,
                                            std::string& error, uint64_t new_rsp = 0);

    /// The guest thread executing on this host thread, or nullptr. A syscall that has to
    /// look at the caller's registers -- clone, which copies them into a new thread --
    /// needs the thread that actually made the call, not the process's first one.
    static GuestThread* Current();

    /// Stops this context compiling anything until the pause is lifted.
    ///
    /// Suspending a guest thread that is halfway through compiling a block leaves it
    /// holding FEXCore's code-invalidation lock, which nothing can then take -- the next
    /// program load waits on it forever, and because that lock gives writers priority,
    /// every other thread stops behind it. Waiting for compilation to stop before any
    /// thread is suspended is what makes stopping them safe.
    void PauseCompilation();
    void ResumeCompilation();

    static const char* FexRevision();

private:
    class Impl;
    explicit FexEngine(std::unique_ptr<Impl> impl);

    std::unique_ptr<Impl> impl_;
};

/// Debugging: the general registers (rax..r15) the calling host thread's last guest fault stopped at.
bool LastFaultRegisters(uint64_t gregs[16], uint64_t* rip);

} // namespace utsuro
