// kernel.h -- the NT kernel half: a process, its handles, its threads, and the syscalls
// the Xbox Era ntdll makes into it.
#pragma once

#include <array>
#include "engine/engine.h"
#include "engine/guest_memory.h"
#include "nt/nt_types.h"
#include "nt/pe.h"
#include "nt/vm.h"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace utsuro::nt {

class Kernel;
class NtThread;

// ---------------------------------------------------------------------------------------
// Arguments of one syscall
// ---------------------------------------------------------------------------------------

/// A syscall's arguments: r10, rdx, r8, r9, then the stack (return address and the
/// 32-byte shadow area first).
struct SyscallArgs {
    uint64_t reg[4] {};
    uint64_t rsp {};
    uint64_t operator[](int i) const {
        return i < 4 ? reg[i] : *reinterpret_cast<const uint64_t*>(rsp + 0x28 + static_cast<uint64_t>(i - 4) * 8);
    }
    template <typename T>
    T* Ptr(int i) const { return reinterpret_cast<T*>((*this)[i]); }
};

using SyscallFn = NTSTATUS (*)(Kernel&, NtThread&, SyscallArgs&);

// ---------------------------------------------------------------------------------------
// Objects
// ---------------------------------------------------------------------------------------

enum class ObjType {
    File, Section, Event, Semaphore, Mutant, Thread, Process, Key, Timer, IoCompletion,
    Directory, SymbolicLink, Token, KeyedEvent, Job, WorkerFactory, Generic
};

struct Object : std::enable_shared_from_this<Object> {
    explicit Object(ObjType t) : type {t} {}
    virtual ~Object() = default;
    ObjType type;
    std::string name;
    /// Waitable state, guarded by Kernel::wait_mutex_.
    bool signaled {false};
    virtual bool IsWaitable() const { return false; }
};

struct EventObject : Object {
    EventObject(bool manual, bool initial) : Object(ObjType::Event), manual_reset {manual} { signaled = initial; }
    bool IsWaitable() const override { return true; }
    bool manual_reset;
};

struct SemaphoreObject : Object {
    SemaphoreObject(int32_t initial, int32_t maximum) : Object(ObjType::Semaphore), count {initial}, max {maximum} {
        signaled = initial > 0;
    }
    bool IsWaitable() const override { return true; }
    int32_t count;
    int32_t max;
};

struct MutantObject : Object {
    MutantObject() : Object(ObjType::Mutant) { signaled = true; }
    bool IsWaitable() const override { return true; }
    uint64_t owner_tid {0};
    int32_t recursion {0};
    bool abandoned {false};
};

struct TimerObject : Object {
    TimerObject(bool manual) : Object(ObjType::Timer), manual_reset {manual} {}
    bool IsWaitable() const override { return true; }
    bool manual_reset;
    int64_t due_ns {-1};  ///< Monotonic ns, or -1 when not armed.
    int64_t period_ns {0};
    uint64_t apc_routine {0};
    uint64_t apc_context {0};
    uint64_t apc_tid {0};
};

struct WorkerFactoryObject;

struct IoCompletionObject : Object {
    IoCompletionObject() : Object(ObjType::IoCompletion) {}
    bool IsWaitable() const override { return true; }
    struct Packet { uint64_t key, apc_context; int32_t status; uint64_t information; };
    std::deque<Packet> queue;
    std::weak_ptr<WorkerFactoryObject> factory;   ///< The thread pool serving this port, if any.
};

/// The Windows 8 thread pool's kernel half: worker threads started on demand that take their
/// work from an I/O completion port.
struct WorkerFactoryObject : Object {
    WorkerFactoryObject() : Object(ObjType::WorkerFactory) {}
    std::shared_ptr<IoCompletionObject> port;
    uint64_t start_routine {0}, start_parameter {0};
    uint64_t stack_reserve {0}, stack_commit {0};
    uint32_t min_threads {0}, max_threads {0x7FFFFFFF};
    uint32_t total {0}, waiting {0}, binding {0};
    int64_t timeout {0}, retry_timeout {0}, idle_timeout {0};
    bool paused {false}, shutdown {false};
};

