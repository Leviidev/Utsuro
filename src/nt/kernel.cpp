#include "nt/kernel.h"
#include <pthread/qos.h>

#include "engine/log.h"

#include <dirent.h>
#include <fcntl.h>
#include <mach/mach_time.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/time.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace utsuro::nt {

static uint64_t AlignUpPage(uint64_t v) { return (v + 0xFFF) & ~0xFFFull; }

// ---------------------------------------------------------------------------------------
// Strings
// ---------------------------------------------------------------------------------------

std::u16string ReadUnicodeString(const UNICODE_STRING* us) {
    if (us == nullptr || us->Buffer == 0) return {};
    return std::u16string(reinterpret_cast<const char16_t*>(us->Buffer), us->Length / 2);
}

std::string Utf16ToUtf8(const std::u16string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        uint32_t c = s[i];
        if (c >= 0xD800 && c < 0xDC00 && i + 1 < s.size()) {
            c = 0x10000 + ((c - 0xD800) << 10) + (s[++i] - 0xDC00);
        }
        if (c < 0x80) out += static_cast<char>(c);
        else if (c < 0x800) { out += static_cast<char>(0xC0 | (c >> 6)); out += static_cast<char>(0x80 | (c & 0x3F)); }
        else if (c < 0x10000) { out += static_cast<char>(0xE0 | (c >> 12)); out += static_cast<char>(0x80 | ((c >> 6) & 0x3F)); out += static_cast<char>(0x80 | (c & 0x3F)); }
        else { out += static_cast<char>(0xF0 | (c >> 18)); out += static_cast<char>(0x80 | ((c >> 12) & 0x3F)); out += static_cast<char>(0x80 | ((c >> 6) & 0x3F)); out += static_cast<char>(0x80 | (c & 0x3F)); }
    }
    return out;
}

std::u16string Utf8ToUtf16(const std::string& s) {
    std::u16string out;
    for (size_t i = 0; i < s.size();) {
        uint32_t c = static_cast<uint8_t>(s[i]);
        int extra = c < 0x80 ? 0 : c < 0xE0 ? 1 : c < 0xF0 ? 2 : 3;
        if (extra == 0) { out += static_cast<char16_t>(c); ++i; continue; }
        c &= (0x3F >> extra);
        for (int k = 1; k <= extra && i + static_cast<size_t>(k) < s.size(); ++k) c = (c << 6) | (static_cast<uint8_t>(s[i + static_cast<size_t>(k)]) & 0x3F);
        i += static_cast<size_t>(extra) + 1;
        if (c >= 0x10000) { c -= 0x10000; out += static_cast<char16_t>(0xD800 + (c >> 10)); out += static_cast<char16_t>(0xDC00 + (c & 0x3FF)); }
        else out += static_cast<char16_t>(c);
    }
    return out;
}

FileObject::~FileObject() {
    if (fd >= 0) ::close(fd);
    if (delete_on_close && !host_path.empty()) ::unlink(host_path.c_str());
}

// ---------------------------------------------------------------------------------------
// Time
// ---------------------------------------------------------------------------------------

int64_t Kernel::MonotonicNs() {
    static mach_timebase_info_data_t tb = [] { mach_timebase_info_data_t t; mach_timebase_info(&t); return t; }();
    return static_cast<int64_t>(mach_absolute_time() * tb.numer / tb.denom);
}

int64_t Kernel::NtSystemTime() {
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    constexpr int64_t kEpochDiff = 11644473600ll;  // seconds from 1601 to 1970
    return (static_cast<int64_t>(tv.tv_sec) + kEpochDiff) * 10000000ll + tv.tv_usec * 10;
}

uint64_t Kernel::QpcNow() { return static_cast<uint64_t>(MonotonicNs() / 100); }

// ---------------------------------------------------------------------------------------
// NtThread
// ---------------------------------------------------------------------------------------

GuestAddressSpace* NtThread::Space() const { return &kernel_.Space(); }
bool NtThread::StopRequested() const { return kernel_.exiting.load(); }

void NtThread::ApplyHostPriority() {
    // On a phone the cores are not equal, and what runs where is decided by QoS. The game's render thread and its background workers
    // (saving, chunk compression) would otherwise all inherit one class and fight for the fast cores.
    const int32_t p = base_priority.load(std::memory_order_relaxed);
    qos_class_t cls = QOS_CLASS_USER_INITIATED;
    if (p >= 9) cls = QOS_CLASS_USER_INTERACTIVE;
    else if (p <= 3) cls = QOS_CLASS_UTILITY;
    else if (p < 8) cls = QOS_CLASS_DEFAULT;
    pthread_set_qos_class_self_np(cls, 0);
}

