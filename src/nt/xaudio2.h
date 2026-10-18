// xaudio2.h -- the host's XAudio2 (xaudio2.cpp).
#pragma once

#include "nt/kernel.h"
#include "nt/winrt.h"

namespace utsuro::nt::xaudio2 {

/// Defines the IXAudio2 and voice interfaces with the runtime.
void Register(winrt::Runtime& rt);
/// xaudio2_9!CreateXAudio2Object(IXAudio2** engine, UINT32 flags, UINT32 processor).
uint64_t CreateXAudio2Object(Kernel& kernel, NtThread& thread, SyscallArgs& args);

} // namespace utsuro::nt::xaudio2