struct FileObject : Object {
    FileObject() : Object(ObjType::File) {}
    ~FileObject() override;
    int fd {-1};
    std::string host_path;
    std::string nt_path;
    bool is_dir {false};
    bool is_device {false};  ///< An NT device with no host file behind it, e.g. \Device\CNG.
    bool is_logan {false};   ///< The console's audio processor device (nt/logan.cpp).
    bool synchronous {true};
    uint32_t access {0};
    uint32_t options {0};
    bool delete_on_close {false};
    int64_t position {0};
    /// Directory enumeration state.
    std::vector<std::string> dir_entries;
    size_t dir_index {0};
    bool dir_loaded {false};
    std::string dir_pattern;
    std::mutex mutex;
};

struct SectionObject : Object {
    SectionObject() : Object(ObjType::Section) {}
    std::shared_ptr<FileObject> file;
    std::unique_ptr<PeImage> image;  ///< Non-null for SEC_IMAGE.
    uint64_t size {0};
    uint32_t protection {0};
    uint32_t attributes {0};
    uint64_t last_base {0};  ///< Where the image was last mapped, for relocation queries.
    /// For data sections not backed by a file: host-side memory shared by all views.
    std::vector<uint8_t> anon;
};

struct KeyObject : Object {
    KeyObject() : Object(ObjType::Key) {}
    std::string path;
};

struct DirectoryObject : Object {
    DirectoryObject() : Object(ObjType::Directory) {}
};

struct SymbolicLinkObject : Object {
    SymbolicLinkObject() : Object(ObjType::SymbolicLink) {}
    std::string target;
};

struct ThreadObjectBase;

// ---------------------------------------------------------------------------------------
// A thread
// ---------------------------------------------------------------------------------------

struct PendingApc {
    uint64_t routine, arg1, arg2, arg3;
};

/// What a host continuation decides after a guest call it made has returned.
struct GuestStep {
    bool call {false};
    uint64_t fn {0};
    uint64_t args[4] {};
    uint64_t result {0};
    std::function<GuestStep(NtThread&, uint64_t)> cont;

    static GuestStep Return(uint64_t result) { GuestStep s; s.result = result; return s; }
    static GuestStep Call(uint64_t fn, std::initializer_list<uint64_t> a, std::function<GuestStep(NtThread&, uint64_t)> cont = nullptr) {
        GuestStep s;
        s.call = true;
        s.fn = fn;
        size_t i = 0;
        for (uint64_t v : a) if (i < 4) s.args[i++] = v;
        s.cont = std::move(cont);
        return s;
    }
};

struct GuestFrame {
    GuestCpuState saved;   ///< Where to resume the code that made the host call.
    std::function<GuestStep(NtThread&, uint64_t)> cont;
};

class NtThread : public Object, public SyscallSink {
public:
    NtThread(Kernel& kernel, uint64_t tid) : Object(ObjType::Thread), kernel_ {kernel}, tid_ {tid} {}
    bool IsWaitable() const override { return true; }

    // SyscallSink
    uint64_t Handle(uint64_t number, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5,
                    uint64_t a6) override;
    int Pid() const override { return 1; }
    int Tid() const override { return static_cast<int>(tid_); }
    GuestAddressSpace* Space() const override;
    bool StopRequested() const override;

