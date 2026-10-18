// d3d.h -- the host's Direct3D 11.X device (d3d.cpp).
#pragma once

#include "nt/kernel.h"
#include "nt/winrt.h"

namespace utsuro::nt::d3d {

/// Defines every D3D11.X / DXGI interface with the runtime.
void Register(winrt::Runtime& rt);
/// d3d11_x!D3D11CreateDevice.
uint64_t CreateDevice(Kernel& kernel, NtThread& thread, SyscallArgs& args);

} // namespace utsuro::nt::d3d