uint64_t NtThread::Handle(uint64_t number, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5,
                          uint64_t a6) {
    if (priority_dirty.load(std::memory_order_relaxed) && priority_dirty.exchange(false)) ApplyHostPriority();
    // FEXCore's OS_LINUX64 order: rax, rdi, rsi, rdx, r10, r8, r9. Windows: r10, rdx, r8, r9.
    SyscallArgs args;
    args.reg[0] = a4;
    args.reg[1] = a3;
    args.reg[2] = a5;
    args.reg[3] = a6;
    args.rsp = guest ? guest->Rsp() : 0;
    current_args = &args;
    current_syscall = number;
    history[history_count++ % history.size()] = {static_cast<uint32_t>(number), args.rsp != 0 ? *reinterpret_cast<uint64_t*>(args.rsp) : 0};
    current_rip = guest ? guest->Rip() : 0;
    // The syscall instruction ends its block, so the thread continues wherever rip says:
    // step past it now. Handlers that put the thread somewhere else overwrite this.
    static const bool keep_stacks = std::getenv("UTSURO_STACKS") != nullptr;
    if (keep_stacks && control != nullptr) {
        GuestCpuState st;
        if (control->CaptureCpuState(&st)) {
            for (int i = 0; i < 16; ++i) stack_regs[i] = st.gregs[i];
            stack_rip = st.rip;
        }
    }
    if (control != nullptr) control->AdvanceGuestRip(2);
    has_raw_result = false;
    has_pending_step = false;
    const NTSTATUS status = kernel_.Dispatch(*this, static_cast<uint32_t>(number), args);
    current_args = nullptr;
    if (has_pending_step && control != nullptr) {
        // A host method asked for a guest call: make it now, remembering where to come back to.
        has_pending_step = false;
        GuestCpuState caller;
        if (control->CaptureCpuState(&caller)) {
            guest_frames.push_back({caller, pending_step.cont});
            kernel_.StartGuestCall(*this, pending_step, caller);
        }
        return 0;
    }
    if (has_raw_result) return raw_result;
    return static_cast<uint64_t>(static_cast<int64_t>(status));
}

// ---------------------------------------------------------------------------------------
// Kernel: handles
// ---------------------------------------------------------------------------------------

Kernel::Kernel(KernelOptions options) : options_ {std::move(options)} {
    int n = 4;
    size_t len = sizeof(n);
    if (sysctlbyname("hw.logicalcpu", &n, &len, nullptr, 0) == 0) nprocs_ = static_cast<uint32_t>(std::clamp(n, 2, 8));
}

Kernel::~Kernel() {
    exiting = true;
    wait_cv_.notify_all();
    if (timer_thread_.joinable()) timer_thread_.join();
}

HANDLE Kernel::AddHandle(std::shared_ptr<Object> object) {
    std::scoped_lock lock {handle_mutex_};
    const HANDLE h = next_handle_;
    next_handle_ += 4;
    handles_[h] = std::move(object);
    return h;
}

std::shared_ptr<Object> Kernel::GetObject(HANDLE h) {
    if (h == ~0ull) {  // current process pseudo handle
        return nullptr;
    }
    if (h == ~1ull) {  // current thread
        return CurrentThread();
    }
    std::scoped_lock lock {handle_mutex_};
    auto it = handles_.find(h);
    return it == handles_.end() ? nullptr : it->second;
}

bool Kernel::CloseHandle(HANDLE h) {
    std::shared_ptr<Object> dead;
    {
        std::scoped_lock lock {handle_mutex_};
        auto it = handles_.find(h);
        if (it == handles_.end()) return false;
        dead = std::move(it->second);
        handles_.erase(it);
    }
    return true;
}

HANDLE Kernel::DuplicateHandle(HANDLE h) {
    auto o = GetObject(h);
    if (!o) return 0;
    return AddHandle(o);
}

NtThread* CurrentNtThreadRaw();

std::shared_ptr<NtThread> Kernel::CurrentThread() {
    NtThread* t = CurrentNtThreadRaw();
    if (t == nullptr) return nullptr;
    return std::static_pointer_cast<NtThread>(t->shared_from_this());
}

std::shared_ptr<NtThread> Kernel::FindThread(uint64_t tid) {
    std::scoped_lock lock {thread_mutex_};
    auto it = threads_.find(tid);
    return it == threads_.end() ? nullptr : it->second;
}

std::shared_ptr<NtThread> Kernel::FindThreadByHandle(HANDLE h) {
    if (h == ~1ull) return CurrentThread();
    return Get<NtThread>(h, ObjType::Thread);
}

// ---------------------------------------------------------------------------------------
// Paths
// ---------------------------------------------------------------------------------------

namespace {

std::string Lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

/// Finds `name` inside `dir` ignoring case. Returns the real name or "".
std::string FindCaseInsensitive(const std::string& dir, const std::string& name) {
    std::string direct = dir + "/" + name;
    struct stat st;
    if (lstat(direct.c_str(), &st) == 0) return name;
    DIR* d = opendir(dir.c_str());
    if (d == nullptr) return {};
    const std::string want = Lower(name);
    std::string found;
    while (auto* e = readdir(d)) {
        if (Lower(e->d_name) == want) { found = e->d_name; break; }
    }
    closedir(d);
    return found;
}

} // namespace