    Kernel& kernel_;
    uint64_t tid_;
    uint64_t teb {0};
    uint64_t stack_base {0};
    uint64_t stack_limit {0};
    uint64_t stack_alloc_base {0};
    uint64_t start_rip {0};
    uint64_t start_arg {0};
    std::unique_ptr<GuestThread> guest;
    std::thread host;
    std::atomic<int32_t> suspend_count {0};
    /// What the game asked for with ThreadBasePriority (8 is normal), applied to the host thread as a QoS class by that thread's own next syscall.
    std::atomic<int32_t> base_priority {8};
    std::atomic<bool> priority_dirty {false};
    /// Gives the host thread the scheduling class its base priority asks for. Only the thread itself can do this on Darwin.
    void ApplyHostPriority();
    int32_t exit_status {0};
    bool terminated {false};
    bool create_suspended {false};
    std::deque<PendingApc> apcs;   ///< Guarded by Kernel::WaitMutex().
    bool alert_pending {false};    ///< NtAlertThread / NtAlertThreadByThreadId, same lock.
    bool alerted_by_id {false};
    std::string name;
    /// The register file at the moment a syscall was made, when something needs it.
    SyscallArgs* current_args {nullptr};
    uint64_t current_syscall {0};
    uint64_t current_rip {0};
    uint64_t current_ret {0};
    /// The last system calls the thread made (number, return address), newest last; UTSURO_HIST prints them when the thread ends.
    std::array<std::pair<uint32_t, uint64_t>, 24> history {};
    size_t history_count {0};
    uint64_t stack_regs[16] {};   ///< General registers at the last system call, kept only when UTSURO_STACKS is set.
    uint64_t stack_rip {0};
    /// Host code that needs the guest to run a function (a COM method, a callback) asks here; the
    /// call is made when the host method returns, and the continuation sees its result.
    void CallGuest(const GuestStep& step) { pending_step = step; has_pending_step = true; }
    std::vector<GuestFrame> guest_frames;
    GuestStep pending_step;
    bool has_pending_step {false};
    std::vector<std::pair<uint64_t, int>> tp_returns;  ///< Saved return addresses of traced calls in flight.
    bool has_raw_result {false};  ///< A handler that sets the full 64-bit rax itself.
    uint64_t raw_result {0};
    GuestThreadControl* control {nullptr};
};

// ---------------------------------------------------------------------------------------
// The process
// ---------------------------------------------------------------------------------------

struct KernelOptions {
    std::string system_root;   ///< Host directory for C:\Windows (the EmbeddedXvd/Windows folder).
    std::string title_root;    ///< Host directory for the title's files (the Mount folder).
    std::string exe_name;      ///< File name of the executable inside title_root.
    std::string cwd_nt = "G:\\";
    std::string command_line;
    uint64_t arena_size = 0x2000000000ull;  ///< 128 GB.
    bool trace = false;
    std::string trace_filter;
};

class Kernel {
public:
    explicit Kernel(KernelOptions options);
    ~Kernel();

    bool Boot(std::string& error);
    /// Runs until the process exits. Returns the process exit status.
    int Run();

    // ---- services used by syscall handlers ----
    GuestAddressSpace& Space() { return *space_; }
    VirtualMemory& Vm() { return *vm_; }
    const KernelOptions& Options() const { return options_; }

    HANDLE AddHandle(std::shared_ptr<Object> object);
    std::shared_ptr<Object> GetObject(HANDLE h);
    template <typename T>
    std::shared_ptr<T> Get(HANDLE h, ObjType type) {
        auto o = GetObject(h);
        if (!o || o->type != type) return nullptr;
        return std::static_pointer_cast<T>(o);
    }
    bool CloseHandle(HANDLE h);
    HANDLE DuplicateHandle(HANDLE h);

    /// Maps an NT path ("\??\C:\Windows\x", "\SystemRoot\x", relative to a directory
    /// handle) onto the host file system, resolving case insensitively.
    bool ResolvePath(const std::string& nt_path, std::string* host_path, bool* exists, bool create_parent_ok = false);
    std::string NtPathFromHost(const std::string& host_path) const;

    std::shared_ptr<NtThread> CurrentThread();
    std::shared_ptr<NtThread> FindThread(uint64_t tid);
    std::shared_ptr<NtThread> FindThreadByHandle(HANDLE h);

    /// Allocates a TEB and stack and registers a (not yet running) thread.
    std::shared_ptr<NtThread> NewThread(uint64_t stack_reserve, uint64_t stack_commit, std::string& error);
    void StartThread(std::shared_ptr<NtThread> thread, uint64_t context_rip, uint64_t arg0, uint64_t arg1,
                     bool suspended);
    void ExitThread(NtThread& thread, int32_t status);
    [[noreturn]] void ExitProcess(int32_t status);

