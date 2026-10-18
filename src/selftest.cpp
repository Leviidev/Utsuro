// selftest.cpp -- runs hand-assembled x86-64 snippets through the engine, to settle
// whether a misbehaviour is the emulator's or the guest's.
#include "engine/engine.h"
#include "engine/guest_memory.h"
#include "engine/log.h"

#include <cstdio>
#include <cstring>
#include <vector>

using namespace utsuro;

namespace {
class NullSink : public SyscallSink {
public:
    uint64_t Handle(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t) override { return 0; }
};

void Emit(std::vector<uint8_t>& c, std::initializer_list<uint8_t> bytes) { c.insert(c.end(), bytes); }
void EmitQ(std::vector<uint8_t>& c, uint64_t v) { for (int i = 0; i < 8; ++i) c.push_back(static_cast<uint8_t>(v >> (8 * i))); }
} // namespace

int RunSelfTest() {
    std::string error;
    GuestAddressSpace* space = GuestAddressSpace::Reserve(1ull << 30, error, 0x10000);
    if (!space) { std::fprintf(stderr, "reserve: %s\n", error.c_str()); return 1; }
    EngineOptions eo;
    auto engine = FexEngine::Create(*space, eo, error);
    if (!engine) { std::fprintf(stderr, "engine: %s\n", error.c_str()); return 1; }

    const uint64_t data = space->Allocate(0x4000, 0, kGuestProtRead | kGuestProtWrite);
    const uint64_t code = space->Allocate(0x4000, 0, kGuestProtRead | kGuestProtWrite | kGuestProtExec);
    const uint64_t stack = space->Allocate(0x10000, 0, kGuestProtRead | kGuestProtWrite);

    std::vector<uint8_t> c;
    Emit(c, {0x48, 0xB9}); EmitQ(c, data);                   // mov rcx, data
    Emit(c, {0xC7, 0x01, 0x01, 0, 0, 0});                    // mov dword [rcx], 1
    Emit(c, {0x49, 0x89, 0xC8});                             // mov r8, rcx
    Emit(c, {0xBA, 0x01, 0, 0, 0});                          // mov edx, 1
    Emit(c, {0x8D, 0x4A, 0x01});                             // lea ecx, [rdx+1]
    Emit(c, {0x89, 0xD0});                                   // mov eax, edx
    Emit(c, {0x41, 0x0F, 0x0D, 0x48, 0x00});                 // prefetchw [r8+0]
    Emit(c, {0xF0, 0x41, 0x0F, 0xB1, 0x48, 0x00});           // lock cmpxchg dword [r8], ecx
    Emit(c, {0x0F, 0x94, 0xC3});                             // sete bl
    Emit(c, {0x41, 0x89, 0x40, 0x08});                       // mov [r8+8], eax
    Emit(c, {0x41, 0x88, 0x58, 0x0C});                       // mov [r8+12], bl
    // second: a failing compare (expect eax stays loaded, ZF=0)
    Emit(c, {0xB8, 0x09, 0, 0, 0});                          // mov eax, 9
    Emit(c, {0xB9, 0x07, 0, 0, 0});                          // mov ecx, 7
    Emit(c, {0xF0, 0x41, 0x0F, 0xB1, 0x48, 0x00});           // lock cmpxchg dword [r8], ecx   (mem=2, eax=9 -> fail)
    Emit(c, {0x0F, 0x94, 0xC3});                             // sete bl
    Emit(c, {0x41, 0x89, 0x40, 0x10});                       // mov [r8+16], eax  (expect 2)
    Emit(c, {0x41, 0x88, 0x58, 0x14});                       // mov [r8+20], bl   (expect 0)
    Emit(c, {0xF4});                                         // hlt
    std::memcpy(reinterpret_cast<void*>(code), c.data(), c.size());
    space->NotifyContentsReplaced(code, code + c.size());

    NullSink sink;
    auto thread = engine->StartThread(code, stack + 0x8000, sink, error);
    if (!thread) { std::fprintf(stderr, "thread: %s\n", error.c_str()); return 1; }
    const RunResult r = thread->Run();
    const auto* d = reinterpret_cast<const uint32_t*>(data);
    std::printf("outcome=%d message=%s\n", static_cast<int>(r.outcome), r.message.c_str());
    std::printf("mem after lock cmpxchg: %u (expect 2)\n", d[0]);
    std::printf("eax after success: %u (expect 1) sete: %u (expect 1)\n", d[2], d[3] & 0xFF);
    std::printf("eax after failure: %u (expect 2) sete: %u (expect 0)\n", d[4], d[5] & 0xFF);
    return 0;
}
