// sys_threadpool.cpp -- the worker factory, the kernel half of the Windows 8 thread pool.
// ntdll's pool creates a factory bound to an I/O completion port; the kernel starts worker
// threads (at StartRoutine(StartParameter)) as work arrives and workers take their work with
// NtWaitForWorkViaWorkerFactory instead of NtRemoveIoCompletion.
#include "nt/kernel.h"

#include "engine/log.h"

#include <algorithm>
#include <cstring>

namespace utsuro::nt {

namespace {

enum Info : uint32_t {
    kTimeout, kRetryTimeout, kIdleTimeout, kBindingCount, kThreadMinimum, kThreadMaximum, kPaused,
    kBasicInformation, kAdjustThreadGoal, kCallbackType, kStackInformation, kThreadBasePriority,
    kTimeoutWaiters, kFlags, kThreadSoftMaximum,
};

/// Starts one worker thread; called with no locks held.
void SpawnWorker(Kernel& k, const std::shared_ptr<WorkerFactoryObject>& f) {
    std::string error;
    auto thread = k.NewThread(f->stack_reserve ? f->stack_reserve : 0x100000, f->stack_commit ? f->stack_commit : 0x10000, error);
    if (!thread) {
        UT_ERROR("worker factory: cannot create a thread: %s", error.c_str());
        return;
    }
    k.StartThread(thread, FindExport(k.NtdllBase(), "RtlUserThreadStart"), f->start_routine, f->start_parameter, false);
    UT_DEBUG("worker factory: started worker %llu", (unsigned long long)thread->tid_);
}

/// How many workers the pool may have running at once: it grows with the load but stays bounded.
uint32_t Cap(const WorkerFactoryObject& f) { return std::min<uint32_t>(f.max_threads, 64); }

// NtCreateWorkerFactory(PHANDLE, Access, ObjectAttributes, IoCompletion, Process, StartRoutine,
//                       StartParameter, MaxThreadCount, StackReserve, StackCommit)
NTSTATUS NtCreateWorkerFactory(Kernel& k, NtThread&, SyscallArgs& a) {
    auto port = k.Get<IoCompletionObject>(a[3], ObjType::IoCompletion);
    if (!port) return STATUS_INVALID_HANDLE;
    auto f = std::make_shared<WorkerFactoryObject>();
    f->port = port;
    f->start_routine = a[5];
    f->start_parameter = a[6];
    f->max_threads = static_cast<uint32_t>(a[7]) ? static_cast<uint32_t>(a[7]) : 0x7FFFFFFF;
    f->stack_reserve = a[8];
    f->stack_commit = a[9];
    port->factory = f;
    *a.Ptr<HANDLE>(0) = k.AddHandle(f);
    UT_DEBUG("worker factory: created, start %#llx(%#llx), max %u", (unsigned long long)f->start_routine, (unsigned long long)f->start_parameter, f->max_threads);
    return STATUS_SUCCESS;
}

NTSTATUS NtSetInformationWorkerFactory(Kernel& k, NtThread&, SyscallArgs& a) {
    auto f = k.Get<WorkerFactoryObject>(a[0], ObjType::WorkerFactory);
    if (!f) return STATUS_INVALID_HANDLE;
    const uint32_t cls = static_cast<uint32_t>(a[1]);
    const auto* buf = a.Ptr<uint8_t>(2);
    const uint32_t len = static_cast<uint32_t>(a[3]);
    uint32_t spawn = 0;
    {
        std::scoped_lock lock {k.WaitMutex()};
        auto u32 = [&]() { uint32_t v = 0; if (buf && len >= 4) std::memcpy(&v, buf, 4); return v; };
        auto i64 = [&]() { int64_t v = 0; if (buf && len >= 8) std::memcpy(&v, buf, 8); return v; };
        switch (cls) {
        case kTimeout: f->timeout = i64(); break;
        case kRetryTimeout: f->retry_timeout = i64(); break;
        case kIdleTimeout: f->idle_timeout = i64(); break;
        case kBindingCount: f->binding = u32(); break;
        case kThreadMinimum:
            f->min_threads = u32();
            if (!f->paused && f->total < f->min_threads) { spawn = f->min_threads - f->total; f->total += spawn; }
            break;
        case kThreadMaximum: f->max_threads = u32(); break;
        case kPaused: f->paused = u32() != 0; break;
        default: break;   // goals, priorities, flags: nothing to schedule
        }
    }
    for (uint32_t i = 0; i < spawn; ++i) SpawnWorker(k, f);
    return STATUS_SUCCESS;
}

NTSTATUS NtQueryInformationWorkerFactory(Kernel& k, NtThread&, SyscallArgs& a) {
    auto f = k.Get<WorkerFactoryObject>(a[0], ObjType::WorkerFactory);
    if (!f) return STATUS_INVALID_HANDLE;
    const uint32_t cls = static_cast<uint32_t>(a[1]);
    auto* buf = a.Ptr<uint8_t>(2);
    const uint32_t len = static_cast<uint32_t>(a[3]);
    auto* ret = a.Ptr<uint32_t>(4);
    if (cls != kBasicInformation) {
        if (ret) *ret = 0;
        return STATUS_INVALID_INFO_CLASS;
    }
    if (ret) *ret = 120;
    if (len < 120) return STATUS_INFO_LENGTH_MISMATCH;
    std::scoped_lock lock {k.WaitMutex()};
    std::memset(buf, 0, 120);
    *reinterpret_cast<int64_t*>(buf) = f->timeout;
    *reinterpret_cast<int64_t*>(buf + 8) = f->retry_timeout;
    *reinterpret_cast<int64_t*>(buf + 16) = f->idle_timeout;
    buf[24] = f->paused;
    buf[27] = f->total < Cap(*f);   // MayCreate
    buf[30] = f->shutdown;
    *reinterpret_cast<uint32_t*>(buf + 32) = f->binding;
    *reinterpret_cast<uint32_t*>(buf + 36) = f->min_threads;
    *reinterpret_cast<uint32_t*>(buf + 40) = f->max_threads;
    *reinterpret_cast<uint32_t*>(buf + 48) = f->waiting;
    *reinterpret_cast<uint32_t*>(buf + 52) = f->total;
    *reinterpret_cast<uint64_t*>(buf + 72) = f->start_routine;
    *reinterpret_cast<uint64_t*>(buf + 80) = f->start_parameter;
    *reinterpret_cast<uint64_t*>(buf + 88) = 4;
    *reinterpret_cast<uint64_t*>(buf + 96) = f->stack_reserve;
    *reinterpret_cast<uint64_t*>(buf + 104) = f->stack_commit;
    return STATUS_SUCCESS;
}

NTSTATUS NtWorkerFactoryWorkerReady(Kernel& k, NtThread&, SyscallArgs& a) {
    return k.Get<WorkerFactoryObject>(a[0], ObjType::WorkerFactory) ? STATUS_SUCCESS : STATUS_INVALID_HANDLE;
}

NTSTATUS NtReleaseWorkerFactoryWorker(Kernel& k, NtThread&, SyscallArgs& a) {
    return k.Get<WorkerFactoryObject>(a[0], ObjType::WorkerFactory) ? STATUS_SUCCESS : STATUS_INVALID_HANDLE;
}

NTSTATUS NtShutdownWorkerFactory(Kernel& k, NtThread&, SyscallArgs& a) {
    auto f = k.Get<WorkerFactoryObject>(a[0], ObjType::WorkerFactory);
    if (!f) return STATUS_INVALID_HANDLE;
    std::scoped_lock lock {k.WaitMutex()};
    f->shutdown = true;
    if (auto* pending = a.Ptr<int32_t>(1)) *pending = 0;
    k.NotifyAll();
    return STATUS_SUCCESS;
}

// NtWaitForWorkViaWorkerFactory(Factory, FILE_IO_COMPLETION_INFORMATION* Packets, ULONG Count,
//                               PULONG PacketsReturned, DeferredWork*): the worker's "get next
// work items" -- blocks until the port has a packet, then takes up to Count.
NTSTATUS NtWaitForWorkViaWorkerFactory(Kernel& k, NtThread&, SyscallArgs& a) {
    auto f = k.Get<WorkerFactoryObject>(a[0], ObjType::WorkerFactory);
    if (!f) return STATUS_INVALID_HANDLE;
    auto* out = a.Ptr<uint8_t>(1);
    const uint32_t max = std::max<uint32_t>(1, static_cast<uint32_t>(a[2]));
    auto* returned = a.Ptr<uint32_t>(3);
    std::unique_lock lock {k.WaitMutex()};
    ++f->waiting;
    for (;;) {
        if (!f->port->queue.empty()) {
            uint32_t n = 0;
            while (n < max && !f->port->queue.empty()) {
                auto p = f->port->queue.front();
                f->port->queue.pop_front();
                auto* e = out + n * 32;
                *reinterpret_cast<uint64_t*>(e) = p.key;
                *reinterpret_cast<uint64_t*>(e + 8) = p.apc_context;
                *reinterpret_cast<uint64_t*>(e + 16) = static_cast<uint64_t>(static_cast<int64_t>(p.status));
                *reinterpret_cast<uint64_t*>(e + 24) = p.information;
                ++n;
            }
            --f->waiting;
            if (returned) *returned = n;
            return STATUS_SUCCESS;
        }
        if (f->shutdown || k.exiting) {
            --f->waiting;
            if (returned) *returned = 0;
            return STATUS_THREAD_IS_TERMINATING;
        }
        k.WaitCv().wait(lock);
    }
}

} // namespace

void WorkerFactoryPacketQueued(Kernel& k, const std::shared_ptr<IoCompletionObject>& port) {
    auto f = port->factory.lock();
    if (!f) return;
    {
        std::scoped_lock lock {k.WaitMutex()};
        if (f->paused || f->shutdown || f->waiting > 0 || f->total >= Cap(*f)) return;
        ++f->total;
    }
    SpawnWorker(k, f);
}

void RegisterWorkerFactorySyscalls(std::unordered_map<std::string, SyscallFn>& t) {
    t["NtCreateWorkerFactory"] = NtCreateWorkerFactory;
    t["NtSetInformationWorkerFactory"] = NtSetInformationWorkerFactory;
    t["NtQueryInformationWorkerFactory"] = NtQueryInformationWorkerFactory;
    t["NtWorkerFactoryWorkerReady"] = NtWorkerFactoryWorkerReady;
    t["NtReleaseWorkerFactoryWorker"] = NtReleaseWorkerFactoryWorker;
    t["NtShutdownWorkerFactory"] = NtShutdownWorkerFactory;
    t["NtWaitForWorkViaWorkerFactory"] = NtWaitForWorkViaWorkerFactory;
}

} // namespace utsuro::nt