bool Kernel::ResolvePath(const std::string& nt_in, std::string* host_path, bool* exists, bool create_parent_ok) {
    std::string p = nt_in;
    std::replace(p.begin(), p.end(), '/', '\\');
    std::string root;
    std::string rest;
    auto starts = [&](const char* prefix) {
        const size_t n = std::strlen(prefix);
        if (p.size() >= n && Lower(p.substr(0, n)) == Lower(prefix)) { rest = p.substr(n); return true; }
        return false;
    };
    const std::string xvd_root = options_.system_root.substr(0, options_.system_root.rfind('/'));
    // The title's own app data (ApplicationData.LocalFolder and friends) is writable and lives with the saves.
    if (starts("\\??\\C:\\Users\\Default\\AppData\\") || starts("C:\\Users\\Default\\AppData\\")) {
        const char* e = std::getenv("UTSURO_SAVES");
        std::string base = e != nullptr ? std::string(e) : std::string(std::getenv("HOME")) + "/Library/Application Support/Utsuro/saves";
        while (base.size() > 1 && base.back() == '/') base.pop_back();
        root = base.substr(0, base.rfind('/')) + "/AppData";
        mkdir(root.c_str(), 0755);
    } else if (starts("\\??\\T:\\") || starts("\\DosDevices\\T:\\") || starts("T:\\")) {
        const char* e = std::getenv("UTSURO_SAVES");
        std::string base = e != nullptr ? std::string(e) : std::string(std::getenv("HOME")) + "/Library/Application Support/Utsuro/saves";
        while (base.size() > 1 && base.back() == '/') base.pop_back();
        root = base.substr(0, base.rfind('/')) + "/Temp";
        mkdir(root.c_str(), 0755);
    } else if (starts("\\??\\C:\\") || starts("\\DosDevices\\C:\\") || starts("C:\\") || starts("\\??\\Volume{c}\\")) {
        root = xvd_root;
    } else if (starts("\\SystemRoot\\") || starts("\\??\\SystemRoot\\")) {
        root = options_.system_root;
    } else if (starts("\\??\\G:\\") || starts("\\DosDevices\\G:\\") || starts("G:\\") ||
               starts("\\Device\\Harddisk0\\Partition1\\")) {
        root = options_.title_root;
    } else if (starts("\\??\\D:\\") || starts("D:\\")) {
        root = options_.title_root;
    } else {
        if (exists) *exists = false;
        return false;
    }
    // A trailing separator names the same thing; keeping it would make a missing leaf look like a missing parent.
    while (!rest.empty() && rest.back() == '\\') rest.pop_back();
    // Walk the components.
    std::string cur = root;
    size_t i = 0;
    bool ok = true;
    while (i <= rest.size() && !rest.empty()) {
        size_t j = rest.find('\\', i);
        if (j == std::string::npos) j = rest.size();
        std::string comp = rest.substr(i, j - i);
        i = j + 1;
        if (comp.empty() || comp == ".") { if (j >= rest.size()) break; continue; }
        if (comp == "..") { cur = cur.substr(0, cur.rfind('/')); if (j >= rest.size()) break; continue; }
        std::string real = FindCaseInsensitive(cur, comp);
        if (real.empty()) {
            // The rest does not exist; keep the remaining components as given so a create can use them.
            cur += "/" + comp;
            ok = false;
            while (j < rest.size()) {
                size_t k = rest.find('\\', j + 1);
                if (k == std::string::npos) k = rest.size();
                cur += "/" + rest.substr(j + 1, k - j - 1);
                j = k;
            }
            break;
        }
        cur += "/" + real;
        if (j >= rest.size()) break;
    }
    *host_path = cur;
    if (exists) *exists = ok;
    return true;
}

std::string Kernel::NtPathFromHost(const std::string& host) const {
    const std::string xvd_root = options_.system_root.substr(0, options_.system_root.rfind('/'));
    auto conv = [&](const std::string& root, const char* prefix) -> std::string {
        if (host.compare(0, root.size(), root) == 0 && (host.size() == root.size() || host[root.size()] == '/')) {
            std::string r = host.substr(root.size());
            std::replace(r.begin(), r.end(), '/', '\\');
            return std::string(prefix) + (r.empty() ? "\\" : r);
        }
        return {};
    };
    std::string s = conv(options_.title_root, "G:");
    if (s.empty()) s = conv(xvd_root, "C:");
    if (s.empty()) {
        const char* e = std::getenv("UTSURO_SAVES");
        std::string base = e != nullptr ? std::string(e) : std::string(std::getenv("HOME")) + "/Library/Application Support/Utsuro/saves";
        while (base.size() > 1 && base.back() == '/') base.pop_back();
        s = conv(base.substr(0, base.rfind('/')) + "/Temp", "T:");
    }
    return s;
}

