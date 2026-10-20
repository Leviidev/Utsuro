// sys_sync.cpp -- events, semaphores, mutants, timers, waits, alerts, keyed events and
// I/O completion. Every waitable's state is guarded by one process-wide mutex and one
// condition variable; with a few dozen guest threads that is cheaper than it sounds and
// far easier to get right than a lock per object.
#include "nt/kernel.h"

#include "engine/log.h"

#include <sched.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <atomic>
#include <cstring>
#include <thread>

namespace utsuro::nt {

namespace {

using Clock = std::chrono::steady_clock;

struct Deadline {
    bool infinite {true};
    Clock::time_point when {};
};

Deadline MakeDeadline(const int64_t* timeout) {
    Deadline d;
    if (timeout == nullptr) return d;
    d.infinite = false;
    const int64_t v = *timeout;
    if (v < 0) {
        d.when = Clock::now() + std::chrono::nanoseconds(-v * 100);
    } else if (v == 0) {
        d.when = Clock::now();
    } else {
        const int64_t rel = v - Kernel::NtSystemTime();
        d.when = Clock::now() + std::chrono::nanoseconds(rel > 0 ? rel * 100 : 0);
    }
    return d;
}

/// Tries to take `o` for `thread`, under the wait mutex. Returns NTSTATUS-success codes:
/// STATUS_WAIT_0 on success, STATUS_ABANDONED_WAIT_0 for an abandoned mutant, or
/// STATUS_TIMEOUT when it is not available.
NTSTATUS TryAcquire(Object& o, NtThread& thread) {
    switch (o.type) {
    case ObjType::Event: {
        auto& e = static_cast<EventObject&>(o);
        if (!e.signaled) return STATUS_TIMEOUT;
        if (!e.manual_reset) e.signaled = false;
        return STATUS_WAIT_0;
    }
    case ObjType::Semaphore: {
        auto& s = static_cast<SemaphoreObject&>(o);
        if (s.count <= 0) return STATUS_TIMEOUT;
        --s.count;
        s.signaled = s.count > 0;
        return STATUS_WAIT_0;
    }
    case ObjType::Mutant: {
        auto& m = static_cast<MutantObject&>(o);
        if (m.owner_tid == thread.tid_) { ++m.recursion; return STATUS_WAIT_0; }
        if (m.owner_tid != 0) return STATUS_TIMEOUT;
        m.owner_tid = thread.tid_;
        m.recursion = 1;
        m.signaled = false;
        if (m.abandoned) { m.abandoned = false; return STATUS_ABANDONED_WAIT_0; }
        return STATUS_WAIT_0;
    }
    case ObjType::Timer: {
        auto& t = static_cast<TimerObject&>(o);
        if (!t.signaled) return STATUS_TIMEOUT;
        if (!t.manual_reset) t.signaled = false;
        return STATUS_WAIT_0;
    }
    case ObjType::Thread:
        return o.signaled ? STATUS_WAIT_0 : STATUS_TIMEOUT;
    case ObjType::File:
        return STATUS_WAIT_0;  // I/O completes before the call returns
    case ObjType::IoCompletion:
        return static_cast<IoCompletionObject&>(o).queue.empty() ? STATUS_TIMEOUT : STATUS_WAIT_0;
    default:
        return STATUS_TIMEOUT;
    }
}

bool HasPendingApc(NtThread& t) { return !t.apcs.empty(); }

/// The common wait loop. `lock` is held on entry and exit. Returns the wait status.
NTSTATUS WaitLocked(Kernel& k, NtThread& thread, std::unique_lock<std::mutex>& lock,
                    std::vector<std::shared_ptr<Object>>& objects, bool wait_all, bool alertable, const Deadline& dl) {
    for (;;) {
        // Satisfied?
        if (wait_all) {
            bool all = true;
            for (auto& o : objects) {
                if (!o) continue;
                // Probe without consuming: only mutex ownership and signaled state matter.
                bool ok = false;
                switch (o->type) {
                case ObjType::Event: ok = o->signaled; break;
                case ObjType::Timer: ok = o->signaled; break;
                case ObjType::Semaphore: ok = static_cast<SemaphoreObject&>(*o).count > 0; break;
                case ObjType::Mutant: ok = static_cast<MutantObject&>(*o).owner_tid == 0 || static_cast<MutantObject&>(*o).owner_tid == thread.tid_; break;
                case ObjType::Thread: ok = o->signaled; break;
                case ObjType::File: ok = true; break;
                default: ok = false; break;
                }
                if (!ok) { all = false; break; }
            }
            if (all) {
                NTSTATUS result = STATUS_WAIT_0;
                for (auto& o : objects) {
                    if (!o) continue;
                    const NTSTATUS r = TryAcquire(*o, thread);
                    if (r == STATUS_ABANDONED_WAIT_0) result = STATUS_ABANDONED_WAIT_0;
                }
                return result;
            }
        } else {
            for (size_t i = 0; i < objects.size(); ++i) {
                if (!objects[i]) continue;
                const NTSTATUS r = TryAcquire(*objects[i], thread);
                if (r == STATUS_WAIT_0 || r == STATUS_ABANDONED_WAIT_0) return r + static_cast<NTSTATUS>(i);
            }
        }
        if (alertable && (HasPendingApc(thread) || thread.alert_pending)) {
            if (thread.alert_pending && !HasPendingApc(thread)) {
                thread.alert_pending = false;
                return 0x101;  // STATUS_ALERTED
            }
            return STATUS_USER_APC;
        }
        if (k.exiting) return STATUS_THREAD_IS_TERMINATING;
        if (dl.infinite) {
            k.WaitCv().wait(lock);
        } else {
            if (Clock::now() >= dl.when) return STATUS_TIMEOUT;
            k.WaitCv().wait_until(lock, dl.when);
        }
    }
}

NTSTATUS FinishWait(Kernel& k, NtThread& thread, NTSTATUS status, std::unique_lock<std::mutex>& lock) {
    if (status == STATUS_USER_APC) {
        lock.unlock();
        k.DeliverUserApc(thread);
        return STATUS_USER_APC;
    }
    return status;
}

// NtWaitForSingleObject(Handle, Alertable, PLARGE_INTEGER Timeout)
NTSTATUS NtWaitForSingleObject(Kernel& k, NtThread& t, SyscallArgs& a) {
    auto o = k.GetObject(a[0]);
    if (!o) return STATUS_INVALID_HANDLE;
    const bool alertable = (a[1] & 0xFF) != 0;
    const Deadline dl = MakeDeadline(a.Ptr<int64_t>(2));
    std::vector<std::shared_ptr<Object>> objs {o};
    std::unique_lock lock {k.WaitMutex()};
    k.CheckTimersLocked();
    const NTSTATUS st = WaitLocked(k, t, lock, objs, false, alertable, dl);
    return FinishWait(k, t, st, lock);
}

// NtWaitForMultipleObjects(Count, Handles*, WaitType, Alertable, Timeout)
NTSTATUS NtWaitForMultipleObjects(Kernel& k, NtThread& t, SyscallArgs& a) {
    const uint32_t count = static_cast<uint32_t>(a[0]);
    const auto* handles = a.Ptr<HANDLE>(1);
    const bool wait_all = (a[2] & 0xFF) == 0;
    const bool alertable = (a[3] & 0xFF) != 0;
    const Deadline dl = MakeDeadline(a.Ptr<int64_t>(4));
    if (count == 0 || count > 64) return STATUS_INVALID_PARAMETER;
    std::vector<std::shared_ptr<Object>> objs;
    for (uint32_t i = 0; i < count; ++i) {
        auto o = k.GetObject(handles[i]);
        if (!o) return STATUS_INVALID_HANDLE;
        objs.push_back(o);
    }
    std::unique_lock lock {k.WaitMutex()};
    const NTSTATUS st = WaitLocked(k, t, lock, objs, wait_all, alertable, dl);
    return FinishWait(k, t, st, lock);
}

// NtSignalAndWaitForSingleObject(Signal, Wait, Alertable, Timeout)
NTSTATUS SignalObject(Kernel& k, NtThread& t, Object& o) {
    switch (o.type) {
    case ObjType::Event: o.signaled = true; break;
    case ObjType::Semaphore: {
        auto& s = static_cast<SemaphoreObject&>(o);
        if (s.count >= s.max) return STATUS_SEMAPHORE_LIMIT_EXCEEDED;
        ++s.count;
        s.signaled = true;
        break;
    }
    case ObjType::Mutant: {
        auto& m = static_cast<MutantObject&>(o);
        if (m.owner_tid != t.tid_) return STATUS_MUTANT_NOT_OWNED;
        if (--m.recursion == 0) { m.owner_tid = 0; m.signaled = true; }
        break;
    }
    default: return STATUS_OBJECT_TYPE_MISMATCH;
    }
    k.NotifyAll();
    return STATUS_SUCCESS;
}

NTSTATUS NtSignalAndWaitForSingleObject(Kernel& k, NtThread& t, SyscallArgs& a) {
    auto s = k.GetObject(a[0]);
    auto w = k.GetObject(a[1]);
    if (!s || !w) return STATUS_INVALID_HANDLE;
    const bool alertable = (a[2] & 0xFF) != 0;
    const Deadline dl = MakeDeadline(a.Ptr<int64_t>(3));
    std::unique_lock lock {k.WaitMutex()};
    const NTSTATUS sr = SignalObject(k, t, *s);
    if (sr != STATUS_SUCCESS) return sr;
    std::vector<std::shared_ptr<Object>> objs {w};
    const NTSTATUS st = WaitLocked(k, t, lock, objs, false, alertable, dl);
    return FinishWait(k, t, st, lock);
}

// NtDelayExecution(Alertable, PLARGE_INTEGER Interval)
NTSTATUS NtDelayExecution(Kernel& k, NtThread& t, SyscallArgs& a) {
    const bool alertable = (a[0] & 0xFF) != 0;
    const Deadline dl = MakeDeadline(a.Ptr<int64_t>(1));
    std::unique_lock lock {k.WaitMutex()};
    std::vector<std::shared_ptr<Object>> none;
    const NTSTATUS st = WaitLocked(k, t, lock, none, false, alertable, dl);
    if (st == STATUS_TIMEOUT) return STATUS_SUCCESS;
    return FinishWait(k, t, st, lock);
}

NTSTATUS NtYieldExecution(Kernel&, NtThread&, SyscallArgs&) {
    sched_yield();
    std::this_thread::sleep_for(std::chrono::microseconds(50));
    return STATUS_SUCCESS;
}

// Events ------------------------------------------------------------------------------

// NtCreateEvent(PHANDLE, Access, POBJECT_ATTRIBUTES, EventType(0=Notification/manual,1=Sync/auto), InitialState)
NTSTATUS NtCreateEvent(Kernel& k, NtThread&, SyscallArgs& a) {
    const bool manual = (a[3] & 0xFF) == 0;
    const bool initial = (a[4] & 0xFF) != 0;
    auto e = std::make_shared<EventObject>(manual, initial);
    *a.Ptr<HANDLE>(0) = k.AddHandle(e);
    return STATUS_SUCCESS;
}

NTSTATUS NtOpenEvent(Kernel&, NtThread&, SyscallArgs&) { return STATUS_OBJECT_NAME_NOT_FOUND; }

// NtSetEvent(Handle, PLONG PreviousState)
NTSTATUS NtSetEvent(Kernel& k, NtThread&, SyscallArgs& a) {
    auto e = k.Get<EventObject>(a[0], ObjType::Event);
    if (!e) return STATUS_INVALID_HANDLE;
    std::scoped_lock lock {k.WaitMutex()};
    if (auto* prev = a.Ptr<int32_t>(1)) *prev = e->signaled ? 1 : 0;
    e->signaled = true;
    k.NotifyAll();
    return STATUS_SUCCESS;
}

NTSTATUS NtSetEventBoostPriority(Kernel& k, NtThread& t, SyscallArgs& a) {
    SyscallArgs b = a;
    b.reg[1] = 0;
    return NtSetEvent(k, t, b);
}

NTSTATUS NtResetEvent(Kernel& k, NtThread&, SyscallArgs& a) {
    auto e = k.Get<EventObject>(a[0], ObjType::Event);
    if (!e) return STATUS_INVALID_HANDLE;
    std::scoped_lock lock {k.WaitMutex()};
    if (auto* prev = a.Ptr<int32_t>(1)) *prev = e->signaled ? 1 : 0;
    e->signaled = false;
    return STATUS_SUCCESS;
}

NTSTATUS NtClearEvent(Kernel& k, NtThread& t, SyscallArgs& a) {
    SyscallArgs b = a;
    b.reg[1] = 0;
    return NtResetEvent(k, t, b);
}

NTSTATUS NtPulseEvent(Kernel& k, NtThread&, SyscallArgs& a) {
    auto e = k.Get<EventObject>(a[0], ObjType::Event);
    if (!e) return STATUS_INVALID_HANDLE;
    std::unique_lock lock {k.WaitMutex()};
    e->signaled = true;
    k.NotifyAll();
    // Let the waiters run before it clears again.
    lock.unlock();
    sched_yield();
    lock.lock();
    e->signaled = false;
    return STATUS_SUCCESS;
}

// NtQueryEvent(Handle, Class, Buffer, Length, PULONG Ret): EVENT_BASIC_INFORMATION {Type, State}
NTSTATUS NtQueryEvent(Kernel& k, NtThread&, SyscallArgs& a) {
    auto e = k.Get<EventObject>(a[0], ObjType::Event);
    if (!e) return STATUS_INVALID_HANDLE;
    if (a[3] < 8) return STATUS_INFO_LENGTH_MISMATCH;
    auto* buf = a.Ptr<uint32_t>(2);
    buf[0] = e->manual_reset ? 0 : 1;
    buf[1] = e->signaled ? 1 : 0;
    if (auto* r = a.Ptr<uint32_t>(4)) *r = 8;
    return STATUS_SUCCESS;
}

// Semaphores --------------------------------------------------------------------------

// NtCreateSemaphore(PHANDLE, Access, POBJECT_ATTRIBUTES, LONG Initial, LONG Maximum)
NTSTATUS NtCreateSemaphore(Kernel& k, NtThread&, SyscallArgs& a) {
    const int32_t initial = static_cast<int32_t>(a[3]);
    const int32_t maximum = static_cast<int32_t>(a[4]);
    if (maximum <= 0 || initial < 0 || initial > maximum) return STATUS_INVALID_PARAMETER;
    *a.Ptr<HANDLE>(0) = k.AddHandle(std::make_shared<SemaphoreObject>(initial, maximum));
    return STATUS_SUCCESS;
}

NTSTATUS NtOpenSemaphore(Kernel&, NtThread&, SyscallArgs&) { return STATUS_OBJECT_NAME_NOT_FOUND; }

// NtReleaseSemaphore(Handle, LONG ReleaseCount, PLONG PreviousCount)
NTSTATUS NtReleaseSemaphore(Kernel& k, NtThread&, SyscallArgs& a) {
    auto s = k.Get<SemaphoreObject>(a[0], ObjType::Semaphore);
    if (!s) return STATUS_INVALID_HANDLE;
    const int32_t n = static_cast<int32_t>(a[1]);
    std::scoped_lock lock {k.WaitMutex()};
    if (n <= 0) return STATUS_INVALID_PARAMETER;
    if (s->count > s->max - n) return STATUS_SEMAPHORE_LIMIT_EXCEEDED;
    if (auto* prev = a.Ptr<int32_t>(2)) *prev = s->count;
    s->count += n;
    s->signaled = true;
    k.NotifyAll();
    return STATUS_SUCCESS;
}

NTSTATUS NtQuerySemaphore(Kernel& k, NtThread&, SyscallArgs& a) {
    auto s = k.Get<SemaphoreObject>(a[0], ObjType::Semaphore);
    if (!s) return STATUS_INVALID_HANDLE;
    if (a[3] < 8) return STATUS_INFO_LENGTH_MISMATCH;
    auto* b = a.Ptr<int32_t>(2);
    b[0] = s->count;
    b[1] = s->max;
    if (auto* r = a.Ptr<uint32_t>(4)) *r = 8;
    return STATUS_SUCCESS;
}

// Mutants -----------------------------------------------------------------------------

// NtCreateMutant(PHANDLE, Access, POBJECT_ATTRIBUTES, BOOLEAN InitialOwner)
NTSTATUS NtCreateMutant(Kernel& k, NtThread& t, SyscallArgs& a) {
    auto m = std::make_shared<MutantObject>();
    if ((a[3] & 0xFF) != 0) {
        m->owner_tid = t.tid_;
        m->recursion = 1;
        m->signaled = false;
    }
    *a.Ptr<HANDLE>(0) = k.AddHandle(m);
    return STATUS_SUCCESS;
}

NTSTATUS NtOpenMutant(Kernel&, NtThread&, SyscallArgs&) { return STATUS_OBJECT_NAME_NOT_FOUND; }

NTSTATUS NtReleaseMutant(Kernel& k, NtThread& t, SyscallArgs& a) {
    auto m = k.Get<MutantObject>(a[0], ObjType::Mutant);
    if (!m) return STATUS_INVALID_HANDLE;
    std::scoped_lock lock {k.WaitMutex()};
    if (m->owner_tid != t.tid_) return STATUS_MUTANT_NOT_OWNED;
    if (auto* prev = a.Ptr<int32_t>(1)) *prev = m->recursion - 1;
    if (--m->recursion == 0) { m->owner_tid = 0; m->signaled = true; }
    k.NotifyAll();
    return STATUS_SUCCESS;
}

// Timers ------------------------------------------------------------------------------

// NtCreateTimer(PHANDLE, Access, POBJECT_ATTRIBUTES, TIMER_TYPE(0=Notification,1=Synchronization))
NTSTATUS NtCreateTimer(Kernel& k, NtThread&, SyscallArgs& a) {
    auto t = std::make_shared<TimerObject>((a[3] & 0xFF) == 0);
    k.RegisterTimer(t);
    *a.Ptr<HANDLE>(0) = k.AddHandle(t);
    return STATUS_SUCCESS;
}

NTSTATUS NtOpenTimer(Kernel&, NtThread&, SyscallArgs&) { return STATUS_OBJECT_NAME_NOT_FOUND; }

// NtSetTimer(Handle, PLARGE_INTEGER DueTime, PTIMER_APC_ROUTINE, PVOID Context, BOOLEAN Resume,
//            LONG Period, PBOOLEAN PreviousState)
NTSTATUS NtSetTimer(Kernel& k, NtThread& th, SyscallArgs& a) {
    auto t = k.Get<TimerObject>(a[0], ObjType::Timer);
    if (!t) return STATUS_INVALID_HANDLE;
    const int64_t* due = a.Ptr<int64_t>(1);
    std::scoped_lock lock {k.WaitMutex()};
    if (auto* prev = a.Ptr<uint8_t>(6)) *prev = t->signaled ? 1 : 0;
    t->signaled = false;
    int64_t ns = 0;
    if (due != nullptr) {
        const int64_t v = *due;
        ns = v < 0 ? -v * 100 : std::max<int64_t>(0, (v - Kernel::NtSystemTime()) * 100);
    }
    t->due_ns = Kernel::MonotonicNs() + ns;
    t->period_ns = static_cast<int64_t>(static_cast<int32_t>(a[5])) * 1000000ll;
    t->apc_routine = a[2];
    t->apc_context = a[3];
    t->apc_tid = t->apc_routine != 0 ? th.tid_ : 0;
    k.NotifyAll();
    return STATUS_SUCCESS;
}

// NtSetTimerEx(Handle, Class, Info, Length): class 0 = TimerSetCoalescedTimer
NTSTATUS NtSetTimerEx(Kernel& k, NtThread& th, SyscallArgs& a) {
    auto t = k.Get<TimerObject>(a[0], ObjType::Timer);
    if (!t) return STATUS_INVALID_HANDLE;
    if (static_cast<uint32_t>(a[1]) != 0 || a[3] < 0x30) return STATUS_INVALID_INFO_CLASS;
    // TIMER_SET_COALESCED_TIMER_INFO: DueTime(8), TimerApcRoutine(8), TimerContext(8),
    // CoalescedTimer(8), Period(4), TolerableDelay(4), PreviousState(8)
    const auto* info = a.Ptr<uint8_t>(2);
    const int64_t due = *reinterpret_cast<const int64_t*>(info);
    std::scoped_lock lock {k.WaitMutex()};
    t->signaled = false;
    const int64_t ns = due < 0 ? -due * 100 : std::max<int64_t>(0, (due - Kernel::NtSystemTime()) * 100);
    t->due_ns = Kernel::MonotonicNs() + ns;
    t->apc_routine = *reinterpret_cast<const uint64_t*>(info + 8);
    t->apc_context = *reinterpret_cast<const uint64_t*>(info + 16);
    t->period_ns = static_cast<int64_t>(*reinterpret_cast<const int32_t*>(info + 32)) * 1000000ll;
    t->apc_tid = t->apc_routine != 0 ? th.tid_ : 0;
    k.NotifyAll();
    return STATUS_SUCCESS;
}

NTSTATUS NtCancelTimer(Kernel& k, NtThread&, SyscallArgs& a) {
    auto t = k.Get<TimerObject>(a[0], ObjType::Timer);
    if (!t) return STATUS_INVALID_HANDLE;
    std::scoped_lock lock {k.WaitMutex()};
    if (auto* st = a.Ptr<uint8_t>(1)) *st = t->signaled ? 1 : 0;
    t->due_ns = -1;
    t->period_ns = 0;
    return STATUS_SUCCESS;
}

NTSTATUS NtQueryTimer(Kernel& k, NtThread&, SyscallArgs& a) {
    auto t = k.Get<TimerObject>(a[0], ObjType::Timer);
    if (!t) return STATUS_INVALID_HANDLE;
    if (a[3] < 16) return STATUS_INFO_LENGTH_MISMATCH;
    auto* b = a.Ptr<int64_t>(2);
    std::scoped_lock lock {k.WaitMutex()};
    const int64_t rem = t->due_ns < 0 ? 0 : std::max<int64_t>(0, (t->due_ns - Kernel::MonotonicNs()) / 100);
    b[0] = -rem;
    reinterpret_cast<uint8_t*>(b)[8] = t->signaled ? 1 : 0;
    if (auto* r = a.Ptr<uint32_t>(4)) *r = 16;
    return STATUS_SUCCESS;
}

// Alerts ------------------------------------------------------------------------------

NTSTATUS NtAlertThread(Kernel& k, NtThread&, SyscallArgs& a) {
    auto t = k.FindThreadByHandle(a[0]);
    if (!t) return STATUS_INVALID_HANDLE;
    std::scoped_lock lock {k.WaitMutex()};
    t->alert_pending = true;
    k.NotifyAll();
    return STATUS_SUCCESS;
}

// NtWaitForAlertByThreadId(Address, PLARGE_INTEGER Timeout)
NTSTATUS NtWaitForAlertByThreadId(Kernel& k, NtThread& t, SyscallArgs& a) {
    const Deadline dl = MakeDeadline(a.Ptr<int64_t>(1));
    std::unique_lock lock {k.WaitMutex()};
    for (;;) {
        if (t.alerted_by_id) { t.alerted_by_id = false; return STATUS_SUCCESS; }
        if (k.exiting) return STATUS_THREAD_IS_TERMINATING;
        if (dl.infinite) {
            k.WaitCv().wait(lock);
        } else {
            if (Clock::now() >= dl.when) return STATUS_TIMEOUT;
            k.WaitCv().wait_until(lock, dl.when);
        }
    }
}

// NtAlertThreadByThreadId(ThreadId)
NTSTATUS NtAlertThreadByThreadId(Kernel& k, NtThread&, SyscallArgs& a) {
    auto t = k.FindThread(a[0]);
    if (!t) return STATUS_INVALID_PARAMETER;
    std::scoped_lock lock {k.WaitMutex()};
    t->alerted_by_id = true;
    k.NotifyAll();
    return STATUS_SUCCESS;
}

NTSTATUS NtTestAlert(Kernel& k, NtThread& t, SyscallArgs&) {
    std::unique_lock lock {k.WaitMutex()};
    if (HasPendingApc(t)) {
        lock.unlock();
        k.DeliverUserApc(t);
        return STATUS_USER_APC;
    }
    if (t.alert_pending) { t.alert_pending = false; return 0x101; }
    return STATUS_SUCCESS;
}

// NtQueueApcThread(Thread, ApcRoutine, Arg1, Arg2, Arg3)
NTSTATUS NtQueueApcThread(Kernel& k, NtThread&, SyscallArgs& a) {
    auto t = k.FindThreadByHandle(a[0]);
    if (!t) return STATUS_INVALID_HANDLE;
    k.QueueApc(*t, PendingApc {a[1], a[2], a[3], a[4]});
    return STATUS_SUCCESS;
}

// NtQueueApcThreadEx(Thread, ReserveHandle, ApcRoutine, Arg1, Arg2, Arg3)
NTSTATUS NtQueueApcThreadEx(Kernel& k, NtThread&, SyscallArgs& a) {
    auto t = k.FindThreadByHandle(a[0]);
    if (!t) return STATUS_INVALID_HANDLE;
    k.QueueApc(*t, PendingApc {a[2], a[3], a[4], a[5]});
    return STATUS_SUCCESS;
}

// Keyed events ------------------------------------------------------------------------

struct KeyedEventObject : Object {
    KeyedEventObject() : Object(ObjType::KeyedEvent) {}
    std::map<uint64_t, int> waiters;
    std::map<uint64_t, int> tokens;
};

NTSTATUS NtCreateKeyedEvent(Kernel& k, NtThread&, SyscallArgs& a) {
    *a.Ptr<HANDLE>(0) = k.AddHandle(std::make_shared<KeyedEventObject>());
    return STATUS_SUCCESS;
}

NTSTATUS NtOpenKeyedEvent(Kernel&, NtThread&, SyscallArgs&) { return STATUS_OBJECT_NAME_NOT_FOUND; }

// NtWaitForKeyedEvent(Handle, Key, Alertable, Timeout)
NTSTATUS NtWaitForKeyedEvent(Kernel& k, NtThread&, SyscallArgs& a) {
    auto o = k.GetObject(a[0]);
    if (!o || o->type != ObjType::KeyedEvent) return STATUS_INVALID_HANDLE;
    auto& ke = static_cast<KeyedEventObject&>(*o);
    const uint64_t key = a[1];
    const Deadline dl = MakeDeadline(a.Ptr<int64_t>(3));
    std::unique_lock lock {k.WaitMutex()};
    ++ke.waiters[key];
    k.NotifyAll();
    for (;;) {
        if (ke.tokens[key] > 0) { --ke.tokens[key]; return STATUS_SUCCESS; }
        if (k.exiting) { --ke.waiters[key]; return STATUS_THREAD_IS_TERMINATING; }
        if (dl.infinite) k.WaitCv().wait(lock);
        else {
            if (Clock::now() >= dl.when) { --ke.waiters[key]; return STATUS_TIMEOUT; }
            k.WaitCv().wait_until(lock, dl.when);
        }
    }
}

NTSTATUS NtReleaseKeyedEvent(Kernel& k, NtThread&, SyscallArgs& a) {
    auto o = k.GetObject(a[0]);
    if (!o || o->type != ObjType::KeyedEvent) return STATUS_INVALID_HANDLE;
    auto& ke = static_cast<KeyedEventObject&>(*o);
    const uint64_t key = a[1];
    const Deadline dl = MakeDeadline(a.Ptr<int64_t>(3));
    std::unique_lock lock {k.WaitMutex()};
    for (;;) {
        if (ke.waiters[key] > 0) {
            --ke.waiters[key];
            ++ke.tokens[key];
            k.NotifyAll();
            return STATUS_SUCCESS;
        }
        if (k.exiting) return STATUS_THREAD_IS_TERMINATING;
        if (dl.infinite) k.WaitCv().wait(lock);
        else {
            if (Clock::now() >= dl.when) return STATUS_TIMEOUT;
            k.WaitCv().wait_until(lock, dl.when);
        }
    }
}

// I/O completion ----------------------------------------------------------------------

NTSTATUS NtCreateIoCompletion(Kernel& k, NtThread&, SyscallArgs& a) {
    *a.Ptr<HANDLE>(0) = k.AddHandle(std::make_shared<IoCompletionObject>());
    return STATUS_SUCCESS;
}

NTSTATUS NtOpenIoCompletion(Kernel&, NtThread&, SyscallArgs&) { return STATUS_OBJECT_NAME_NOT_FOUND; }

// NtSetIoCompletion(Handle, Key, ApcContext, Status, Information)
NTSTATUS NtSetIoCompletion(Kernel& k, NtThread&, SyscallArgs& a) {
    auto c = k.Get<IoCompletionObject>(a[0], ObjType::IoCompletion);
    if (!c) return STATUS_INVALID_HANDLE;
    std::unique_lock lock {k.WaitMutex()};
    c->queue.push_back({a[1], a[2], static_cast<int32_t>(a[3]), a[4]});
    c->signaled = true;
    k.NotifyAll();
    lock.unlock();
    WorkerFactoryPacketQueued(k, c);
    return STATUS_SUCCESS;
}

NTSTATUS NtSetIoCompletionEx(Kernel& k, NtThread&, SyscallArgs& a) {
    // (Port, ReserveHandle, Key, ApcContext, Status, Information)
    auto c = k.Get<IoCompletionObject>(a[0], ObjType::IoCompletion);
    if (!c) return STATUS_INVALID_HANDLE;
    std::unique_lock lock {k.WaitMutex()};
    c->queue.push_back({a[2], a[3], static_cast<int32_t>(a[4]), a[5]});
    c->signaled = true;
    k.NotifyAll();
    lock.unlock();
    WorkerFactoryPacketQueued(k, c);
    return STATUS_SUCCESS;
}

// NtRemoveIoCompletion(Handle, PVOID* Key, PVOID* ApcContext, PIO_STATUS_BLOCK, PLARGE_INTEGER Timeout)
NTSTATUS NtRemoveIoCompletion(Kernel& k, NtThread&, SyscallArgs& a) {
    auto c = k.Get<IoCompletionObject>(a[0], ObjType::IoCompletion);
    if (!c) return STATUS_INVALID_HANDLE;
    const Deadline dl = MakeDeadline(a.Ptr<int64_t>(4));
    std::unique_lock lock {k.WaitMutex()};
    for (;;) {
        if (!c->queue.empty()) {
            auto p = c->queue.front();
            c->queue.pop_front();
            *a.Ptr<uint64_t>(1) = p.key;
            *a.Ptr<uint64_t>(2) = p.apc_context;
            auto* iosb = a.Ptr<IO_STATUS_BLOCK>(3);
            iosb->Status = static_cast<uint64_t>(static_cast<int64_t>(p.status));
            iosb->Information = p.information;
            return STATUS_SUCCESS;
        }
        if (k.exiting) return STATUS_THREAD_IS_TERMINATING;
        if (dl.infinite) k.WaitCv().wait(lock);
        else {
            if (Clock::now() >= dl.when) return STATUS_TIMEOUT;
            k.WaitCv().wait_until(lock, dl.when);
        }
    }
}

// NtRemoveIoCompletionEx(Handle, FILE_IO_COMPLETION_INFORMATION*, Count, PULONG Removed, Timeout, Alertable)
NTSTATUS NtRemoveIoCompletionEx(Kernel& k, NtThread& t, SyscallArgs& a) {
    auto c = k.Get<IoCompletionObject>(a[0], ObjType::IoCompletion);
    if (!c) return STATUS_INVALID_HANDLE;
    auto* out = a.Ptr<uint8_t>(1);
    const uint32_t max = static_cast<uint32_t>(a[2]);
    auto* removed = a.Ptr<uint32_t>(3);
    const Deadline dl = MakeDeadline(a.Ptr<int64_t>(4));
    const bool alertable = (a[5] & 0xFF) != 0;
    std::unique_lock lock {k.WaitMutex()};
    for (;;) {
        uint32_t n = 0;
        while (n < max && !c->queue.empty()) {
            auto p = c->queue.front();
            c->queue.pop_front();
            auto* e = out + n * 32;
            *reinterpret_cast<uint64_t*>(e) = p.key;
            *reinterpret_cast<uint64_t*>(e + 8) = p.apc_context;
            *reinterpret_cast<uint64_t*>(e + 16) = static_cast<uint64_t>(static_cast<int64_t>(p.status));
            *reinterpret_cast<uint64_t*>(e + 24) = p.information;
            ++n;
        }
        if (n != 0) { *removed = n; return STATUS_SUCCESS; }
        if (alertable && HasPendingApc(t)) { *removed = 0; return FinishWait(k, t, STATUS_USER_APC, lock); }
        if (k.exiting) return STATUS_THREAD_IS_TERMINATING;
        if (dl.infinite) k.WaitCv().wait(lock);
        else {
            if (Clock::now() >= dl.when) { *removed = 0; return STATUS_TIMEOUT; }
            k.WaitCv().wait_until(lock, dl.when);
        }
    }
}

NTSTATUS NtQueryPerformanceCounter(Kernel&, NtThread&, SyscallArgs& a) {
    *a.Ptr<uint64_t>(0) = Kernel::QpcNow();
    if (auto* f = a.Ptr<uint64_t>(1)) *f = 10000000;
    return STATUS_SUCCESS;
}

} // namespace

// ---------------------------------------------------------------------------------------
// Kernel members
// ---------------------------------------------------------------------------------------

void Kernel::RegisterTimer(std::shared_ptr<TimerObject> timer) {
    std::scoped_lock lock {wait_mutex_};
    timers_.push_back(std::move(timer));
    if (!timer_thread_.joinable()) timer_thread_ = std::thread([this] { TimerThreadMain(); });
}

void Kernel::CheckTimersLocked() {
    const int64_t now = MonotonicNs();
    for (auto& t : timers_) {
        if (t->due_ns < 0 || t->due_ns > now) continue;
        t->signaled = true;
        if (t->apc_routine != 0) {
            if (auto th = FindThread(t->apc_tid)) {
                // Timer APC routine: (Context, TimerLow, TimerHigh)
                const int64_t st = NtSystemTime();
                th->apcs.push_back(PendingApc {t->apc_routine, t->apc_context, static_cast<uint64_t>(st) & 0xFFFFFFFF,
                                               static_cast<uint64_t>(st) >> 32});
            }
        }
        if (t->period_ns > 0) {
            t->due_ns += t->period_ns;
            if (t->due_ns <= now) t->due_ns = now + t->period_ns;
        } else {
            t->due_ns = -1;
        }
        wait_cv_.notify_all();
    }
}

void Kernel::TimerThreadMain() {
    pthread_setname_np("utsuro-timers");
    std::unique_lock lock {wait_mutex_};
    while (!exiting) {
        CheckTimersLocked();
        // Wake for the earliest armed timer, or for a change.
        int64_t earliest = -1;
        for (auto& t : timers_) {
            if (t->due_ns >= 0 && (earliest < 0 || t->due_ns < earliest)) earliest = t->due_ns;
        }
        // Keep KUSER_SHARED_DATA's clocks moving as well (about 15 ms resolution).
        if (kusd_ != 0) {
            const int64_t now = NtSystemTime();
            auto* k = reinterpret_cast<uint8_t*>(kusd_);
            const int64_t ms = MonotonicNs() / 1000000;
            *reinterpret_cast<uint32_t*>(k + 0x14) = static_cast<uint32_t>(now & 0xFFFFFFFF);
            *reinterpret_cast<int32_t*>(k + 0x18) = static_cast<int32_t>(now >> 32);
            *reinterpret_cast<int32_t*>(k + 0x1c) = static_cast<int32_t>(now >> 32);
            const uint64_t ticks = static_cast<uint64_t>(ms) * 10000;
            *reinterpret_cast<uint32_t*>(k + 0x8) = static_cast<uint32_t>(ticks & 0xFFFFFFFF);
            *reinterpret_cast<int32_t*>(k + 0xc) = static_cast<int32_t>(ticks >> 32);
            *reinterpret_cast<int32_t*>(k + 0x10) = static_cast<int32_t>(ticks >> 32);
            // TickCount counts 15.625 ms ticks (TickCountMultiplier is 15.625 in 8.24 fixed point): GetTickCount64 multiplies it back.
            *reinterpret_cast<uint32_t*>(k + 0x320) = static_cast<uint32_t>(ms * 64 / 1000);
        }
        const int64_t now_ns = MonotonicNs();
        int64_t wait_ns = 15000000;
        if (earliest >= 0) wait_ns = std::min<int64_t>(wait_ns, std::max<int64_t>(earliest - now_ns, 100000));
        wait_cv_.wait_for(lock, std::chrono::nanoseconds(wait_ns));
    }
}

void Kernel::QueueApc(NtThread& target, const PendingApc& apc) {
    std::scoped_lock lock {wait_mutex_};
    target.apcs.push_back(apc);
    wait_cv_.notify_all();
}

bool Kernel::DeliverUserApc(NtThread& thread) {
    PendingApc apc;
    {
        std::scoped_lock lock {wait_mutex_};
        if (thread.apcs.empty()) return false;
        apc = thread.apcs.front();
        thread.apcs.pop_front();
    }
    GuestThreadControl* ctl = thread.control;
    GuestCpuState st;
    if (ctl == nullptr || !ctl->CaptureCpuState(&st)) return false;
    // Save the interrupted context on the thread's stack, with the APC parameters in the
    // CONTEXT's home area where KiUserApcDispatcher looks for them.
    const uint64_t sp = (st.gregs[4] - 0x100 - sizeof(CONTEXT)) & ~0xFull;
    auto* ctx = reinterpret_cast<CONTEXT*>(sp);
    std::memset(ctx, 0, sizeof(CONTEXT));
    ctx->ContextFlags = CONTEXT_FULL;
    ctx->MxCsr = st.mxcsr;
    ctx->SegCs = 0x33; ctx->SegSs = 0x2b;
    ctx->EFlags = st.eflags;
    uint64_t* r = &ctx->Rax;
    // x86 encoding order matches CONTEXT's: rax, rcx, rdx, rbx, rsp, rbp, rsi, rdi, r8-r15.
    for (int i = 0; i < 16; ++i) r[i] = st.gregs[i];
    ctx->Rax = static_cast<uint64_t>(STATUS_USER_APC);
    ctx->Rip = st.rip;
    ctx->ControlWord = 0x27f;
    ctx->MxCsr2 = st.mxcsr;
    for (int i = 0; i < 16; ++i) {
        ctx->XmmRegisters[i].Low = st.xmm[i][0];
        ctx->XmmRegisters[i].High = static_cast<int64_t>(st.xmm[i][1]);
    }
    ctx->P1Home = apc.arg1;
    ctx->P2Home = apc.arg2;
    ctx->P3Home = apc.arg3;
    ctx->P4Home = apc.routine;
    st.gregs[4] = sp;
    st.rip = ki_user_apc_dispatcher;
    ctl->RestoreCpuState(st);
    return true;
}

// Wait completion packets -------------------------------------------------------------
//
// The thread pool's wait objects: a packet ties a waitable object to an I/O completion port; when
// the object is signalled the packet's key/context arrive on the port, once.

namespace {

struct WaitCompletionPacket : Object {
    WaitCompletionPacket() : Object(ObjType::Generic) {}
    int state {0};              ///< 0 idle, 1 waiting for the target, 2 delivered. Guarded by the wait mutex.
    uint64_t generation {0};    ///< Bumped by every association and cancellation; stale watchers stop.
};

std::shared_ptr<WaitCompletionPacket> GetPacket(Kernel& k, HANDLE h) {
    return std::dynamic_pointer_cast<WaitCompletionPacket>(k.GetObject(h));
}

} // namespace

NTSTATUS NtCreateWaitCompletionPacket(Kernel& k, NtThread&, SyscallArgs& a) {
    *a.Ptr<HANDLE>(0) = k.AddHandle(std::make_shared<WaitCompletionPacket>());
    return STATUS_SUCCESS;
}

// NtAssociateWaitCompletionPacket(Packet, IoCompletion, Target, Key, ApcContext, IoStatus, Information, PBOOLEAN AlreadySignaled)
NTSTATUS NtAssociateWaitCompletionPacket(Kernel& k, NtThread&, SyscallArgs& a) {
    auto pkt = GetPacket(k, a[0]);
    auto port = k.Get<IoCompletionObject>(a[1], ObjType::IoCompletion);
    auto target = k.GetObject(a[2]);
    if (!pkt || !port || !target) return STATUS_INVALID_HANDLE;
    const IoCompletionObject::Packet delivery {a[3], a[4], static_cast<int32_t>(a[5]), a[6]};
    uint64_t generation;
    {
        std::scoped_lock lock {k.WaitMutex()};
        if (pkt->state == 1) return STATUS_INVALID_PARAMETER;
        pkt->state = 1;
        generation = ++pkt->generation;
    }
    if (auto* already = a.Ptr<uint8_t>(7)) *already = 0;
    std::thread([&k, pkt, port, target, delivery, generation] {
        NtThread waiter {k, 0};
        std::unique_lock lock {k.WaitMutex()};
        for (;;) {
            if (pkt->generation != generation || pkt->state != 1 || k.exiting) return;
            k.CheckTimersLocked();
            const NTSTATUS r = TryAcquire(*target, waiter);
            if (r == STATUS_WAIT_0 || r == STATUS_ABANDONED_WAIT_0) break;
            k.WaitCv().wait_for(lock, std::chrono::milliseconds(20));
        }
        pkt->state = 2;
        port->queue.push_back(delivery);
        port->signaled = true;
        k.NotifyAll();
        lock.unlock();
        WorkerFactoryPacketQueued(k, port);
    }).detach();
    return STATUS_SUCCESS;
}

// NtCancelWaitCompletionPacket(Packet, BOOLEAN RemoveSignaledPacketFromQueue)
NTSTATUS NtCancelWaitCompletionPacket(Kernel& k, NtThread&, SyscallArgs& a) {
    auto pkt = GetPacket(k, a[0]);
    if (!pkt) return STATUS_INVALID_HANDLE;
    std::scoped_lock lock {k.WaitMutex()};
    if (pkt->state == 1) {
        pkt->state = 0;
        ++pkt->generation;
        k.NotifyAll();
        return STATUS_SUCCESS;
    }
    return pkt->state == 2 ? static_cast<NTSTATUS>(0xC0000120) /* STATUS_NOT_CANCELLED */ : STATUS_SUCCESS;
}

void RegisterSyncSyscalls(std::unordered_map<std::string, SyscallFn>& t) {
    t["NtCreateWaitCompletionPacket"] = NtCreateWaitCompletionPacket;
    t["NtAssociateWaitCompletionPacket"] = NtAssociateWaitCompletionPacket;
    t["NtCancelWaitCompletionPacket"] = NtCancelWaitCompletionPacket;
    t["NtWaitForSingleObject"] = NtWaitForSingleObject;
    t["NtWaitForMultipleObjects"] = NtWaitForMultipleObjects;
    t["NtSignalAndWaitForSingleObject"] = NtSignalAndWaitForSingleObject;
    t["NtDelayExecution"] = NtDelayExecution;
    t["NtYieldExecution"] = NtYieldExecution;
    t["NtCreateEvent"] = NtCreateEvent;
    t["NtOpenEvent"] = NtOpenEvent;
    t["NtSetEvent"] = NtSetEvent;
    t["NtSetEventBoostPriority"] = NtSetEventBoostPriority;
    t["NtResetEvent"] = NtResetEvent;
    t["NtClearEvent"] = NtClearEvent;
    t["NtPulseEvent"] = NtPulseEvent;
    t["NtQueryEvent"] = NtQueryEvent;
    t["NtCreateSemaphore"] = NtCreateSemaphore;
    t["NtOpenSemaphore"] = NtOpenSemaphore;
    t["NtReleaseSemaphore"] = NtReleaseSemaphore;
    t["NtQuerySemaphore"] = NtQuerySemaphore;
    t["NtCreateMutant"] = NtCreateMutant;
    t["NtOpenMutant"] = NtOpenMutant;
    t["NtReleaseMutant"] = NtReleaseMutant;
    t["NtCreateTimer"] = NtCreateTimer;
    t["NtOpenTimer"] = NtOpenTimer;
    t["NtSetTimer"] = NtSetTimer;
    t["NtSetTimerEx"] = NtSetTimerEx;
    t["NtCancelTimer"] = NtCancelTimer;
    t["NtQueryTimer"] = NtQueryTimer;
    t["NtAlertThread"] = NtAlertThread;
    t["NtWaitForAlertByThreadId"] = NtWaitForAlertByThreadId;
    t["NtAlertThreadByThreadId"] = NtAlertThreadByThreadId;
    t["NtTestAlert"] = NtTestAlert;
    t["NtQueueApcThread"] = NtQueueApcThread;
    t["NtQueueApcThreadEx"] = NtQueueApcThreadEx;
    t["NtCreateKeyedEvent"] = NtCreateKeyedEvent;
    t["NtOpenKeyedEvent"] = NtOpenKeyedEvent;
    t["NtWaitForKeyedEvent"] = NtWaitForKeyedEvent;
    t["NtReleaseKeyedEvent"] = NtReleaseKeyedEvent;
    t["NtCreateIoCompletion"] = NtCreateIoCompletion;
    t["NtOpenIoCompletion"] = NtOpenIoCompletion;
    t["NtSetIoCompletion"] = NtSetIoCompletion;
    t["NtSetIoCompletionEx"] = NtSetIoCompletionEx;
    t["NtRemoveIoCompletion"] = NtRemoveIoCompletion;
    t["NtRemoveIoCompletionEx"] = NtRemoveIoCompletionEx;
    t["NtQueryPerformanceCounter"] = NtQueryPerformanceCounter;
}

} // namespace utsuro::nt
