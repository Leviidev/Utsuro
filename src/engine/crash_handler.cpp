// crash_handler.cpp -- the last thing Fathom does when the process is about to die.
//
// A sideloaded app has no debugger attached and no crash report you can read on the
// device, and a hard fault inside the JIT simply ends the process: the log file stops
// mid-line with no indication of why. This writes one final record first.
//
// Everything here runs inside a signal handler after something has already gone badly
// wrong, so it uses only async-signal-safe calls: a file descriptor opened ahead of
// time, write(), and hand-rolled number formatting. No malloc, no stdio, no locks.

#include "crash_handler.h"

#include <atomic>
#include <csignal>
#include <initializer_list>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <execinfo.h>
#include <unistd.h>

namespace utsuro {
namespace {

int g_crash_fd = -1;
/// A descriptor onto /dev/null, for asking the kernel whether an address is readable:
/// write() answers EFAULT where a load would raise a second fault inside this handler.
int g_null_fd = -1;
std::atomic<FaultRecovery> g_recovery {nullptr};
std::atomic<GuestStateDescriber> g_describer {nullptr};
std::atomic<GuestFaultEnder> g_fault_ender {nullptr};

// Updated on every guest syscall. Plain atomics, so reading them from a signal handler
// is safe -- and the last syscall the guest made is usually the single most useful fact
// about where it died.
std::atomic<uint64_t> g_last_syscall {UINT64_MAX};

// Per host thread, so that the handler -- which runs on the thread that faulted -- can
// say who it was. A plain char buffer rather than a std::string: this is read from a
// signal handler, where allocating is not allowed.
thread_local uint64_t t_last_syscall = UINT64_MAX;
thread_local uint64_t t_last_syscall_arg = 0;
thread_local uint64_t t_last_rip = 0;
thread_local int t_pid = 0;
thread_local int t_tid = 0;
thread_local bool t_is_32bit = false;
thread_local char t_program[256] = {};
std::atomic<uint64_t> g_last_syscall_arg {0};
std::atomic<uint64_t> g_syscall_count {0};
std::atomic<uint64_t> g_last_guest_rip {0};

void WriteRaw(const char* text, size_t length) {
    if (g_crash_fd < 0) {
        return;
    }
    ssize_t ignored = write(g_crash_fd, text, length);
    (void)ignored;
}

void WriteText(const char* text) {
    WriteRaw(text, std::strlen(text));
}

void WriteHex(uint64_t value) {
    char buffer[19];
    buffer[0] = '0';
    buffer[1] = 'x';
    size_t position = sizeof(buffer);
    if (value == 0) {
        buffer[--position] = '0';
    }
    while (value != 0 && position > 2) {
        const auto digit = static_cast<unsigned>(value & 0xF);
        buffer[--position] = static_cast<char>(digit < 10 ? '0' + digit : 'a' + (digit - 10));
        value >>= 4;
    }
    WriteRaw("0x", 2);
    WriteRaw(buffer + position, sizeof(buffer) - position);
}

void WriteDecimal(uint64_t value) {
    char buffer[21];
    size_t position = sizeof(buffer);
    if (value == 0) {
        buffer[--position] = '0';
    }
    while (value != 0 && position > 0) {
        buffer[--position] = static_cast<char>('0' + (value % 10));
        value /= 10;
    }
    WriteRaw(buffer + position, sizeof(buffer) - position);
}

const char* SignalName(int number) {
    switch (number) {
    case SIGSEGV: return "SIGSEGV (bad memory access)";
    case SIGBUS: return "SIGBUS (bad address / alignment)";
    case SIGILL: return "SIGILL (illegal instruction)";
    case SIGTRAP: return "SIGTRAP (breakpoint -- usually a JIT trap with no debugger to service it)";
    case SIGABRT: return "SIGABRT (abort)";
    case SIGFPE: return "SIGFPE (arithmetic)";
    default: return "signal";
    }
}

/// Dumps the guest's state without dying, on SIGUSR1.
///
/// A guest that hangs gives nothing away: no syscalls, no signal, no output, just a
/// process at 100% CPU. This makes the state readable on demand -- send SIGUSR1 and the
/// log says where the guest actually is.
void Report(int, siginfo_t*, void* context) {
    WriteText("\n=== FATHOM STATE ===\nguest syscalls so far: ");
    WriteDecimal(g_syscall_count.load(std::memory_order_relaxed));
    const uint64_t last = g_last_syscall.load(std::memory_order_relaxed);
    if (last != UINT64_MAX) {
        WriteText("\nlast guest syscall: ");
        WriteDecimal(last);
    }
    if (context != nullptr) {
        auto* uc = static_cast<ucontext_t*>(context);
        WriteText("\nhost pc: ");
        WriteHex(uc->uc_mcontext->__ss.__pc);
    }
    if (auto* describer = g_describer.load(std::memory_order_acquire)) {
        char state[1024];
        const size_t length = describer(state, sizeof(state));
        if (length > 0) {
            WriteText("\nguest registers:\n");
            ssize_t ignored = write(g_crash_fd, state, length);
            (void)ignored;
        }
    }
    WriteText("=== END STATE ===\n");
    fsync(g_crash_fd);
}

void Handle(int number, siginfo_t* info, void* context) {
    // An unaligned guest access is a normal event, not a crash: x86 permits unaligned
    // memory access and the ARM64 instructions FEXCore emits for it do not. The engine's
    // recovery hook emulates the access and moves the PC past it, and execution carries
    // on as if nothing happened. Only if nobody claims the fault is it really fatal.
    if (const auto recovery = g_recovery.load(std::memory_order_acquire)) {
        if (recovery(number, info, context)) {
            return;
        }
    }

    WriteText("\n=== FATHOM CRASH ===\nsignal: ");
    WriteText(SignalName(number));
    WriteText("\nfault address: ");
    WriteHex(info == nullptr ? 0 : reinterpret_cast<uint64_t>(info->si_addr));

    if (t_pid != 0) {
        WriteText("\nin: pid ");
        WriteDecimal(static_cast<uint64_t>(t_pid));
        if (t_tid != 0 && t_tid != t_pid) {
            WriteText(" tid ");
            WriteDecimal(static_cast<uint64_t>(t_tid));
        }
        WriteText(t_is_32bit ? " (32-bit) " : " (64-bit) ");
        WriteText(t_program);
    }

    WriteText("\nguest syscalls so far: ");
    WriteDecimal(g_syscall_count.load(std::memory_order_relaxed));

    // This thread's own history first. The global figures below are every thread's put
    // together, which is the wrong question to answer about a crash.
    if (t_last_syscall != UINT64_MAX) {
        WriteText("\nlast syscall on this thread: ");
        WriteDecimal(t_last_syscall);
        WriteText(" arg0=");
        WriteHex(t_last_syscall_arg);
    } else {
        WriteText("\nlast syscall on this thread: none");
    }
    if (t_last_rip != 0) {
        WriteText("\nlast guest RIP on this thread: ");
        WriteHex(t_last_rip);
    }

    const uint64_t last = g_last_syscall.load(std::memory_order_relaxed);
    if (last != UINT64_MAX) {
        WriteText("\nlast guest syscall anywhere: ");
        WriteDecimal(last);
        WriteText(" arg0=");
        WriteHex(g_last_syscall_arg.load(std::memory_order_relaxed));
    }
    WriteText("\n=== END ===\n");

    // The ARM64 instruction that faulted, as a raw word. Decoding it says which kind of
    // access went wrong -- a plain load, a store-release, a pre-indexed push -- which is
    // what distinguishes an emulation bug from a guest one.
    if (context != nullptr) {
        auto* uc = static_cast<ucontext_t*>(context);
        const uint64_t pc = uc->uc_mcontext->__ss.__pc;
        WriteText("\nfaulting host pc: ");
        WriteHex(pc);
        // Only when the faulting address is not the program counter itself. When it is,
        // the fault was an instruction fetch -- a jump to memory that is not there -- and
        // reading that address to report it faults again, inside this handler, forever.
        // The symptom is not a crash report but a process pinned at 100% CPU with no
        // output at all, which is considerably harder to recognise.
        const uint64_t fault = info == nullptr ? 0 : reinterpret_cast<uint64_t>(info->si_addr);
        if (fault == pc) {
            WriteText("\nfaulting instruction: unreadable -- the fault was the fetch itself,"
                      " so this is a jump to an address with no code at it\n");
        } else {
            // Not read at all. Generated code is mapped execute-only, the interesting
            // faults are the ones where this address is not there, and a load that faults
            // inside this handler stops the thread here for good -- holding whatever
            // FEXCore lock it was holding when it faulted, which stops the session.
            WriteText("\nfaulting instruction: not read, to avoid faulting again here\n");
        }
    }

    // The guest's own registers. A fault at a small address means some pointer was null;
    // this is what says which one, and what the code was doing with it.
    if (auto* describer = g_describer.load(std::memory_order_acquire)) {
        char state[1024];
        const size_t length = describer(state, sizeof(state));
        if (length > 0) {
            WriteText("\nguest registers:\n");
            ssize_t ignored = write(g_crash_fd, state, length);
            (void)ignored;
        }
    }

    // A host backtrace, which is the difference between "it crashed somewhere" and
    // knowing which function. backtrace() walks the frame pointers and touches no locks,
    // which is about as safe as anything gets inside a signal handler; backtrace_symbols
    // would allocate, so the raw addresses are written and symbolised afterwards with
    // atos or llvm-symbolizer.
    WriteText("\nhost backtrace:\n");
    void* frames[32];
    const int depth = backtrace(frames, 32);
    backtrace_symbols_fd(frames, depth, g_crash_fd);

    fsync(g_crash_fd);

    // A fault in guest code belongs to the guest process that ran it, not to Fathom. If
    // that process can be ended on its own -- which needs the fault to have happened
    // inside the JIT's generated code -- this call unwinds and never comes back, and the
    // rest of the session carries on the way Linux would have carried on.
    if (const auto ender = g_fault_ender.load(std::memory_order_acquire)) {
        if (ender(number, info, context)) {
            return;
        }
    }

    // Restore the default action and re-raise, so the process still dies the way it
    // would have and iOS still records its own crash report.
    signal(number, SIG_DFL);
    raise(number);
}

} // namespace

void SetFaultRecovery(FaultRecovery recovery) {
    g_recovery.store(recovery, std::memory_order_release);
}

void SetGuestFaultEnder(GuestFaultEnder ender) {
    g_fault_ender.store(ender, std::memory_order_release);
}

void SetGuestStateDescriber(GuestStateDescriber describer) {
    g_describer.store(describer, std::memory_order_release);
}

void NoteSyscall(uint64_t number, uint64_t first_argument, uint64_t count) {
    t_last_syscall = number;
    t_last_syscall_arg = first_argument;
    g_last_syscall.store(number, std::memory_order_relaxed);
    g_last_syscall_arg.store(first_argument, std::memory_order_relaxed);
    g_syscall_count.store(count, std::memory_order_relaxed);
}

void NoteGuestRip(uint64_t rip) {
    t_last_rip = rip;
    g_last_guest_rip.store(rip, std::memory_order_relaxed);
}

uint64_t LastSyscallHere() {
    return t_last_syscall;
}

uint64_t LastGuestRipHere() {
    return t_last_rip;
}

void NoteGuestIdentity(int pid, int tid, bool is_32bit, const char* program) {
    t_pid = pid;
    t_tid = tid;
    t_is_32bit = is_32bit;
    if (program != nullptr) {
        size_t index = 0;
        for (; index + 1 < sizeof(t_program) && program[index] != '\0'; ++index) {
            t_program[index] = program[index];
        }
        t_program[index] = '\0';
    }
}

} // namespace utsuro

