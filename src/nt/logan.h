// logan.h -- the console's audio processor ("Logan": ACP, SHAPE and friends) as the title's AcpHal.dll talks to it. The real thing is
// a device (\??\ACPI_HAL#Logan#0#{...}) the HAL maps memory through and exchanges command and message rings with. Here the device is
// answered by the host: memory the HAL allocates is ordinary guest memory with an "APU address" that this layer translates, and a
// thread plays the ACP's part of the rings. The layout and behaviour follow WinDurango's Logan emulation (WinDurango.KernelX).
#pragma once

#include "nt/kernel.h"

#include <cstdint>
#include <string>

namespace utsuro::nt::logan {

/// Whether an NT path names the Logan device.
bool IsDevicePath(const std::string& nt_path);

/// DeviceIoControl on the device. Returns an NTSTATUS; `*returned` is the number of bytes written to `out`.
NTSTATUS Ioctl(Kernel& kernel, uint32_t code, const uint8_t* in, uint32_t in_len, uint8_t* out, uint32_t out_len, uint64_t* returned);

/// Memory the HAL (or the title through acphal!ApuAlloc) holds in guest address space `va`..`va + size`: gives it an APU address.
uint32_t MapMemory(uint64_t va, uint32_t size);

/// The APU address of guest address `va`: inside a mapping it is that mapping's; otherwise `size` bytes there are mapped now.
uint32_t ApuFor(uint64_t va, uint32_t size);

/// Forgets the mapping that starts at `va`.
void UnmapMemory(uint64_t va);

} // namespace utsuro::nt::logan