// ---------------------------------------------------------------------------------------
// Dispatch
// ---------------------------------------------------------------------------------------

uint64_t Kernel::GuestReturnStub() {
    if (guest_return_stub_ == 0) {
        uint64_t base = 0, size = 0x1000;
        vm_->Allocate(&base, &size, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE, RegionKind::Private, "guest return");
        auto* p = reinterpret_cast<uint8_t*>(base);
        p[0] = 0x4C; p[1] = 0x8B; p[2] = 0xD0;                 // mov r10, rax
        p[3] = 0xB8; const uint32_t n = 0xFFFFFFF0u; std::memcpy(p + 4, &n, 4);  // mov eax, 0xFFFFFFF0
        p[8] = 0x0F; p[9] = 0x05;                               // syscall
        p[10] = 0xCC;
        guest_return_stub_ = base;
    }
    return guest_return_stub_;
}

uint64_t Kernel::ThreadStartShim() {
    static uint64_t shim = 0;
    if (shim == 0) {
        uint64_t base = 0, size = 0x1000;
        vm_->Allocate(&base, &size, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE, RegionKind::Private, "thread start shim");
        auto* p = reinterpret_cast<uint8_t*>(base);
        size_t n = 0;
        auto put = [&](std::initializer_list<uint8_t> bytes) { for (uint8_t b : bytes) p[n++] = b; };
        const uint64_t allocate = FindExport(ntdll_base_, "RtlAllocateHeap");
        put({0x53});                                  // push rbx
        put({0x48, 0x89, 0xCB});                      // mov rbx, rcx           (block: start routine, argument)
        put({0x48, 0x83, 0xEC, 0x20});                // sub rsp, 32
        put({0x65, 0x48, 0x8B, 0x04, 0x25, 0x60, 0x00, 0x00, 0x00});   // mov rax, gs:[0x60]     PEB
        put({0x48, 0x8B, 0x48, 0x30});                // mov rcx, [rax+0x30]    ProcessHeap
        put({0xBA, 0x08, 0x00, 0x00, 0x00});          // mov edx, HEAP_ZERO_MEMORY
        put({0x41, 0xB8, 0x80, 0x00, 0x00, 0x00});    // mov r8d, 128
        put({0x48, 0xB8});                            // mov rax, RtlAllocateHeap
        std::memcpy(p + n, &allocate, 8); n += 8;
        put({0xFF, 0xD0});                            // call rax               a TLS array with room for the title's index
        put({0x49, 0x89, 0xC2});                      // mov r10, rax
        put({0xB8, 0xF1, 0xFF, 0xFF, 0xFF});          // mov eax, 0xFFFFFFF1
        put({0x0F, 0x05});                            // syscall                (host: fills it in)
        put({0x48, 0x83, 0xC4, 0x20});                // add rsp, 32
        put({0x48, 0x8B, 0x4B, 0x08});                // mov rcx, [rbx+8]
        put({0x48, 0x8B, 0x03});                      // mov rax, [rbx]
        put({0x5B});                                  // pop rbx
        put({0xFF, 0xE0});                            // jmp rax
        shim = base;
    }
    return shim;
}

// The title's own implicit TLS (a .tls section; the host maps the image without the loader's help): the loader gives every thread a copy at
// ThreadLocalStoragePointer[index], and the console's loader hands the title index 1, which its code has built in. ntdll here knows only
// index 0 and sizes the per-thread array for that alone, so the thread gets a larger array (`heap_array`, from the process heap, so that
// ntdll can free it as its own) holding the same block under both numbers.
void Kernel::FixThreadTls(NtThread& thread, uint64_t heap_array) {
    if (exe_image_ == nullptr || thread.teb == 0 || heap_array == 0) return;
    const auto* dos = reinterpret_cast<const uint8_t*>(exe_base_);
    const uint32_t nt = *reinterpret_cast<const uint32_t*>(dos + 0x3C);
    const uint32_t tls_rva = *reinterpret_cast<const uint32_t*>(dos + nt + 0x18 + 0x70 + 9 * 8);   // PE32+ data directory 9
    if (tls_rva == 0) return;
    struct TlsDirectory { uint64_t start, end, index, callbacks; uint32_t zero_fill, characteristics; };
    const auto* dir = reinterpret_cast<const TlsDirectory*>(exe_base_ + tls_rva);
    constexpr uint64_t kIndex = 1;
    uint64_t& array_ptr = *reinterpret_cast<uint64_t*>(thread.teb + 0x58);
    const uint64_t own = dir->index != 0 && vm_->Contains(dir->index, 4) ? *reinterpret_cast<uint32_t*>(dir->index) : 0;
    auto* old_slots = reinterpret_cast<uint64_t*>(array_ptr);
    // ntdll's TLS arrays carry their length 16 bytes before the first slot, and it frees the block from there; one entry, so that the
    // duplicate under the title's index is not freed a second time.
    *reinterpret_cast<uint32_t*>(heap_array) = 1;
    auto* slots = reinterpret_cast<uint64_t*>(heap_array + 0x10);
    if (old_slots != nullptr) {
        std::memcpy(slots, old_slots, 8 * (own + 1));
    }
    uint64_t block = slots[own];
    if (block == 0) {
        // ntdll has no block for it either: make one (it will not free this one).
        const uint64_t raw = dir->end - dir->start, total = raw + dir->zero_fill;
        uint64_t size = (total + 0xFFFF) & ~0xFFFFull;
        if (vm_->Allocate(&block, &size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE) != STATUS_SUCCESS) return;
        std::memcpy(reinterpret_cast<void*>(block), reinterpret_cast<const void*>(dir->start), raw);
        slots[own] = block;
        *reinterpret_cast<uint32_t*>(heap_array) = 0;   // not a heap block: ntdll must not free it
    }
    slots[kIndex] = block;
    array_ptr = heap_array + 0x10;
    UT_INFO("tls: thread %llu: block %#llx under index %llu and 1; array %#llx", (unsigned long long)thread.tid_, (unsigned long long)block, (unsigned long long)own, (unsigned long long)array_ptr);
}