extern "C" void utsuro_install_crash_handler(const char* log_path) {
    if (log_path == nullptr) {
        return;
    }
    // Opened now, while everything still works. Opening a file from inside a signal
    // handler is exactly the kind of thing that fails when it matters.
    // Appended to across runs, but started afresh once it passes 64 MB: a session that
    // faults in a loop otherwise grows it without end (it reached 2.7 GB in the temp folder).
    struct stat existing {};
    const bool too_big = stat(log_path, &existing) == 0 && existing.st_size > (64 << 20);
    utsuro::g_crash_fd = open(log_path, O_WRONLY | O_APPEND | O_CREAT | (too_big ? O_TRUNC : 0), 0644);
    if (utsuro::g_crash_fd < 0) {
        return;
    }

    struct sigaction action {};
    action.sa_sigaction = utsuro::Handle;
    action.sa_flags = SA_SIGINFO;
    sigemptyset(&action.sa_mask);

    if (utsuro::g_null_fd < 0) {
        utsuro::g_null_fd = open("/dev/null", O_WRONLY | O_CLOEXEC);
    }
    for (const int number : {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT, SIGTRAP}) {
        sigaction(number, &action, nullptr);
    }

    // Not a crash: a way to ask a running -- or stuck -- guest where it is.
    struct sigaction report {};
    report.sa_sigaction = utsuro::Report;
    report.sa_flags = SA_SIGINFO | SA_RESTART;
    sigemptyset(&report.sa_mask);
    sigaction(SIGUSR1, &report, nullptr);
}