    uint64_t PebAddress() const { return peb_; }
    uint64_t NtdllBase() const { return ntdll_base_; }
    /// Link time of the console OS's ntdll: tells which OS build (and so which D3D11.X ABI) the dump has.
    uint32_t NtdllTimestamp() const { return ntdll_image_ ? ntdll_image_->timestamp : 0; }
    uint64_t ImageBase() const { return exe_base_; }
    uint64_t FindLoadedImage(const std::string& name) const;
    uint64_t KusdAddress() const { return kusd_; }

    // Waiting
    std::mutex& WaitMutex() { return wait_mutex_; }
    std::condition_variable& WaitCv() { return wait_cv_; }
    void NotifyAll() { wait_cv_.notify_all(); }

    // Time helpers
    static int64_t MonotonicNs();
    static int64_t NtSystemTime();  ///< 100ns ticks since 1601.
    static uint64_t QpcNow();       ///< At 10 MHz.

    // Image mapping used by sections
    struct MappedImage {
        uint64_t base {};
        uint64_t size {};
        PeImage* pe {};
    };
    NTSTATUS MapImageSection(SectionObject& section, uint64_t* base_io, uint64_t* size_io, bool relocate = false);

    // Dispatch
    NTSTATUS Dispatch(NtThread& thread, uint32_t number, SyscallArgs& args);
    const char* SyscallName(uint32_t number) const;
    /// "image+rva (nearest export+off)" for traces.
    std::string Describe(uint64_t address);
    /// Rewrites an image's references to KUSER_SHARED_DATA (0x7ffe0000, which this host cannot map)
    /// to use the copy beside ntdll, following the table in data/kusd made by tools/kusd_scan.py.
    void ApplyKusdPatches(const PeImage& pe, uint64_t base);
    /// Replaces selected exports of a mapped image with host implementations (hle.cpp).
    void ApplyHleHooks(const std::string& image, uint64_t base);
    bool DispatchHle(NtThread& thread, uint32_t number, SyscallArgs& args, uint64_t* result);
    /// Applies a host request to run a guest function (see NtThread::CallGuest).
    void StartGuestCall(NtThread& thread, const GuestStep& step, const GuestCpuState& caller);
    /// A guest function called by the host has returned (via the return stub).
    void OnGuestReturn(NtThread& thread, uint64_t rax);
    uint64_t GuestReturnStub();
    /// Code that new threads start in: it gives the thread the title's implicit TLS (see FixThreadTls) and then jumps to the real start.
    /// `block` holds {start routine, argument}.
    uint64_t ThreadStartShim();
    void FixThreadTls(NtThread& thread, uint64_t heap_array);
    /// Unwinds a guest stack using the images' .pdata. `regs` is rax..r15 in encoding order.
    std::vector<uint64_t> Backtrace(uint64_t rip, const uint64_t* regs, int max_frames = 24);

    FexEngine& Engine() { return *engine_; }

    std::atomic<bool> exiting {false};
    std::atomic<int32_t> exit_code {0};

    // Trace
    bool Tracing() const { return options_.trace; }

    std::map<uint64_t, std::string> image_names;  ///< Loaded image base -> name, for traces.
    mutable std::mutex image_mutex;

    // Wake-ups for APCs and timers
    void QueueApc(NtThread& target, const PendingApc& apc);
    /// Delivers one pending user APC to the calling thread by arranging for the guest to
    /// run KiUserApcDispatcher. Returns true if one was delivered.
    bool DeliverUserApc(NtThread& thread);

    uint64_t ki_user_apc_dispatcher {0};
    uint64_t ki_user_exception_dispatcher {0};

    void RegisterTimer(std::shared_ptr<TimerObject> timer);
    void CheckTimersLocked();

    uint32_t NumberOfProcessors() const { return nprocs_; }

