// jit.h -- the one executable region FEXCore compiles into.
//
// On the phone no process can map executable memory itself (TXM): a debugger (StikDebug, TrollStore's
// enable-jit, Dopamine's CS_DEBUGGED) grants it, once, at launch. That region is a dual mapping -- code is
// written through a writable alias and run through the executable view -- and FEXCore is told about it
// through JITHostHooks so it never asks for more. The Mac build uses the identical path with a region the
// process maps for itself, so what is tested on the Mac is what ships.
#pragma once

#include <cstddef>
#include <string>

namespace utsuro::jit {

/// Bytes of JIT memory to claim (UTSURO_JIT_MIB overrides, in MiB).
size_t RegionBytes();

/// Claims the region at app launch while a debugger is still attached (iOS), and holds it. Safe to call on
/// the Mac, where it does nothing. Returns false if no executable memory could be had.
/// `self_only` makes the region without any trap to a debugger (before iOS 26, where a process marked as debugged may map its own).
bool Prewarm(bool self_only = false);

/// Opens the arena on the held region and hands it to FEXCore. Call before the first FexEngine::Create.
bool Install(std::string& error);

/// Lets the debugger go. The region stays valid; nothing else can be requested afterwards.
void Detach();

} // namespace utsuro::jit