void Kernel::StartGuestCall(NtThread& thread, const GuestStep& step, const GuestCpuState& caller) {
    GuestCpuState st = caller;
    // A fresh frame below the caller's stack pointer: [ret][shadow x4] with the return address at rsp.
    const uint64_t base = (caller.gregs[4] - 0x80) & ~0xFull;
    *reinterpret_cast<uint64_t*>(base - 8) = GuestReturnStub();
    st.gregs[4] = base - 8;
    st.gregs[1] = step.args[0];
    st.gregs[2] = step.args[1];
    st.gregs[8] = step.args[2];
    st.gregs[9] = step.args[3];
    st.rip = step.fn;
    thread.control->RestoreCpuState(st);
}

void Kernel::OnGuestReturn(NtThread& thread, uint64_t rax) {
    if (thread.guest_frames.empty()) { UT_ERROR("guest return with no pending host call"); return; }
    GuestFrame frame = std::move(thread.guest_frames.back());
    thread.guest_frames.pop_back();
    GuestStep step = frame.cont ? frame.cont(thread, rax) : GuestStep::Return(rax);
    if (step.call) {
        thread.guest_frames.push_back({frame.saved, step.cont});
        StartGuestCall(thread, step, frame.saved);
        return;
    }
    GuestCpuState st = frame.saved;
    st.gregs[0] = step.result;
    thread.control->RestoreCpuState(st);
    thread.has_raw_result = true;
    thread.raw_result = step.result;
}

const char* Kernel::SyscallName(uint32_t number) const {
    auto it = numbers_.find(number);
    return it == numbers_.end() ? "?" : it->second.c_str();
}

