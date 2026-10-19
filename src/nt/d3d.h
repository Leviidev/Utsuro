// d3d.h -- the host's Direct3D 11.X device (d3d.cpp).
#pragma once

#include "nt/kernel.h"
#include "nt/winrt.h"

namespace utsuro::nt::d3d {

/// Defines every D3D11.X / DXGI interface with the runtime.
/// `os_timestamp` is the link time of the dump's ntdll; it selects the interface layouts of that OS build.
void Register(winrt::Runtime& rt, uint32_t os_timestamp);
/// d3d11_x!D3D11CreateDevice.
uint64_t CreateDevice(Kernel& kernel, NtThread& thread, SyscallArgs& args);
uint64_t CreateDeviceX(Kernel& kernel, NtThread& thread, SyscallArgs& args);

/// d3d11_x!DXGIXPresentArray(sync, immediate threshold, flags, count, IDXGISwapChain1** chains, parameters): the console's present.
uint64_t PresentArray(Kernel& kernel, NtThread& thread, SyscallArgs& args);

} // namespace utsuro::nt::d3d
