// sys_thread.cpp -- threads, contexts, exceptions and process termination.
#include "nt/kernel.h"

#include "engine/log.h"

#include <cstring>

namespace utsuro::nt {

namespace {

bool IsCurrentThreadHandle(HANDLE h) { return h == 0 || h == ~1ull; }

// GuestCpuState <-> CONTEXT ----------------------------------------------------------

void StateToContext(const GuestCpuState& st, CONTEXT* c, uint32_t flags) {
    c->ContextFlags = flags;
    c->SegCs = 0x33;
    c->SegSs = 0x2b;
    c->SegDs = c->SegEs = c->SegGs = 0x2b;
    c->SegFs = 0x53;
    if (flags & CONTEXT_CONTROL) {
        c->Rsp = st.gregs[4];
        c->Rbp = st.gregs[5];
        c->Rip = st.rip;
        c->EFlags = st.eflags;
    }
    if (flags & CONTEXT_INTEGER) {
        c->Rax = st.gregs[0]; c->Rcx = st.gregs[1]; c->Rdx = st.gregs[2]; c->Rbx = st.gregs[3];
        c->Rsi = st.gregs[6]; c->Rdi = st.gregs[7];
        uint64_t* r8 = &c->R8;
        for (int i = 0; i < 8; ++i) r8[i] = st.gregs[8 + i];
    }
    if (flags & CONTEXT_FLOATING_POINT) {
        c->MxCsr = c->MxCsr2 = st.mxcsr;
        c->ControlWord = st.fcw ? st.fcw : 0x27f;
        c->TagWord = st.ftw;
        for (int i = 0; i < 16; ++i) {
            c->XmmRegisters[i].Low = st.xmm[i][0];
            c->XmmRegisters[i].High = static_cast<int64_t>(st.xmm[i][1]);
        }
        for (int i = 0; i < 8; ++i) {
            c->FloatRegisters[i].Low = st.mm[i][0];
            c->FloatRegisters[i].High = static_cast<int64_t>(st.mm[i][1]);
        }
    }
}

void ContextToState(const CONTEXT* c, GuestCpuState* st, uint32_t flags) {
    if (flags & CONTEXT_CONTROL) {
        st->gregs[4] = c->Rsp;
        st->gregs[5] = c->Rbp;
        st->rip = c->Rip;
        st->eflags = c->EFlags;
    }
    if (flags & CONTEXT_INTEGER) {
        st->gregs[0] = c->Rax; st->gregs[1] = c->Rcx; st->gregs[2] = c->Rdx; st->gregs[3] = c->Rbx;
        st->gregs[6] = c->Rsi; st->gregs[7] = c->Rdi;
        const uint64_t* r8 = &c->R8;
        for (int i = 0; i < 8; ++i) st->gregs[8 + i] = r8[i];
    }
    if (flags & CONTEXT_FLOATING_POINT) {
        st->mxcsr = c->MxCsr;
        st->fcw = c->ControlWord;
        st->ftw = c->TagWord;
        for (int i = 0; i < 16; ++i) {
            st->xmm[i][0] = c->XmmRegisters[i].Low;
            st->xmm[i][1] = static_cast<uint64_t>(c->XmmRegisters[i].High);
        }
        for (int i = 0; i < 8; ++i) {
            st->mm[i][0] = c->FloatRegisters[i].Low;
            st->mm[i][1] = static_cast<uint64_t>(c->FloatRegisters[i].High);
        }
    }
}

// NtContinue(Context, TestAlert)
NTSTATUS NtContinue(Kernel& k, NtThread& t, SyscallArgs& a) {
    auto* c = a.Ptr<CONTEXT>(0);
    GuestThreadControl* ctl = t.control;
    GuestCpuState st;
    if (ctl == nullptr || !ctl->CaptureCpuState(&st)) return STATUS_NOT_SUPPORTED;
    uint32_t flags = c->ContextFlags;
    if ((flags & CONTEXT_AMD64) == 0) flags = CONTEXT_FULL;
    ContextToState(c, &st, flags);
    if (flags & CONTEXT_INTEGER) {
        // The register that carries the syscall's own result is overwritten by it; hand
        // back the value the context holds so the thread resumes with that.
    }
    ctl->RestoreCpuState(st);
    if ((a[1] & 0xFF) != 0) {
        // TestAlert: a pending APC runs before the thread resumes.
        std::unique_lock lock {k.WaitMutex()};
        const bool pending = !t.apcs.empty();
        lock.unlock();
        if (pending) k.DeliverUserApc(t);
    }
    t.has_raw_result = true;
    t.raw_result = c->Rax;
    return STATUS_SUCCESS;
}

// NtGetContextThread(Thread, PCONTEXT)
NTSTATUS NtGetContextThread(Kernel&, NtThread& t, SyscallArgs& a) {
    if (!IsCurrentThreadHandle(a[0])) return STATUS_NOT_SUPPORTED;
    auto* c = a.Ptr<CONTEXT>(1);
    GuestCpuState st;
    if (t.control == nullptr || !t.control->CaptureCpuState(&st)) return STATUS_NOT_SUPPORTED;
    StateToContext(st, c, c->ContextFlags);
    return STATUS_SUCCESS;
}

NTSTATUS NtSetContextThread(Kernel&, NtThread& t, SyscallArgs& a) {
    if (!IsCurrentThreadHandle(a[0])) return STATUS_NOT_SUPPORTED;
    auto* c = a.Ptr<CONTEXT>(1);
    GuestCpuState st;
    if (t.control == nullptr || !t.control->CaptureCpuState(&st)) return STATUS_NOT_SUPPORTED;
    ContextToState(c, &st, c->ContextFlags);
    t.control->RestoreCpuState(st);
    return STATUS_SUCCESS;
}

// NtRaiseException(PEXCEPTION_RECORD, PCONTEXT, BOOLEAN FirstChance)
NTSTATUS NtRaiseException(Kernel& k, NtThread& t, SyscallArgs& a) {
    auto* rec = a.Ptr<EXCEPTION_RECORD>(0);
    auto* ctx = a.Ptr<CONTEXT>(1);
    const bool first = (a[2] & 0xFF) != 0;
    if (first) {
        // Build the frame KiUserExceptionDispatcher expects: CONTEXT at rsp, the record at
        // rsp+0x4f0, and run it.
        GuestCpuState st;
        if (t.control == nullptr || !t.control->CaptureCpuState(&st)) return STATUS_NOT_SUPPORTED;
        const uint64_t sp = (ctx->Rsp - 0x200 - 0x4f0 - sizeof(EXCEPTION_RECORD)) & ~0xFull;
        std::memcpy(reinterpret_cast<void*>(sp), ctx, sizeof(CONTEXT));
        std::memcpy(reinterpret_cast<void*>(sp + 0x4f0), rec, sizeof(EXCEPTION_RECORD));
        st.gregs[4] = sp;
        st.rip = k.ki_user_exception_dispatcher;
        t.control->RestoreCpuState(st);
        return STATUS_SUCCESS;
    }
    UT_ERROR("[%llu] unhandled exception %#x at %#llx (flags %#x), rsp %#llx", (unsigned long long)t.tid_, rec->ExceptionCode,
             (unsigned long long)rec->ExceptionAddress, rec->ExceptionFlags, (unsigned long long)ctx->Rsp);
    for (uint32_t i = 0; i < rec->NumberParameters && i < 4; ++i)
        UT_ERROR("  parameter[%u] = %#llx", i, (unsigned long long)rec->ExceptionInformation[i]);
    k.ExitProcess(static_cast<int32_t>(rec->ExceptionCode));
}

// NtCreateThreadEx(PHANDLE, Access, POBJECT_ATTRIBUTES, Process, StartRoutine, Argument,
//                  CreateFlags, ZeroBits, StackSize, MaxStackSize, PS_ATTRIBUTE_LIST*)
NTSTATUS NtCreateThreadEx(Kernel& k, NtThread&, SyscallArgs& a) {
    auto* out = a.Ptr<HANDLE>(0);
    const uint64_t start = a[4];
    const uint64_t arg = a[5];
    const uint32_t flags = static_cast<uint32_t>(a[6]);
    uint64_t commit = a[8];
    uint64_t reserve = a[9];
    if (reserve == 0) reserve = 0x100000;
    if (reserve < commit) reserve = commit;
    std::string error;
    // Guest threads are given their whole stack up front; it costs address space only.
    auto thread = k.NewThread(std::max<uint64_t>(reserve, 0x100000), commit, error);
    if (!thread) {
        UT_ERROR("NtCreateThreadEx: %s", error.c_str());
        return STATUS_NO_MEMORY;
    }
    // Attributes that report back: client id and TEB.
    if (const uint64_t list = a[10]) {
        const uint64_t total = *reinterpret_cast<uint64_t*>(list);
        for (uint64_t off = 8; off + 32 <= total; off += 32) {
            const uint64_t attr = *reinterpret_cast<uint64_t*>(list + off) & 0xFFFFFF;
            const uint64_t size = *reinterpret_cast<uint64_t*>(list + off + 8);
            const uint64_t value = *reinterpret_cast<uint64_t*>(list + off + 16);
            const uint64_t retlen = *reinterpret_cast<uint64_t*>(list + off + 24);
            if (attr == 0x3 && value != 0 && size >= 16) {        // PsAttributeClientId
                reinterpret_cast<CLIENT_ID*>(value)->UniqueProcess = 4;
                reinterpret_cast<CLIENT_ID*>(value)->UniqueThread = thread->tid_;
                if (retlen) *reinterpret_cast<uint64_t*>(retlen) = 16;
            } else if (attr == 0x1 && value != 0) {               // PsAttributeTeb
                *reinterpret_cast<uint64_t*>(value) = thread->teb;
                if (retlen) *reinterpret_cast<uint64_t*>(retlen) = 8;
            }
        }
    }
    *out = k.AddHandle(thread);
    k.StartThread(thread, FindExport(k.NtdllBase(), "RtlUserThreadStart"), start, arg, (flags & 1) != 0);
    return STATUS_SUCCESS;
}

NTSTATUS NtTerminateThread(Kernel& k, NtThread& t, SyscallArgs& a) {
    if (IsCurrentThreadHandle(a[0]) || a[0] == 0 || (k.FindThreadByHandle(a[0]) && k.FindThreadByHandle(a[0]).get() == &t)) {
        t.exit_status = static_cast<int32_t>(a[1]);
        t.control->ExitGuest(static_cast<int>(a[1]));
    }
    auto other = k.FindThreadByHandle(a[0]);
    if (!other) return STATUS_INVALID_HANDLE;
    UT_WARN("NtTerminateThread on another thread (%llu) is not supported", (unsigned long long)other->tid_);
    return STATUS_NOT_SUPPORTED;
}

NTSTATUS NtTerminateProcess(Kernel& k, NtThread&, SyscallArgs& a) {
    if (a[0] == ~0ull || a[0] == 4) k.ExitProcess(static_cast<int32_t>(a[1]));
    // NULL means "all threads but this one", which RtlExitUserProcess issues first.
    return STATUS_SUCCESS;
}

NTSTATUS NtSuspendThread(Kernel& k, NtThread& t, SyscallArgs& a) {
    auto target = k.FindThreadByHandle(a[0]);
    if (!target) return STATUS_INVALID_HANDLE;
    const int32_t prev = target->suspend_count.fetch_add(1);
    if (auto* p = a.Ptr<uint32_t>(1)) *p = static_cast<uint32_t>(prev);
    if (target.get() == &t) {
        std::unique_lock lock {k.WaitMutex()};
        k.WaitCv().wait(lock, [&] { return t.suspend_count.load() <= 0 || k.exiting.load(); });
    } else {
        UT_WARN("suspend of running thread %llu is advisory only", (unsigned long long)target->tid_);
    }
    return STATUS_SUCCESS;
}

NTSTATUS NtResumeThread(Kernel& k, NtThread&, SyscallArgs& a) {
    auto target = k.FindThreadByHandle(a[0]);
    if (!target) return STATUS_INVALID_HANDLE;
    std::scoped_lock lock {k.WaitMutex()};
    const int32_t prev = target->suspend_count.load();
    if (prev > 0) target->suspend_count.fetch_sub(1);
    if (auto* p = a.Ptr<uint32_t>(1)) *p = static_cast<uint32_t>(prev);
    k.NotifyAll();
    return STATUS_SUCCESS;
}

NTSTATUS NtAlertResumeThread(Kernel& k, NtThread& t, SyscallArgs& a) { return NtResumeThread(k, t, a); }

NTSTATUS NtOpenThread(Kernel& k, NtThread&, SyscallArgs& a) {
    const auto* cid = a.Ptr<CLIENT_ID>(3);
    if (cid == nullptr) return STATUS_INVALID_PARAMETER;
    auto t = k.FindThread(cid->UniqueThread);
    if (!t) return STATUS_INVALID_PARAMETER;
    *a.Ptr<HANDLE>(0) = k.AddHandle(t);
    return STATUS_SUCCESS;
}

// NtQueryInformationThread(Thread, Class, Buffer, Length, PULONG Ret)
NTSTATUS NtQueryInformationThread(Kernel& k, NtThread& cur, SyscallArgs& a) {
    auto t = IsCurrentThreadHandle(a[0]) ? k.CurrentThread() : k.FindThreadByHandle(a[0]);
    if (!t) return STATUS_INVALID_HANDLE;
    const uint32_t cls = static_cast<uint32_t>(a[1]);
    auto* buf = a.Ptr<uint8_t>(2);
    const uint32_t len = static_cast<uint32_t>(a[3]);
    auto* ret = a.Ptr<uint32_t>(4);
    switch (cls) {
    case 0:  // ThreadBasicInformation
        if (len < 48) return STATUS_INFO_LENGTH_MISMATCH;
        std::memset(buf, 0, 48);
        *reinterpret_cast<int32_t*>(buf) = t->terminated ? t->exit_status : 0x103;
        *reinterpret_cast<uint64_t*>(buf + 8) = t->teb;
        *reinterpret_cast<uint64_t*>(buf + 16) = 4;
        *reinterpret_cast<uint64_t*>(buf + 24) = t->tid_;
        *reinterpret_cast<uint64_t*>(buf + 32) = (1ull << k.NumberOfProcessors()) - 1;
        *reinterpret_cast<int32_t*>(buf + 40) = 8;
        *reinterpret_cast<int32_t*>(buf + 44) = 8;
        if (ret) *ret = 48;
        return STATUS_SUCCESS;
    case 1:  // ThreadTimes
        if (len < 32) return STATUS_INFO_LENGTH_MISMATCH;
        std::memset(buf, 0, 32);
        if (ret) *ret = 32;
        return STATUS_SUCCESS;
    case 6:  // ThreadAmILastThread
    case 17: // ThreadIsIoPending
        if (len < 4) return STATUS_INFO_LENGTH_MISMATCH;
        *reinterpret_cast<uint32_t*>(buf) = 0;
        if (ret) *ret = 4;
        return STATUS_SUCCESS;
    case 9:  // ThreadQuerySetWin32StartAddress
        if (len < 8) return STATUS_INFO_LENGTH_MISMATCH;
        *reinterpret_cast<uint64_t*>(buf) = t->start_arg;
        if (ret) *ret = 8;
        return STATUS_SUCCESS;
    case 38: // ThreadNameInformation
        if (len < 16) return STATUS_INFO_LENGTH_MISMATCH;
        std::memset(buf, 0, 16);
        if (ret) *ret = 16;
        return STATUS_SUCCESS;
    default:
        UT_DEBUG("NtQueryInformationThread class %u not handled", cls);
        (void)cur;
        return STATUS_INVALID_INFO_CLASS;
    }
}

NTSTATUS NtSetInformationThread(Kernel& k, NtThread&, SyscallArgs& a) {
    const uint32_t cls = static_cast<uint32_t>(a[1]);
    auto t = IsCurrentThreadHandle(a[0]) ? k.CurrentThread() : k.FindThreadByHandle(a[0]);
    if (!t) return STATUS_INVALID_HANDLE;
    if (cls == 38 || cls == 35) {  // ThreadNameInformation
        const auto* us = a.Ptr<UNICODE_STRING>(2);
        if (us != nullptr && a[3] >= sizeof(UNICODE_STRING)) t->name = Utf16ToUtf8(ReadUnicodeString(us));
    }
    return STATUS_SUCCESS;
}

NTSTATUS NtGetCurrentProcessorNumber(Kernel& k, NtThread& t, SyscallArgs&) {
    return static_cast<NTSTATUS>(t.tid_ % k.NumberOfProcessors());
}

NTSTATUS Succeed(Kernel&, NtThread&, SyscallArgs&) { return STATUS_SUCCESS; }

} // namespace

void RegisterThreadSyscalls(std::unordered_map<std::string, SyscallFn>& t) {
    t["NtContinue"] = NtContinue;
    t["NtGetContextThread"] = NtGetContextThread;
    t["NtSetContextThread"] = NtSetContextThread;
    t["NtRaiseException"] = NtRaiseException;
    t["NtCreateThreadEx"] = NtCreateThreadEx;
    t["NtTerminateThread"] = NtTerminateThread;
    t["NtTerminateProcess"] = NtTerminateProcess;
    t["NtSuspendThread"] = NtSuspendThread;
    t["NtResumeThread"] = NtResumeThread;
    t["NtAlertResumeThread"] = NtAlertResumeThread;
    t["NtOpenThread"] = NtOpenThread;
    t["NtQueryInformationThread"] = NtQueryInformationThread;
    t["NtSetInformationThread"] = NtSetInformationThread;
    t["NtGetCurrentProcessorNumber"] = NtGetCurrentProcessorNumber;
    t["NtFlushProcessWriteBuffers"] = Succeed;
    t["NtRegisterThreadTerminatePort"] = Succeed;
}

} // namespace utsuro::nt
