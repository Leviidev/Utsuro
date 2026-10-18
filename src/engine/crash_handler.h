// crash_handler.h -- breadcrumbs the crash handler reads after everything has gone wrong.
#pragma once

#include <csignal>
#include <cstdint>

namespace utsuro {

/// Offered every fatal signal before the crash record is written. Returning true means
/// the fault was fixed up in `context` and execution should resume there.
using FaultRecovery = bool (*)(int signal, siginfo_t* info, void* context);

/// Installs the recovery hook. The engine uses this for guest alignment faults, which
/// are an expected part of running x86 code on ARM64 rather than a crash.
void SetFaultRecovery(FaultRecovery recovery);

/// Called after a crash has been reported and before the process dies. If it returns --
/// it usually does not, because it unwinds -- the fault was not one a guest process could
/// be blamed for, and the process still dies.
using GuestFaultEnder = bool (*)(int signal, siginfo_t* info, void* context);
void SetGuestFaultEnder(GuestFaultEnder ender);

/// Writes a description of the currently executing guest thread's registers into `buffer`.
/// Supplied by the engine, because only it knows FEXCore exists. Returns the length
/// written. Runs inside a signal handler, so it must not allocate or take locks.
using GuestStateDescriber = size_t (*)(char* buffer, size_t capacity);

/// Installs the describer. Without one, a guest fault records only its address, which
/// says a pointer was null but nothing about which one.
void SetGuestStateDescriber(GuestStateDescriber describer);

/// Records the guest syscall about to be serviced. Cheap enough for the hot path
/// (three relaxed atomic stores) and it is what turns "the log stops here" into
/// "the log stops in openat".
void NoteSyscall(uint64_t number, uint64_t first_argument, uint64_t count);

/// Records where the guest was, as of the last syscall boundary.
void NoteGuestRip(uint64_t rip);

/// This thread's last syscall and the guest address it was made from, for a report that
/// needs to say where a thread was before it went wrong.
uint64_t LastSyscallHere();
uint64_t LastGuestRipHere();

/// Which guest process and thread this host thread is running, and which program. Kept
/// per host thread rather than globally, because a crash report that names the last
/// syscall any thread made says nothing about the one that died.
void NoteGuestIdentity(int pid, int tid, bool is_32bit, const char* program);

} // namespace utsuro
