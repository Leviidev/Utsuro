// xg.h -- running the console OS's own xg_x.dll (texture tiling arithmetic) from the host.
//
// Placement textures hold their texels in the console GPU's tiled layout; xg_x.dll is the library that knows it. Its code is plain
// arithmetic, so a helper guest thread runs it on request: the host puts a call (function and up to eight arguments) in a job block,
// wakes the thread, and waits for the result.
#pragma once

#include "nt/kernel.h"

#include <initializer_list>
#include <vector>

namespace utsuro::nt::xg {

/// xg_x.dll was mapped at `base` (called for every image; others are ignored).
void NoteImage(const std::string& image, uint64_t base);
/// The address of an export of xg_x.dll (0 when it is not loaded).
uint64_t Export(const char* name);
/// Whether an address lies inside the mapped xg_x.dll.
bool InImage(uint64_t address);

/// Calls the guest function `fn` with the arguments on the helper thread and returns its rax; `~0` when the helper is not available.
uint64_t Call(Kernel& kernel, uint64_t fn, std::initializer_list<uint64_t> args);
uint64_t Call(Kernel& kernel, uint64_t fn, const std::vector<uint64_t>& args);

} // namespace utsuro::nt::xg