    /// A tracepoint is a patch in guest code that logs and carries on; see InstallTracepoints.
    struct Tracepoint { std::string name; uint64_t address; bool log_return {false}; };
    std::vector<Tracepoint> tracepoints;

private:
    void RegisterSyscalls();
    bool PrepareNtdll(std::string& error);
    bool BuildProcess(std::string& error);
    void DumpLoaderState();
    void DumpRequestedMemory(const char* env);
    void DumpHashTable();
    void DumpImportProgress(uint64_t base);
    void BuildKusd();
    void InstallTracepointsFor(const std::string& image, uint64_t base);
    uint64_t BuildProcessParameters();
    void TimerThreadMain();

    KernelOptions options_;
    std::unique_ptr<GuestAddressSpace> space_;
    std::unique_ptr<VirtualMemory> vm_;
    std::unique_ptr<FexEngine> engine_;

    std::mutex handle_mutex_;
    std::unordered_map<HANDLE, std::shared_ptr<Object>> handles_;
    HANDLE next_handle_ {4};

    std::mutex thread_mutex_;
    std::map<uint64_t, std::shared_ptr<NtThread>> threads_;
    uint64_t next_tid_ {0x100};

    std::mutex wait_mutex_;
    std::condition_variable wait_cv_;

    std::unordered_map<uint32_t, std::pair<std::string, SyscallFn>> syscalls_;
    std::unordered_map<std::string, SyscallFn> handlers_;
    std::unordered_map<uint32_t, std::string> numbers_;
    std::map<uint64_t, std::string> ntdll_symbols_;

    uint64_t ntdll_base_ {0};
    uint64_t exe_base_ {0};
    uint64_t peb_ {0};
    uint64_t kusd_ {0};
    uint64_t kusd_stub_next_ {0};
    uint64_t tp_stub_next_ {0};
    uint64_t tp_return_stub_ {0};
    bool winrt_init_pending_ {false};
    uint64_t hle_stub_next_ {0};
    uint64_t guest_return_stub_ {0};
    uint64_t process_heap_ {0};
    uint32_t nprocs_ {4};
    std::unique_ptr<PeImage> ntdll_image_;
    std::unique_ptr<PeImage> exe_image_;
    std::vector<std::shared_ptr<TimerObject>> timers_;
    std::thread timer_thread_;
    std::mutex exit_mutex_;
    std::condition_variable exit_cv_;
    bool process_done_ {false};
    friend class NtThread;
};

/// Registers all the syscall handlers. Defined across the sys_*.cpp files.
void RegisterMemorySyscalls(std::unordered_map<std::string, SyscallFn>& table);
void RegisterFileSyscalls(std::unordered_map<std::string, SyscallFn>& table);
void RegisterSyncSyscalls(std::unordered_map<std::string, SyscallFn>& table);
void RegisterThreadSyscalls(std::unordered_map<std::string, SyscallFn>& table);
void RegisterInfoSyscalls(std::unordered_map<std::string, SyscallFn>& table);
void RegisterMiscSyscalls(std::unordered_map<std::string, SyscallFn>& table);
void RegisterWorkerFactorySyscalls(std::unordered_map<std::string, SyscallFn>& table);
/// Starts a worker thread when a packet arrives at a port whose pool has none idle.
void WorkerFactoryPacketQueued(Kernel& k, const std::shared_ptr<IoCompletionObject>& port);
void RegisterRegistrySyscalls(std::unordered_map<std::string, SyscallFn>& table);
void InitRegistry(const std::string& config_dir);
void RegisterPackageClasses(const std::string& manifest_path);

bool BuildNlsTables(VirtualMemory& vm, uint64_t* ansi, uint64_t* oem, uint64_t* casemap);

/// Helpers for reading guest structures.
std::u16string ReadUnicodeString(const UNICODE_STRING* us);
std::string Utf16ToUtf8(const std::u16string& s);
std::u16string Utf8ToUtf16(const std::string& s);

} // namespace utsuro::nt
