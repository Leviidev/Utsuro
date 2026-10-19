// safe_mem.h -- reading guest memory without trusting it: reserved-but-uncommitted pages fault, so the kernel does the read.
#pragma once

#include <mach/mach.h>

#include <cstdint>

namespace utsuro::nt {

inline bool SafeRead(uint64_t address, void* destination, uint64_t bytes) {
    vm_size_t done = 0;
    return address != 0 &&
           vm_read_overwrite(mach_task_self(), static_cast<vm_address_t>(address), static_cast<vm_size_t>(bytes), reinterpret_cast<vm_address_t>(destination), &done) == KERN_SUCCESS &&
           done == bytes;
}

} // namespace utsuro::nt