NTSTATUS Kernel::Dispatch(NtThread& thread, uint32_t number, SyscallArgs& args) {
    if (number == 0xFFFFFFF1u) {  // a new thread is about to run its start routine
        FixThreadTls(thread, args.reg[0]);
        return STATUS_SUCCESS;
    }
    if (number == 0xFFFFFFF0u) {  // a guest function called from the host returned; r10 holds its rax
        OnGuestReturn(thread, args.reg[0]);
        return STATUS_SUCCESS;
    }
    {
        uint64_t result;
        if (DispatchHle(thread, number, args, &result)) {
            thread.has_raw_result = true;
            thread.raw_result = result;
            return STATUS_SUCCESS;
        }
    }
    if (number == 0xFFFF7001u) {  // a traced function returned
        if (thread.tp_returns.empty()) return STATUS_SUCCESS;
        const auto [ret, id] = thread.tp_returns.back();
        thread.tp_returns.pop_back();
        UT_INFO("[%llu] TP%d returned rax=%#llx", (unsigned long long)thread.tid_, id, (unsigned long long)args.reg[0]);
        GuestCpuState st;
        if (thread.control != nullptr && thread.control->CaptureCpuState(&st)) {
            st.rip = ret;
            st.gregs[0] = args.reg[0];  // r10 carried rax
            thread.control->RestoreCpuState(st);
            thread.has_raw_result = true;
            thread.raw_result = args.reg[0];
        }
        return STATUS_SUCCESS;
    }
    if (number >= 0xFFFF0000u) {  // a tracepoint
        const uint32_t id = number - 0xFFFF0000u;
        const uint64_t ret = args.rsp != 0 ? *reinterpret_cast<uint64_t*>(args.rsp + 32) : 0;
        if (id == 0) {  // vDbgPrintExWithPrefix(prefix, component, level, format, arglist)
            auto readable = [&](uint64_t p, uint64_t n = 8) { return p > 0x10000 && vm_->Contains(p, n) && space_->MaybeCommitted(p) && space_->MaybeCommitted(p + n - 1); };
            const char* fmt = readable(args.reg[3]) ? reinterpret_cast<const char*>(args.reg[3]) : nullptr;
            uint64_t va_ptr = readable(args.rsp + 0x48) ? *reinterpret_cast<uint64_t*>(args.rsp + 0x48) : 0;
            const uint64_t* va = readable(va_ptr, 64) ? reinterpret_cast<const uint64_t*>(va_ptr) : nullptr;
            std::string out;
            int arg = 0;
            for (const char* f = fmt; f != nullptr && *f; ++f) {
                if (*f != '%') { out += *f; continue; }
                std::string spec = "%";
                ++f;
                while (*f && std::strchr("-+ #0123456789.*hlIwLqjzt", *f)) spec += *f++;
                char buf[512];
                // "%.*s" / "%*d": the precision or width comes from the argument list first.
                long long star_precision = -1;
                if (spec.find(".*") != std::string::npos) { star_precision = va ? static_cast<int32_t>(va[arg]) : 0; ++arg; }
                else if (spec.find('*') != std::string::npos) ++arg;
                const uint64_t v = va ? va[arg] : 0;
                switch (*f) {
                case 's': ++arg; if (spec.find('w') != std::string::npos || spec.find('l') != std::string::npos) {
                              out += readable(v, 2) ? Utf16ToUtf8(std::u16string(reinterpret_cast<const char16_t*>(v))) : "(bad)";
                          } else {
                              if (!readable(v, 1)) out += "(bad)";
                              else if (star_precision >= 0) out.append(reinterpret_cast<const char*>(v), static_cast<size_t>(std::min<long long>(star_precision, 512)));
                              else out += reinterpret_cast<const char*>(v);
                          } break;
                case 'S': ++arg; out += readable(v, 2) ? Utf16ToUtf8(std::u16string(reinterpret_cast<const char16_t*>(v))) : "(bad)"; break;
                case 'Z': { ++arg; out += readable(v, 16) ? Utf16ToUtf8(ReadUnicodeString(reinterpret_cast<const UNICODE_STRING*>(v))) : "(bad)"; break; }
                case 'c': ++arg; out += static_cast<char>(v); break;
                case 'd': case 'i': ++arg; std::snprintf(buf, sizeof buf, (spec + (spec.find("I64") != std::string::npos || spec.find("ll") != std::string::npos ? "" : "") + "lld").c_str(), spec.find("I64") != std::string::npos || spec.find("ll") != std::string::npos ? static_cast<long long>(v) : static_cast<long long>(static_cast<int32_t>(v))); out += buf; break;
                case 'u': case 'x': case 'X': case 'o': { ++arg; const bool wide = spec.find("I64") != std::string::npos || spec.find("ll") != std::string::npos; std::string s2 = spec; size_t p2; while ((p2 = s2.find("I64")) != std::string::npos) s2.erase(p2, 3); s2 += "ll"; s2 += *f; std::snprintf(buf, sizeof buf, s2.c_str(), static_cast<unsigned long long>(wide ? v : static_cast<uint32_t>(v))); out += buf; break; }
                case 'p': ++arg; std::snprintf(buf, sizeof buf, "%#llx", static_cast<unsigned long long>(v)); out += buf; break;
                case '%': out += '%'; break;
                default: out += spec; out += *f; break;
                }
            }
            while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
            if (out.find("(bad)") != std::string::npos && fmt != nullptr) UT_INFO("[%llu] ntdll (format \"%s\", args at %#llx): %s", (unsigned long long)thread.tid_, fmt, (unsigned long long)va_ptr, out.c_str());
            else UT_INFO("[%llu] ntdll: %s", (unsigned long long)thread.tid_, out.c_str());
            return STATUS_SUCCESS;
        }
        UT_INFO("[%llu] TP%u %s rsp=%#llx rcx=%#llx rdx=%#llx r8=%#llx r9=%#llx <- %s", (unsigned long long)thread.tid_, id,
                id < tracepoints.size() ? tracepoints[id].name.c_str() : "?", (unsigned long long)args.rsp + 32, (unsigned long long)args.reg[0],
                (unsigned long long)args.reg[1], (unsigned long long)args.reg[2], (unsigned long long)args.reg[3],
                Describe(ret).c_str());
        // UTSURO_TPDUMP=rcx|rdx|r8|r9|rsp[+off]:len also dumps guest memory around an argument register of the tracepoint.
        if (const char* td = std::getenv("UTSURO_TPDUMP"); td != nullptr && (td[0] == 'r')) {
            const std::string t = td;
            const auto colon = t.find(':');
            const std::string r = t.substr(0, t.find_first_of("+:"));
            const uint64_t base_v = r == "rcx" ? args.reg[0] : r == "rdx" ? args.reg[1] : r == "r8" ? args.reg[2] : r == "r9" ? args.reg[3] : args.rsp + 32;
            const auto plus = t.find('+');
            const uint64_t off = plus != std::string::npos ? std::strtoull(t.c_str() + plus + 1, nullptr, 0) : 0;
            const size_t len = colon != std::string::npos ? std::strtoull(t.c_str() + colon + 1, nullptr, 0) : 32;
            if (base_v != 0)
                for (size_t o = 0; o < len; o += 16)
                    UT_WARN("    %#llx: %016llx %016llx", (unsigned long long)(base_v + off + o), (unsigned long long)*reinterpret_cast<uint64_t*>(base_v + off + o), (unsigned long long)*reinterpret_cast<uint64_t*>(base_v + off + o + 8));
        } else DumpRequestedMemory("UTSURO_TPDUMP");
        if (id < tracepoints.size() && tracepoints[id].log_return && tp_return_stub_ != 0 && args.rsp != 0) {
            auto* slot = reinterpret_cast<uint64_t*>(args.rsp + 32);
            thread.tp_returns.emplace_back(*slot, static_cast<int>(id));
            *slot = tp_return_stub_;
        }
        if (std::getenv("UTSURO_TPBT") != nullptr && thread.control != nullptr && id < tracepoints.size()) {
            GuestCpuState st;
            if (thread.control->CaptureCpuState(&st)) {
                st.gregs[4] = args.rsp + 32;  // past the stub's four pushes: at the return address
                const auto frames = Backtrace(tracepoints[id].address, st.gregs, 12);
                for (size_t i = 0; i < frames.size(); ++i) UT_INFO("      #%zu %s", i, Describe(frames[i]).c_str());
            }
        }
        return STATUS_SUCCESS;
    }
    auto it = syscalls_.find(number);
    if (it == syscalls_.end()) {
        UT_ERROR("[%llu] unknown syscall %u (%s) from %#llx", (unsigned long long)thread.tid_, number, SyscallName(number),
                 (unsigned long long)thread.current_rip);
        if (thread.control != nullptr) {
            GuestCpuState st;
            if (thread.control->CaptureCpuState(&st)) {
                uint64_t regs[16];
                for (int i = 0; i < 16; ++i) regs[i] = st.gregs[i];
                const auto frames = Backtrace(st.rip, regs, 10);
                for (size_t i = 0; i < frames.size(); ++i) UT_ERROR("      #%zu %s", i, Describe(frames[i]).c_str());
            }
        }
        return STATUS_NOT_IMPLEMENTED;
    }
    if (it->second.second == nullptr) {
        UT_WARN("[%llu] unimplemented syscall %s(%#llx, %#llx, %#llx, %#llx) from %#llx", (unsigned long long)thread.tid_,
                it->second.first.c_str(), (unsigned long long)args[0], (unsigned long long)args[1],
                (unsigned long long)args[2], (unsigned long long)args[3], (unsigned long long)thread.current_rip);
        return STATUS_NOT_IMPLEMENTED;
    }
    thread.current_ret = args.rsp != 0 ? *reinterpret_cast<uint64_t*>(args.rsp) : 0;
    const bool trace = options_.trace &&
                       (options_.trace_filter.empty() || it->second.first.find(options_.trace_filter) != std::string::npos);
    if (trace) {
        UT_INFO("[%llu] %s(%#llx, %#llx, %#llx, %#llx) <- %s", (unsigned long long)thread.tid_, it->second.first.c_str(),
                (unsigned long long)args[0], (unsigned long long)args[1], (unsigned long long)args[2],
                (unsigned long long)args[3], Describe(thread.current_ret).c_str());
    }
    const NTSTATUS status = it->second.second(*this, thread, args);
    if (trace) UT_INFO("[%llu]   -> %#x", (unsigned long long)thread.tid_, static_cast<uint32_t>(status));
    return status;
}

