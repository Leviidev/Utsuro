#include "nt/xg.h"

#include "engine/log.h"
#include "nt/pe.h"

#include <chrono>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

namespace utsuro::nt::xg {
namespace {

uint64_t g_base = 0;
std::mutex g_mutex;

// The job block, in guest memory. The helper thread's loop reads it; offsets are what the stub below encodes.
struct Job {
    uint64_t fn;            // +0x00
    uint64_t args[8];       // +0x08 .. +0x47
    uint64_t result;        // +0x48
    volatile uint32_t done; // +0x50
    uint32_t pad0;
    uint64_t pad1[3];       // up to +0x70
    uint64_t pad2[2];       // up to +0x7f
    uint64_t request;       // +0x80: handle of the event that wakes the thread
    uint64_t pad3;          // +0x88
    uint64_t wait;          // +0x90: ntdll's NtWaitForSingleObject
};
static_assert(offsetof(Job, result) == 0x48 && offsetof(Job, done) == 0x50 && offsetof(Job, request) == 0x80 && offsetof(Job, wait) == 0x90, "job layout");

Job* g_job = nullptr;
std::shared_ptr<EventObject> g_request;
bool g_failed = false;

bool StartHelper(Kernel& k) {
    if (g_job != nullptr) return true;
    if (g_failed || g_base == 0) return false;
    uint64_t job_at = 0, job_size = 0x1000;
    if (k.Vm().Allocate(&job_at, &job_size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE, RegionKind::Private, "xg job") != STATUS_SUCCESS) { g_failed = true; return false; }
    uint64_t code_at = 0, code_size = 0x1000;
    if (k.Vm().Allocate(&code_at, &code_size, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE, RegionKind::Private, "xg helper") != STATUS_SUCCESS) { g_failed = true; return false; }
    auto* job = reinterpret_cast<Job*>(job_at);
    std::memset(job, 0, sizeof *job);
    g_request = std::make_shared<EventObject>(false, false);
    job->request = reinterpret_cast<uint64_t>(k.AddHandle(g_request));
    job->wait = FindExport(k.NtdllBase(), "NtWaitForSingleObject");
    if (job->wait == 0) { g_failed = true; return false; }

    auto* p = reinterpret_cast<uint8_t*>(code_at);
    size_t n = 0;
    auto put = [&](std::initializer_list<uint8_t> bytes) { for (uint8_t b : bytes) p[n++] = b; };
    put({0x53, 0x56});                                  // push rbx; push rsi
    put({0x48, 0x83, 0xEC, 0x58});                      // sub rsp, 0x58
    put({0x48, 0x89, 0xCB});                            // mov rbx, rcx           the job
    const size_t loop = n;
    put({0x48, 0x8B, 0x8B, 0x80, 0x00, 0x00, 0x00});    // mov rcx, [rbx+0x80]    the event
    put({0x31, 0xD2});                                  // xor edx, edx
    put({0x45, 0x31, 0xC0});                            // xor r8d, r8d
    put({0x48, 0x8B, 0x83, 0x90, 0x00, 0x00, 0x00});    // mov rax, [rbx+0x90]    NtWaitForSingleObject
    put({0xFF, 0xD0});                                  // call rax
    put({0x48, 0x8B, 0x03});                            // mov rax, [rbx]         the function
    put({0x48, 0x8B, 0x4B, 0x08});                      // mov rcx, [rbx+8]
    put({0x48, 0x8B, 0x53, 0x10});                      // mov rdx, [rbx+0x10]
    put({0x4C, 0x8B, 0x43, 0x18});                      // mov r8, [rbx+0x18]
    put({0x4C, 0x8B, 0x4B, 0x20});                      // mov r9, [rbx+0x20]
    for (uint8_t i = 0; i < 4; ++i) {
        put({0x48, 0x8B, 0x73, static_cast<uint8_t>(0x28 + 8 * i)});          // mov rsi, [rbx+0x28+8i]
        put({0x48, 0x89, 0x74, 0x24, static_cast<uint8_t>(0x20 + 8 * i)});    // mov [rsp+0x20+8i], rsi
    }
    put({0xFF, 0xD0});                                  // call rax
    put({0x48, 0x89, 0x43, 0x48});                      // mov [rbx+0x48], rax    the result
    put({0xC7, 0x43, 0x50, 0x01, 0x00, 0x00, 0x00});    // mov dword [rbx+0x50], 1
    p[n++] = 0xE9;                                      // jmp loop
    const int32_t back = static_cast<int32_t>(static_cast<int64_t>(loop) - static_cast<int64_t>(n + 4));
    std::memcpy(p + n, &back, 4); n += 4;

    std::string error;
    auto thread = k.NewThread(0x100000, 0x10000, error);
    if (!thread) { UT_ERROR("xg: no thread for the helper: %s", error.c_str()); g_failed = true; return false; }
    k.StartThread(thread, FindExport(k.NtdllBase(), "RtlUserThreadStart"), code_at, job_at, false);
    g_job = job;
    UT_INFO("xg: helper thread %llu started (xg_x.dll at %#llx)", (unsigned long long)thread->tid_, (unsigned long long)g_base);
    return true;
}

} // namespace

void NoteImage(const std::string& image, uint64_t base) {
    if (image == "xg_x.dll") g_base = base;
}

uint64_t Export(const char* name) { return g_base != 0 ? FindExport(g_base, name) : 0; }

bool InImage(uint64_t address) {
    if (g_base == 0) return false;
    const uint32_t nt = *reinterpret_cast<const uint32_t*>(g_base + 0x3c);
    const uint32_t size = *reinterpret_cast<const uint32_t*>(g_base + nt + 0x18 + 0x38);   // OptionalHeader.SizeOfImage
    return address >= g_base && address < g_base + size;
}

uint64_t Call(Kernel& k, uint64_t fn, std::initializer_list<uint64_t> args) { return Call(k, fn, std::vector<uint64_t>(args)); }

uint64_t Call(Kernel& k, uint64_t fn, const std::vector<uint64_t>& args) {
    std::scoped_lock lock {g_mutex};
    if (!StartHelper(k) || fn == 0) return ~0ull;
    g_job->fn = fn;
    size_t i = 0;
    for (uint64_t a : args) if (i < 8) g_job->args[i++] = a;
    for (; i < 8; ++i) g_job->args[i] = 0;
    g_job->done = 0;
    {
        std::scoped_lock wait {k.WaitMutex()};
        g_request->signaled = true;
        k.NotifyAll();
    }
    const auto start = std::chrono::steady_clock::now();
    while (g_job->done == 0) {
        std::this_thread::sleep_for(std::chrono::microseconds(10));
        if (std::chrono::steady_clock::now() - start > std::chrono::seconds(20)) { UT_ERROR("xg: the helper did not return from %#llx", (unsigned long long)fn); return ~0ull; }
    }
    return g_job->result;
}

} // namespace utsuro::nt::xg
