// darwin_support.cpp -- host symbols FEXCore needs that iOS does not provide.

#include <libkern/OSCacheControl.h>
#include <cstddef>

// FEXCore's JIT writes ARM64 instructions and then has to make the CPU see them, which it
// does through __builtin___clear_cache. On this toolchain that builtin lowers to a call to
// __clear_cache rather than emitting the cache-maintenance instructions inline -- and
// Apple's device compiler-rt archive (libclang_rt.ios.a) has no such symbol, so the app
// link fails with an undefined reference from deep inside FEXCore's dispatcher.
// (The simulator archive does carry it, which is why a simulator build would hide this.)
//
// Providing it here through Darwin's own public cache-control call is both the smallest
// fix and the correct one: sys_icache_invalidate is exactly the primitive this builtin
// is meant to reach on Apple platforms.
extern "C" void __clear_cache(void* start, void* end) {
    sys_icache_invalidate(start, static_cast<size_t>(static_cast<char*>(end) - static_cast<char*>(start)));
}