std::string Kernel::Describe(uint64_t address) {
    char buf[200];
    {
        std::scoped_lock lock {image_mutex};
        auto it = image_names.upper_bound(address);
        if (it != image_names.begin()) {
            --it;
            const auto info = vm_->Query(address);
            if (info.allocation_base == it->first) {
                std::string name = it->second.substr(it->second.rfind('\\') + 1);
                std::string sym;
                if (it->first == ntdll_base_) {
                    auto s = ntdll_symbols_.upper_bound(address);
                    if (s != ntdll_symbols_.begin()) { --s; std::snprintf(buf, sizeof buf, " (%s+%#llx)", s->second.c_str(), (unsigned long long)(address - s->first)); sym = buf; }
                }
                std::snprintf(buf, sizeof buf, "%s+%#llx%s", name.c_str(), (unsigned long long)(address - it->first), sym.c_str());
                return buf;
            }
        }
    }
    std::snprintf(buf, sizeof buf, "%#llx", (unsigned long long)address);
    return buf;
}

void Kernel::RegisterSyscalls() {
    RegisterMemorySyscalls(handlers_);
    RegisterFileSyscalls(handlers_);
    RegisterSyncSyscalls(handlers_);
    RegisterThreadSyscalls(handlers_);
    RegisterInfoSyscalls(handlers_);
    RegisterMiscSyscalls(handlers_);
    RegisterRegistrySyscalls(handlers_);
    RegisterWorkerFactorySyscalls(handlers_);
    // Numbers come from the ntdll stubs themselves: `mov r10, rcx; mov eax, N; syscall`.
    const auto exports = ListExports(ntdll_base_);
    for (const auto& [name, address] : exports) ntdll_symbols_[address] = name;
    for (const auto& [name, address] : exports) {
        if (name.compare(0, 2, "Nt") != 0) continue;
        const auto* b = reinterpret_cast<const uint8_t*>(address);
        // `mov r10, rcx; mov eax, N; [test byte [KUSER+0x308], 1; jne ...;] syscall`: the later OS builds put a check before the syscall.
        bool stub = b[0] == 0x4c && b[1] == 0x8b && b[2] == 0xd1 && b[3] == 0xb8;
        if (stub) {
            stub = false;
            for (int i = 8; i + 1 < 24; ++i) if (b[i] == 0x0f && b[i + 1] == 0x05) { stub = true; break; }
        }
        if (stub) {
            uint32_t n;
            std::memcpy(&n, b + 4, 4);
            auto h = handlers_.find(name);
            syscalls_[n] = {name, h == handlers_.end() ? nullptr : h->second};
            numbers_[n] = name;
        }
    }
    UT_INFO("syscalls: %zu in ntdll, %zu handled", syscalls_.size(),
            static_cast<size_t>(std::count_if(syscalls_.begin(), syscalls_.end(), [](auto& p) { return p.second.second != nullptr; })));
}

