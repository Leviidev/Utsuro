#include "jit/jit.h"

#include "engine/log.h"

#include "jit/husk-ios-jit.h"
#include "jit/husk-jit-arena.h"

#include <FEXCore/Utils/AllocatorHooks.h>

#include <TargetConditionals.h>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <mutex>
#include <thread>

extern "C" void tl_log_line(const char* fmt, ...) {
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    UT_INFO("%s", line);
}

namespace utsuro::jit {

size_t RegionBytes() {
    if (const char* mib = std::getenv("UTSURO_JIT_MIB")) {
        const long v = std::atol(mib);
        if (v > 0) return static_cast<size_t>(v) << 20;
    }
    return size_t {256} << 20;
}

bool Prewarm() {
#if TARGET_OS_IPHONE
    husk_ios_jit_install_trap_handler();
    return husk_ios_jit_prewarm(RegionBytes());
#else
    return true;
#endif
}

namespace {

void* HookAlloc(size_t size, void** exec_out) { return husk_jit_arena_alloc(size, exec_out); }
int HookFree(void* write_addr, size_t size) { return husk_jit_arena_free(write_addr, size) ? 0 : -1; }

} // namespace

bool Install(std::string& error) {
    static std::once_flag once;
    static bool ok = false;
    static std::string failure;
    std::call_once(once, [] {
        char err[256] = {};
        if (!husk_jit_arena_open(RegionBytes(), 0, err, sizeof err)) {
            failure = err;
            return;
        }
        FEXCore::Allocator::SetJITHostHooks(FEXCore::Allocator::JITHostHooks {
            .Alloc = HookAlloc,
            .Free = HookFree,
            .Delta = husk_jit_arena_delta(),
            .RWBase = reinterpret_cast<uintptr_t>(husk_jit_arena_rw_base()),
            .Size = husk_jit_arena_held_bytes(),
        });
        ok = true;
        // How much of the region the game has actually used, so the size on the phone can be argued from a number.
        std::thread([] {
            size_t last = 0;
            for (;;) {
                std::this_thread::sleep_for(std::chrono::seconds(5));
                const size_t high = husk_jit_arena_high_water_bytes();
                if (high != last) {
                    UT_INFO("jit-arena: %zu MiB in use, high water %zu MiB of %zu MiB, %zu refusals",
                            husk_jit_arena_used_bytes() >> 20, high >> 20, husk_jit_arena_held_bytes() >> 20,
                            husk_jit_arena_refusals());
                    last = high;
                }
            }
        }).detach();
    });
    if (!ok) error = "no JIT memory: " + failure;
    return ok;
}

void Detach() {
#if TARGET_OS_IPHONE
    husk_ios_jit_detach();
#endif
}

} // namespace utsuro::jit