// ---------------------------------------------------------------------------------------
// Image mapping
// ---------------------------------------------------------------------------------------

namespace {
uint32_t SectionProtect(uint32_t c) {
    const bool x = (c & 0x20000000) != 0, r = (c & 0x40000000) != 0, w = (c & 0x80000000) != 0;
    if (x) return w ? PAGE_EXECUTE_READWRITE : r ? PAGE_EXECUTE_READ : PAGE_EXECUTE;
    if (w) return PAGE_READWRITE;
    if (r) return PAGE_READONLY;
    return PAGE_READONLY;
}
} // namespace

NTSTATUS Kernel::MapImageSection(SectionObject& section, uint64_t* base_io, uint64_t* size_io, bool relocate) {
    PeImage& pe = *section.image;
    uint64_t base = *base_io;
    uint64_t size = pe.size_of_image;
    NTSTATUS rc = vm_->Allocate(&base, &size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE, RegionKind::Image, pe.nt_path);
    if (rc != STATUS_SUCCESS) return rc;
    std::string error;
    if (!PlaceImage(pe, base, error, relocate)) {
        UT_ERROR("image %s: %s", pe.path.c_str(), error.c_str());
        uint64_t b = base, s = 0;
        vm_->Free(&b, &s, MEM_RELEASE);
        return STATUS_INVALID_IMAGE_FORMAT;
    }
    // Headers are read-only; each section gets its own protection.
    vm_->SetImagePages(base, pe.size_of_headers, PAGE_READONLY);
    for (const auto& s : pe.sections) {
        const uint64_t len = std::max<uint64_t>(s.vsize, s.raw_size);
        vm_->SetImagePages(base + s.rva, len, SectionProtect(s.characteristics));
    }
    space_->NotifyContentsReplaced(base, base + pe.size_of_image);
    {
        std::scoped_lock lock {image_mutex};
        image_names[base] = pe.nt_path;
    }
    if (kusd_ != 0) {
        ApplyKusdPatches(pe, base);
        std::string lower = pe.path.substr(pe.path.rfind('/') + 1);
        for (auto& ch : lower) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        ApplyHleHooks(lower, base);
        if (lower != "ntdll.dll") InstallTracepointsFor(lower, base);
    }
    section.last_base = base;
    UT_INFO("image %s mapped at %#llx (preferred %#llx)", pe.nt_path.c_str(), (unsigned long long)base,
            (unsigned long long)pe.image_base);
    *base_io = base;
    *size_io = AlignUpPage(pe.size_of_image);
    if (base != pe.image_base) return STATUS_IMAGE_NOT_AT_BASE;
    return STATUS_SUCCESS;
}

uint64_t Kernel::FindLoadedImage(const std::string& name) const {
    std::scoped_lock lock {image_mutex};
    for (const auto& [base, path] : image_names) {
        auto pos = path.rfind('\\');
        std::string filename = (pos != std::string::npos) ? path.substr(pos + 1) : path;
        if (strcasecmp(filename.c_str(), name.c_str()) == 0) return base;
    }
    return 0;
}

} // namespace utsuro::nt
