// sys_file.cpp -- the file system: NtCreateFile and friends, over the host's.
#include "nt/kernel.h"
#include "nt/logan.h"

#include "engine/log.h"

#include <dirent.h>
#include <fcntl.h>
#include <sys/mount.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include <filesystem>

#include <algorithm>
#include <cerrno>
#include <cstring>

namespace utsuro::nt {

namespace {

constexpr uint32_t FILE_ATTRIBUTE_READONLY = 0x1;
constexpr uint32_t FILE_ATTRIBUTE_DIRECTORY = 0x10;
constexpr uint32_t FILE_ATTRIBUTE_ARCHIVE = 0x20;
constexpr uint32_t FILE_ATTRIBUTE_NORMAL = 0x80;

int64_t ToNtTime(const struct timespec& ts) {
    constexpr int64_t kEpochDiff = 11644473600ll;
    return (static_cast<int64_t>(ts.tv_sec) + kEpochDiff) * 10000000ll + ts.tv_nsec / 100;
}

uint32_t AttributesOf(const struct stat& st) {
    if (S_ISDIR(st.st_mode)) return FILE_ATTRIBUTE_DIRECTORY;
    uint32_t a = FILE_ATTRIBUTE_ARCHIVE;
    if ((st.st_mode & S_IWUSR) == 0) a |= FILE_ATTRIBUTE_READONLY;
    return a;
}

NTSTATUS FromErrno(int e) {
    switch (e) {
    case ENOENT: return STATUS_OBJECT_NAME_NOT_FOUND;
    case ENOTDIR: return STATUS_OBJECT_PATH_NOT_FOUND;
    case EACCES:
    case EPERM:
    case EROFS: return STATUS_ACCESS_DENIED;
    case EEXIST: return STATUS_OBJECT_NAME_COLLISION;
    case EISDIR: return STATUS_FILE_IS_A_DIRECTORY;
    case ENOTEMPTY: return STATUS_DIRECTORY_NOT_EMPTY;
    case ENOSPC: return STATUS_DISK_FULL;
    case EBADF: return STATUS_INVALID_HANDLE;
    default: return STATUS_UNSUCCESSFUL;
    }
}

void SetIosb(IO_STATUS_BLOCK* iosb, NTSTATUS status, uint64_t info) {
    if (iosb == nullptr) return;
    iosb->Status = static_cast<uint64_t>(static_cast<int64_t>(status));
    iosb->Information = info;
}

bool NameFromAttributes(Kernel& k, const OBJECT_ATTRIBUTES* oa, std::string* nt_path) {
    if (oa == nullptr || oa->ObjectName == 0) return false;
    std::string name = Utf16ToUtf8(ReadUnicodeString(reinterpret_cast<const UNICODE_STRING*>(oa->ObjectName)));
    if (oa->RootDirectory != 0) {
        auto root = k.Get<FileObject>(oa->RootDirectory, ObjType::File);
        if (!root) return false;
        std::string base = root->nt_path;
        if (!base.empty() && base.back() != '\\') base += '\\';
        while (!name.empty() && name[0] == '\\') name.erase(0, 1);
        *nt_path = base + name;
    } else {
        *nt_path = name;
    }
    return true;
}

void PostCompletion(Kernel& k, SyscallArgs& a, IO_STATUS_BLOCK* iosb, int event_arg, int apc_arg) {
    // The operation completed synchronously. A caller that supplied an event or an APC
    // routine is told as it would be on completion.
    if (const HANDLE ev = a[event_arg]; ev != 0) {
        if (auto e = k.Get<EventObject>(ev, ObjType::Event)) {
            std::scoped_lock lock {k.WaitMutex()};
            e->signaled = true;
            k.Notify(*e);
        }
    }
    // A handle associated with a completion port: the packet goes there too, with the caller's context (the OVERLAPPED, as
    // ReadFile passes it -- none when the event's low bit asks for no packet). Windows queues it even for I/O that finished at once,
    // and titles wait for it (Forza Horizon 2's streaming thread sits in GetQueuedCompletionStatus).
    const uint64_t context = a[apc_arg + 1];
    if (auto f = k.Get<FileObject>(a[0], ObjType::File); f && context != 0 && !f->skip_completion_on_success) {
        if (auto port = std::static_pointer_cast<IoCompletionObject>(f->completion_port.lock())) {
            const IO_STATUS_BLOCK st = iosb != nullptr ? *iosb : IO_STATUS_BLOCK {};
            {
                std::scoped_lock lock {k.WaitMutex()};
                port->queue.push_back({f->completion_key, context, static_cast<int32_t>(st.Status), st.Information});
                port->signaled = true;
                k.NotifyAll();
            }
            WorkerFactoryPacketQueued(k, port);
        }
    }
}

// NtCreateFile / NtOpenFile -----------------------------------------------------------

NTSTATUS DoCreate(Kernel& k, NtThread&, HANDLE* out, uint32_t access, const OBJECT_ATTRIBUTES* oa, IO_STATUS_BLOCK* iosb,
                  uint32_t disposition, uint32_t options) {
    std::string nt_path;
    if (!NameFromAttributes(k, oa, &nt_path)) return STATUS_OBJECT_NAME_INVALID;
    // Devices the Era userland opens that have no file behind them.
    {
        std::string low = nt_path;
        for (auto& c : low) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (low == "\\device\\cng" || low == "\\device\\ksecdd" || low == "\\device\\null" || low == "\\??\\nul") {
            auto dev = std::make_shared<FileObject>();
            dev->is_device = true;
            dev->nt_path = nt_path;
            dev->host_path = "/dev/null";
            dev->fd = open("/dev/null", O_RDWR);
            *out = k.AddHandle(dev);
            SetIosb(iosb, STATUS_SUCCESS, FILE_OPENED);
            return STATUS_SUCCESS;
        }
    }
    if (logan::IsDevicePath(nt_path)) {
        auto dev = std::make_shared<FileObject>();
        dev->is_device = true;
        dev->is_logan = true;
        dev->nt_path = nt_path;
        dev->host_path = "/dev/null";
        dev->fd = open("/dev/null", O_RDWR);
        *out = k.AddHandle(dev);
        SetIosb(iosb, STATUS_SUCCESS, FILE_OPENED);
        return STATUS_SUCCESS;
    }
    std::string host;
    bool exists = false;
    if (!k.ResolvePath(nt_path, &host, &exists)) {
        UT_DEBUG("open: unmapped path %s", nt_path.c_str());
        SetIosb(iosb, STATUS_OBJECT_PATH_NOT_FOUND, 0);
        return STATUS_OBJECT_PATH_NOT_FOUND;
    }
    struct stat st {};
    const bool present = exists && stat(host.c_str(), &st) == 0;
    const bool is_dir_target = present && S_ISDIR(st.st_mode);
    const bool want_write = (access & (FILE_WRITE_DATA | FILE_APPEND_DATA | GENERIC_WRITE | GENERIC_ALL | DELETE_ACCESS)) != 0;

    uint64_t information = FILE_OPENED;
    if (present) {
        if (disposition == FILE_CREATE) { SetIosb(iosb, STATUS_OBJECT_NAME_COLLISION, FILE_EXISTS); return STATUS_OBJECT_NAME_COLLISION; }
        if ((options & FILE_DIRECTORY_FILE) && !is_dir_target) return STATUS_NOT_A_DIRECTORY;
        if ((options & FILE_NON_DIRECTORY_FILE) && is_dir_target) return STATUS_FILE_IS_A_DIRECTORY;
    } else {
        if (disposition == FILE_OPEN || disposition == FILE_OVERWRITE) {
            // Distinguish a missing leaf from a missing parent.
            const auto slash = host.rfind('/');
            struct stat pst {};
            const bool parent_ok = slash != std::string::npos && stat(host.substr(0, slash).c_str(), &pst) == 0;
            const NTSTATUS rc = parent_ok ? STATUS_OBJECT_NAME_NOT_FOUND : STATUS_OBJECT_PATH_NOT_FOUND;
            UT_DEBUG("open: %s not found (%s)", nt_path.c_str(), parent_ok ? "name" : "path");
            SetIosb(iosb, rc, FILE_DOES_NOT_EXIST);
            return rc;
        }
    }

    auto file = std::make_shared<FileObject>();
    file->host_path = host;
    file->nt_path = k.NtPathFromHost(host);
    if (file->nt_path.empty()) file->nt_path = nt_path;
    file->access = access;
    file->options = options;
    file->synchronous = (options & (FILE_SYNCHRONOUS_IO_ALERT | FILE_SYNCHRONOUS_IO_NONALERT)) != 0;
    file->delete_on_close = (options & FILE_DELETE_ON_CLOSE) != 0;

    if (!present) {
        if ((options & FILE_DIRECTORY_FILE) != 0) {
            std::error_code ec;
            std::filesystem::create_directories(host, ec);
            if (ec) return FromErrno(ec.value());
            file->is_dir = true;
            file->fd = open(host.c_str(), O_RDONLY);
        } else {
            file->fd = open(host.c_str(), O_RDWR | O_CREAT | O_EXCL, 0644);
            if (file->fd < 0) {
                SetIosb(iosb, FromErrno(errno), 0);
                return FromErrno(errno);
            }
        }
        information = FILE_CREATED;
    } else if (is_dir_target) {
        file->is_dir = true;
        file->fd = open(host.c_str(), O_RDONLY);
    } else {
        int flags = want_write ? O_RDWR : O_RDONLY;
        if (disposition == FILE_SUPERSEDE || disposition == FILE_OVERWRITE || disposition == FILE_OVERWRITE_IF) flags = O_RDWR | O_TRUNC;
        file->fd = open(host.c_str(), flags);
        if (file->fd < 0 && (flags & O_RDWR) && errno == EACCES && !want_write) file->fd = open(host.c_str(), O_RDONLY);
        if (file->fd < 0) {
            SetIosb(iosb, FromErrno(errno), 0);
            return FromErrno(errno);
        }
        if (disposition == FILE_SUPERSEDE) information = FILE_SUPERSEDED;
        else if (disposition == FILE_OVERWRITE || disposition == FILE_OVERWRITE_IF) information = FILE_OVERWRITTEN;
    }
    UT_DEBUG("open: %s (%s)", nt_path.c_str(), information == FILE_CREATED ? "created" : "opened");
    *out = k.AddHandle(file);
    SetIosb(iosb, STATUS_SUCCESS, information);
    return STATUS_SUCCESS;
}

/// UTSURO_FILELOG=<text>: logs every open whose path contains <text> (any case; "*" for all), with its result.
void LogOpen(Kernel& k, NtThread& t, const OBJECT_ATTRIBUTES* oa, NTSTATUS rc) {
    static const char* filter = std::getenv("UTSURO_FILELOG");
    if (filter == nullptr) return;
    std::string nt_path;
    if (!NameFromAttributes(k, oa, &nt_path)) return;
    std::string low = nt_path, want = filter;
    for (auto& c : low) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    for (auto& c : want) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (want != "*" && low.find(want) == std::string::npos) return;
    UT_INFO("file: [%llu] open %s -> %#x", (unsigned long long)t.tid_, nt_path.c_str(), rc);
}

NTSTATUS NtCreateFile(Kernel& k, NtThread& t, SyscallArgs& a) {
    const NTSTATUS rc = DoCreate(k, t, a.Ptr<HANDLE>(0), static_cast<uint32_t>(a[1]), a.Ptr<OBJECT_ATTRIBUTES>(2),
                                 a.Ptr<IO_STATUS_BLOCK>(3), static_cast<uint32_t>(a[7]), static_cast<uint32_t>(a[8]));
    LogOpen(k, t, a.Ptr<OBJECT_ATTRIBUTES>(2), rc);
    return rc;
}

NTSTATUS NtOpenFile(Kernel& k, NtThread& t, SyscallArgs& a) {
    const NTSTATUS rc = DoCreate(k, t, a.Ptr<HANDLE>(0), static_cast<uint32_t>(a[1]), a.Ptr<OBJECT_ATTRIBUTES>(2),
                                 a.Ptr<IO_STATUS_BLOCK>(3), FILE_OPEN, static_cast<uint32_t>(a[5]));
    LogOpen(k, t, a.Ptr<OBJECT_ATTRIBUTES>(2), rc);
    return rc;
}

// NtReadFile / NtWriteFile ------------------------------------------------------------

NTSTATUS NtReadFile(Kernel& k, NtThread&, SyscallArgs& a) {
    auto f = k.Get<FileObject>(a[0], ObjType::File);
    if (!f) return STATUS_INVALID_HANDLE;
    auto* iosb = a.Ptr<IO_STATUS_BLOCK>(4);
    auto* buf = a.Ptr<uint8_t>(5);
    const uint32_t len = static_cast<uint32_t>(a[6]);
    const auto* off = a.Ptr<int64_t>(7);
    if (f->is_dir) return STATUS_INVALID_PARAMETER;
    std::scoped_lock lock {f->mutex};
    ssize_t n;
    if (off != nullptr && *off >= 0) {
        n = pread(f->fd, buf, len, static_cast<off_t>(*off));
        if (n >= 0 && f->synchronous) f->position = *off + n;
    } else {
        n = pread(f->fd, buf, len, static_cast<off_t>(f->position));
        if (n >= 0) f->position += n;
    }
    UT_DEBUG("read: %s off %lld len %u -> %zd", f->nt_path.c_str(), (long long)(off ? *off : -1), len, n);
    if (n < 0) { SetIosb(iosb, FromErrno(errno), 0); return FromErrno(errno); }
    if (n == 0 && len != 0) { SetIosb(iosb, STATUS_END_OF_FILE, 0); return STATUS_END_OF_FILE; }
    SetIosb(iosb, STATUS_SUCCESS, static_cast<uint64_t>(n));
    PostCompletion(k, a, iosb, 1, 2);
    return STATUS_SUCCESS;
}

NTSTATUS NtWriteFile(Kernel& k, NtThread&, SyscallArgs& a) {
    auto f = k.Get<FileObject>(a[0], ObjType::File);
    if (!f) {
        // Console or unknown handles: swallow output so debug prints do not fail.
        return STATUS_INVALID_HANDLE;
    }
    auto* iosb = a.Ptr<IO_STATUS_BLOCK>(4);
    const auto* buf = a.Ptr<uint8_t>(5);
    const uint32_t len = static_cast<uint32_t>(a[6]);
    const auto* off = a.Ptr<int64_t>(7);
    std::scoped_lock lock {f->mutex};
    ssize_t n;
    if (off != nullptr && *off == -2) {
        struct stat st;
        fstat(f->fd, &st);
        n = pwrite(f->fd, buf, len, st.st_size);
        if (n >= 0) f->position = st.st_size + n;
    } else if (off != nullptr && *off >= 0) {
        n = pwrite(f->fd, buf, len, static_cast<off_t>(*off));
        if (n >= 0 && f->synchronous) f->position = *off + n;
    } else {
        n = pwrite(f->fd, buf, len, static_cast<off_t>(f->position));
        if (n >= 0) f->position += n;
    }
    if (n < 0) { SetIosb(iosb, FromErrno(errno), 0); return FromErrno(errno); }
    SetIosb(iosb, STATUS_SUCCESS, static_cast<uint64_t>(n));
    PostCompletion(k, a, iosb, 1, 2);
    return STATUS_SUCCESS;
}

// Information -------------------------------------------------------------------------

void FillBasic(uint8_t* p, const struct stat& st) {
    auto* q = reinterpret_cast<int64_t*>(p);
    q[0] = ToNtTime(st.st_birthtimespec);
    q[1] = ToNtTime(st.st_atimespec);
    q[2] = ToNtTime(st.st_mtimespec);
    q[3] = ToNtTime(st.st_ctimespec);
    *reinterpret_cast<uint32_t*>(p + 32) = AttributesOf(st);
    *reinterpret_cast<uint32_t*>(p + 36) = 0;
}

uint64_t Alloc(const struct stat& st) { return (static_cast<uint64_t>(st.st_size) + 4095) & ~4095ull; }

std::u16string BaseName(const std::string& nt_path) {
    const auto pos = nt_path.rfind('\\');
    return Utf8ToUtf16(pos == std::string::npos ? nt_path : nt_path.substr(pos + 1));
}

// NtQueryInformationFile(Handle, IOSB, Buffer, Length, Class)
NTSTATUS NtQueryInformationFile(Kernel& k, NtThread&, SyscallArgs& a) {
    auto f = k.Get<FileObject>(a[0], ObjType::File);
    auto* iosb = a.Ptr<IO_STATUS_BLOCK>(1);
    auto* buf = a.Ptr<uint8_t>(2);
    const uint32_t len = static_cast<uint32_t>(a[3]);
    const uint32_t cls = static_cast<uint32_t>(a[4]);
    if (!f) return STATUS_INVALID_HANDLE;
    struct stat st {};
    if (fstat(f->fd, &st) != 0) return FromErrno(errno);
    auto need = [&](uint32_t n) { return len >= n; };
    switch (cls) {
    case 4:  // FileBasicInformation
        if (!need(40)) return STATUS_INFO_LENGTH_MISMATCH;
        FillBasic(buf, st);
        SetIosb(iosb, STATUS_SUCCESS, 40);
        return STATUS_SUCCESS;
    case 5: {  // FileStandardInformation
        if (!need(24)) return STATUS_INFO_LENGTH_MISMATCH;
        auto* q = reinterpret_cast<int64_t*>(buf);
        q[0] = static_cast<int64_t>(Alloc(st));
        q[1] = st.st_size;
        *reinterpret_cast<uint32_t*>(buf + 16) = 1;
        buf[20] = f->delete_on_close ? 1 : 0;
        buf[21] = S_ISDIR(st.st_mode) ? 1 : 0;
        SetIosb(iosb, STATUS_SUCCESS, 24);
        return STATUS_SUCCESS;
    }
    case 6:  // FileInternalInformation
        if (!need(8)) return STATUS_INFO_LENGTH_MISMATCH;
        *reinterpret_cast<uint64_t*>(buf) = st.st_ino;
        SetIosb(iosb, STATUS_SUCCESS, 8);
        return STATUS_SUCCESS;
    case 7:  // FileEaInformation
        if (!need(4)) return STATUS_INFO_LENGTH_MISMATCH;
        *reinterpret_cast<uint32_t*>(buf) = 0;
        SetIosb(iosb, STATUS_SUCCESS, 4);
        return STATUS_SUCCESS;
    case 8:  // FileAccessInformation
        if (!need(4)) return STATUS_INFO_LENGTH_MISMATCH;
        *reinterpret_cast<uint32_t*>(buf) = f->access;
        SetIosb(iosb, STATUS_SUCCESS, 4);
        return STATUS_SUCCESS;
    case 9: {  // FileNameInformation: name relative to the volume
        std::string path = f->nt_path.size() > 2 ? f->nt_path.substr(2) : "\\";
        const std::u16string name = Utf8ToUtf16(path);
        const uint32_t total = 4 + static_cast<uint32_t>(name.size() * 2);
        if (!need(4)) return STATUS_INFO_LENGTH_MISMATCH;
        *reinterpret_cast<uint32_t*>(buf) = static_cast<uint32_t>(name.size() * 2);
        const uint32_t room = len - 4;
        std::memcpy(buf + 4, name.data(), std::min<size_t>(room, name.size() * 2));
        SetIosb(iosb, room < name.size() * 2 ? STATUS_BUFFER_OVERFLOW : STATUS_SUCCESS, std::min(total, len));
        return room < name.size() * 2 ? STATUS_BUFFER_OVERFLOW : STATUS_SUCCESS;
    }
    case 14:  // FilePositionInformation
        if (!need(8)) return STATUS_INFO_LENGTH_MISMATCH;
        *reinterpret_cast<int64_t*>(buf) = f->position;
        SetIosb(iosb, STATUS_SUCCESS, 8);
        return STATUS_SUCCESS;
    case 16:  // FileModeInformation
    case 17:  // FileAlignmentInformation
        if (!need(4)) return STATUS_INFO_LENGTH_MISMATCH;
        *reinterpret_cast<uint32_t*>(buf) = cls == 16 ? (f->synchronous ? 0x20 : 0) : 0;
        SetIosb(iosb, STATUS_SUCCESS, 4);
        return STATUS_SUCCESS;
    case 18: {  // FileAllInformation
        const std::u16string name = Utf8ToUtf16(f->nt_path.size() > 2 ? f->nt_path.substr(2) : "\\");
        const uint32_t fixed = 40 + 24 + 8 + 4 + 4 + 8 + 4 + 4 + 4;  // up to the name length
        if (!need(fixed)) return STATUS_INFO_LENGTH_MISMATCH;
        std::memset(buf, 0, std::min<uint32_t>(len, fixed));
        FillBasic(buf, st);
        auto* q = reinterpret_cast<int64_t*>(buf + 40);
        q[0] = static_cast<int64_t>(Alloc(st));
        q[1] = st.st_size;
        *reinterpret_cast<uint32_t*>(buf + 56) = 1;
        buf[60] = 0;
        buf[61] = S_ISDIR(st.st_mode) ? 1 : 0;
        *reinterpret_cast<uint64_t*>(buf + 64) = st.st_ino;
        *reinterpret_cast<uint32_t*>(buf + 76) = f->access;
        *reinterpret_cast<int64_t*>(buf + 80) = f->position;
        *reinterpret_cast<uint32_t*>(buf + 88) = f->synchronous ? 0x20 : 0;
        *reinterpret_cast<uint32_t*>(buf + 92) = 0;
        *reinterpret_cast<uint32_t*>(buf + 96) = static_cast<uint32_t>(name.size() * 2);
        const uint32_t room = len - fixed;
        std::memcpy(buf + fixed, name.data(), std::min<size_t>(room, name.size() * 2));
        const bool over = room < name.size() * 2;
        SetIosb(iosb, over ? STATUS_BUFFER_OVERFLOW : STATUS_SUCCESS, std::min<uint64_t>(len, fixed + name.size() * 2));
        return over ? STATUS_BUFFER_OVERFLOW : STATUS_SUCCESS;
    }
    case 34: {  // FileNetworkOpenInformation
        if (!need(56)) return STATUS_INFO_LENGTH_MISMATCH;
        auto* q = reinterpret_cast<int64_t*>(buf);
        q[0] = ToNtTime(st.st_birthtimespec);
        q[1] = ToNtTime(st.st_atimespec);
        q[2] = ToNtTime(st.st_mtimespec);
        q[3] = ToNtTime(st.st_ctimespec);
        q[4] = static_cast<int64_t>(Alloc(st));
        q[5] = st.st_size;
        *reinterpret_cast<uint32_t*>(buf + 48) = AttributesOf(st);
        *reinterpret_cast<uint32_t*>(buf + 52) = 0;
        SetIosb(iosb, STATUS_SUCCESS, 56);
        return STATUS_SUCCESS;
    }
    case 35:  // FileAttributeTagInformation
        if (!need(8)) return STATUS_INFO_LENGTH_MISMATCH;
        *reinterpret_cast<uint32_t*>(buf) = AttributesOf(st);
        *reinterpret_cast<uint32_t*>(buf + 4) = 0;
        SetIosb(iosb, STATUS_SUCCESS, 8);
        return STATUS_SUCCESS;
    case 22:  // FileStreamInformation: just the unnamed data stream
        if (!need(40)) return STATUS_INFO_LENGTH_MISMATCH;
        std::memset(buf, 0, 40);
        {
            const std::u16string n = u"::$DATA";
            *reinterpret_cast<uint32_t*>(buf + 4) = static_cast<uint32_t>(n.size() * 2);
            *reinterpret_cast<int64_t*>(buf + 8) = st.st_size;
            *reinterpret_cast<int64_t*>(buf + 16) = static_cast<int64_t>(Alloc(st));
            if (len >= 24 + n.size() * 2) std::memcpy(buf + 24, n.data(), n.size() * 2);
        }
        SetIosb(iosb, STATUS_SUCCESS, 24 + 14);
        return STATUS_SUCCESS;
    default:
        UT_DEBUG("NtQueryInformationFile class %u not handled", cls);
        return STATUS_INVALID_INFO_CLASS;
    }
}

// NtSetInformationFile(Handle, IOSB, Buffer, Length, Class)
NTSTATUS NtSetInformationFile(Kernel& k, NtThread&, SyscallArgs& a) {
    auto f = k.Get<FileObject>(a[0], ObjType::File);
    auto* iosb = a.Ptr<IO_STATUS_BLOCK>(1);
    auto* buf = a.Ptr<uint8_t>(2);
    const uint32_t len = static_cast<uint32_t>(a[3]);
    const uint32_t cls = static_cast<uint32_t>(a[4]);
    if (!f) return STATUS_INVALID_HANDLE;
    switch (cls) {
    case 14:  // FilePositionInformation
        f->position = *reinterpret_cast<int64_t*>(buf);
        SetIosb(iosb, STATUS_SUCCESS, 0);
        return STATUS_SUCCESS;
    case 13:  // FileDispositionInformation
    case 64:  // FileDispositionInformationEx
        f->delete_on_close = buf[0] != 0;
        SetIosb(iosb, STATUS_SUCCESS, 0);
        return STATUS_SUCCESS;
    case 20:  // FileEndOfFileInformation
    case 19:  // FileAllocationInformation
        if (cls == 20 && ftruncate(f->fd, *reinterpret_cast<int64_t*>(buf)) != 0) return FromErrno(errno);
        SetIosb(iosb, STATUS_SUCCESS, 0);
        return STATUS_SUCCESS;
    case 4:  // FileBasicInformation: times and attributes are not tracked
    case 16: // FileModeInformation
        SetIosb(iosb, STATUS_SUCCESS, 0);
        return STATUS_SUCCESS;
    case 10: {  // FileRenameInformation: ReplaceIfExists(1+pad) RootDirectory(8) FileNameLength(4) FileName
        const uint64_t root = *reinterpret_cast<uint64_t*>(buf + 8);
        const uint32_t nlen = *reinterpret_cast<uint32_t*>(buf + 16);
        std::string target = Utf16ToUtf8(std::u16string(reinterpret_cast<char16_t*>(buf + 20), nlen / 2));
        if (root != 0) {
            auto r = k.Get<FileObject>(root, ObjType::File);
            if (!r) return STATUS_INVALID_HANDLE;
            target = r->nt_path + "\\" + target;
        }
        std::string host;
        bool exists = false;
        if (!k.ResolvePath(target, &host, &exists)) return STATUS_OBJECT_PATH_NOT_FOUND;
        if (exists && buf[0] == 0) return STATUS_OBJECT_NAME_COLLISION;
        if (rename(f->host_path.c_str(), host.c_str()) != 0) return FromErrno(errno);
        f->host_path = host;
        f->nt_path = k.NtPathFromHost(host);
        SetIosb(iosb, STATUS_SUCCESS, 0);
        return STATUS_SUCCESS;
    }
    case 30: {  // FileCompletionInformation {HANDLE Port; PVOID Key}: completions on this handle go to the port
        if (len < 16) return STATUS_INFO_LENGTH_MISMATCH;
        uint64_t port_handle = 0, key = 0;
        std::memcpy(&port_handle, buf, 8);
        std::memcpy(&key, buf + 8, 8);
        auto port = k.Get<IoCompletionObject>(port_handle, ObjType::IoCompletion);
        if (!port) return STATUS_INVALID_HANDLE;
        f->completion_port = port;
        f->completion_key = key;
        static int shown = 0;
        if (shown++ < 12) UT_INFO("file: %s completes to port %#llx (key %#llx)", f->nt_path.c_str(), (unsigned long long)port_handle, (unsigned long long)key);
        SetIosb(iosb, STATUS_SUCCESS, 0);
        return STATUS_SUCCESS;
    }
    case 41:  // FileIoCompletionNotificationInformation: FILE_SKIP_COMPLETION_PORT_ON_SUCCESS (1), FILE_SKIP_SET_EVENT_ON_HANDLE (2)
        if (len < 4) return STATUS_INFO_LENGTH_MISMATCH;
        f->skip_completion_on_success = (buf[0] & 1) != 0;
        SetIosb(iosb, STATUS_SUCCESS, 0);
        return STATUS_SUCCESS;
    default:
        UT_DEBUG("NtSetInformationFile class %u (len %u) not handled", cls, len);
        return STATUS_INVALID_INFO_CLASS;
    }
}

// Directories -------------------------------------------------------------------------

bool WildMatch(const std::u16string& pattern, const std::u16string& name) {
    // DOS-style: * ? and the NT forms < (zero or more not-dot... treated as *), > (one char), " (dot).
    size_t p = 0, n = 0, star = std::u16string::npos, mark = 0;
    auto lower = [](char16_t c) { return (c >= u'A' && c <= u'Z') ? static_cast<char16_t>(c + 32) : c; };
    while (n < name.size()) {
        if (p < pattern.size() && (pattern[p] == u'*' || pattern[p] == u'<')) { star = p++; mark = n; continue; }
        if (p < pattern.size() && (pattern[p] == u'?' || pattern[p] == u'>' || (pattern[p] == u'"' && name[n] == u'.') || lower(pattern[p]) == lower(name[n]))) { ++p; ++n; continue; }
        if (star != std::u16string::npos) { p = star + 1; n = ++mark; continue; }
        return false;
    }
    while (p < pattern.size() && (pattern[p] == u'*' || pattern[p] == u'<' || pattern[p] == u'"')) ++p;
    return p == pattern.size();
}

// NtQueryDirectoryFile(Handle, Event, ApcRoutine, ApcContext, IOSB, Buffer, Length, Class,
//                      ReturnSingleEntry, FileName, RestartScan)
NTSTATUS NtQueryDirectoryFile(Kernel& k, NtThread&, SyscallArgs& a) {
    auto f = k.Get<FileObject>(a[0], ObjType::File);
    if (!f) return STATUS_INVALID_HANDLE;
    auto* iosb = a.Ptr<IO_STATUS_BLOCK>(4);
    auto* buf = a.Ptr<uint8_t>(5);
    const uint32_t len = static_cast<uint32_t>(a[6]);
    const uint32_t cls = static_cast<uint32_t>(a[7]);
    const bool single = (a[8] & 0xFF) != 0;
    const auto* fname = a.Ptr<UNICODE_STRING>(9);
    const bool restart = (a[10] & 0xFF) != 0;
    if (!f->is_dir) return STATUS_INVALID_PARAMETER;

    std::scoped_lock lock {f->mutex};
    if (!f->dir_loaded || restart) {
        f->dir_entries.clear();
        f->dir_entries.push_back(".");
        f->dir_entries.push_back("..");
        if (DIR* d = opendir(f->host_path.c_str())) {
            while (auto* e = readdir(d)) {
                if (!std::strcmp(e->d_name, ".") || !std::strcmp(e->d_name, "..")) continue;
                f->dir_entries.push_back(e->d_name);
            }
            closedir(d);
        }
        f->dir_index = 0;
        f->dir_loaded = true;
        if (fname != nullptr && fname->Buffer != 0) f->dir_pattern = Utf16ToUtf8(ReadUnicodeString(fname));
        else if (restart) f->dir_pattern.clear();
    } else if (fname != nullptr && fname->Buffer != 0 && f->dir_pattern.empty()) {
        f->dir_pattern = Utf16ToUtf8(ReadUnicodeString(fname));
    }
    const std::u16string pattern = Utf8ToUtf16(f->dir_pattern.empty() ? "*" : f->dir_pattern);

    uint32_t header, name_off;
    switch (cls) {
    case 1: header = 64; name_off = 64; break;                 // FileDirectoryInformation
    case 2: header = 68; name_off = 68; break;                 // FileFullDirectoryInformation
    case 3: header = 94; name_off = 94; break;                 // FileBothDirectoryInformation
    case 12: header = 12; name_off = 12; break;                // FileNamesInformation
    case 37: header = 104; name_off = 104; break;              // FileIdBothDirectoryInformation
    case 38: header = 80; name_off = 80; break;                // FileIdFullDirectoryInformation
    default:
        UT_DEBUG("NtQueryDirectoryFile class %u not handled", cls);
        return STATUS_INVALID_INFO_CLASS;
    }

    uint32_t used = 0;
    uint32_t last_entry = 0;
    bool wrote = false;
    while (f->dir_index < f->dir_entries.size()) {
        const std::string& name8 = f->dir_entries[f->dir_index];
        const std::u16string name = Utf8ToUtf16(name8);
        if (!WildMatch(pattern, name)) { ++f->dir_index; continue; }
        const uint32_t entry_size = (name_off + static_cast<uint32_t>(name.size() * 2) + 7) & ~7u;
        if (used + entry_size > len) {
            if (!wrote) return STATUS_BUFFER_OVERFLOW;
            break;
        }
        struct stat st {};
        const std::string full = f->host_path + "/" + name8;
        if (lstat(full.c_str(), &st) != 0) { ++f->dir_index; continue; }
        if (S_ISLNK(st.st_mode)) stat(full.c_str(), &st);
        uint8_t* e = buf + used;
        std::memset(e, 0, entry_size);
        *reinterpret_cast<uint32_t*>(e) = 0;  // NextEntryOffset, patched below
        if (cls == 12) {
            *reinterpret_cast<uint32_t*>(e + 8) = static_cast<uint32_t>(name.size() * 2);
        } else {
            *reinterpret_cast<uint32_t*>(e + 4) = static_cast<uint32_t>(f->dir_index);
            auto* q = reinterpret_cast<int64_t*>(e + 8);
            q[0] = ToNtTime(st.st_birthtimespec);
            q[1] = ToNtTime(st.st_atimespec);
            q[2] = ToNtTime(st.st_mtimespec);
            q[3] = ToNtTime(st.st_ctimespec);
            *reinterpret_cast<int64_t*>(e + 40) = st.st_size;
            *reinterpret_cast<int64_t*>(e + 48) = static_cast<int64_t>(Alloc(st));
            *reinterpret_cast<uint32_t*>(e + 56) = AttributesOf(st);
            *reinterpret_cast<uint32_t*>(e + 60) = static_cast<uint32_t>(name.size() * 2);
            if (cls == 37) *reinterpret_cast<uint64_t*>(e + 96) = st.st_ino;
            if (cls == 38) *reinterpret_cast<uint64_t*>(e + 72) = st.st_ino;
        }
        std::memcpy(e + name_off, name.data(), name.size() * 2);
        if (wrote) *reinterpret_cast<uint32_t*>(buf + last_entry) = used - last_entry;
        last_entry = used;
        used += entry_size;
        wrote = true;
        ++f->dir_index;
        if (single) break;
    }
    if (!wrote) {
        SetIosb(iosb, STATUS_NO_MORE_FILES, 0);
        PostCompletion(k, a, iosb, 1, 2);
        return STATUS_NO_MORE_FILES;
    }
    SetIosb(iosb, STATUS_SUCCESS, used);
    PostCompletion(k, a, iosb, 1, 2);
    return STATUS_SUCCESS;
}

// NtQueryAttributesFile(POBJECT_ATTRIBUTES, PFILE_BASIC_INFORMATION)
NTSTATUS NtQueryAttributesFile(Kernel& k, NtThread&, SyscallArgs& a) {
    std::string nt_path;
    if (!NameFromAttributes(k, a.Ptr<OBJECT_ATTRIBUTES>(0), &nt_path)) return STATUS_OBJECT_NAME_INVALID;
    std::string host;
    bool exists = false;
    if (!k.ResolvePath(nt_path, &host, &exists) || !exists) {
        UT_DEBUG("query attributes: %s not found", nt_path.c_str());
        return STATUS_OBJECT_NAME_NOT_FOUND;
    }
    struct stat st;
    if (stat(host.c_str(), &st) != 0) return FromErrno(errno);
    FillBasic(a.Ptr<uint8_t>(1), st);
    return STATUS_SUCCESS;
}

NTSTATUS NtQueryFullAttributesFile(Kernel& k, NtThread&, SyscallArgs& a) {
    std::string nt_path;
    if (!NameFromAttributes(k, a.Ptr<OBJECT_ATTRIBUTES>(0), &nt_path)) return STATUS_OBJECT_NAME_INVALID;
    std::string host;
    bool exists = false;
    if (!k.ResolvePath(nt_path, &host, &exists) || !exists) {
        UT_DEBUG("query attributes: %s not found", nt_path.c_str());
        return STATUS_OBJECT_NAME_NOT_FOUND;
    }
    struct stat st;
    if (stat(host.c_str(), &st) != 0) return FromErrno(errno);
    auto* buf = a.Ptr<uint8_t>(1);
    auto* q = reinterpret_cast<int64_t*>(buf);
    q[0] = ToNtTime(st.st_birthtimespec);
    q[1] = ToNtTime(st.st_atimespec);
    q[2] = ToNtTime(st.st_mtimespec);
    q[3] = ToNtTime(st.st_ctimespec);
    q[4] = static_cast<int64_t>(Alloc(st));
    q[5] = st.st_size;
    *reinterpret_cast<uint32_t*>(buf + 48) = AttributesOf(st);
    *reinterpret_cast<uint32_t*>(buf + 52) = 0;
    return STATUS_SUCCESS;
}

// NtQueryVolumeInformationFile(Handle, IOSB, Buffer, Length, Class)
NTSTATUS NtQueryVolumeInformationFile(Kernel& k, NtThread&, SyscallArgs& a) {
    auto* iosb = a.Ptr<IO_STATUS_BLOCK>(1);
    auto* buf = a.Ptr<uint8_t>(2);
    const uint32_t len = static_cast<uint32_t>(a[3]);
    const uint32_t cls = static_cast<uint32_t>(a[4]);
    auto f = k.Get<FileObject>(a[0], ObjType::File);
    if (!f) return STATUS_INVALID_HANDLE;
    struct statfs sf {};
    statfs(f->host_path.c_str(), &sf);
    switch (cls) {
    case 4:  // FileFsDeviceInformation
        if (len < 8) return STATUS_INFO_LENGTH_MISMATCH;
        *reinterpret_cast<uint32_t*>(buf) = 7;      // FILE_DEVICE_DISK
        *reinterpret_cast<uint32_t*>(buf + 4) = 0;
        SetIosb(iosb, STATUS_SUCCESS, 8);
        return STATUS_SUCCESS;
    case 5: {  // FileFsAttributeInformation
        const std::u16string fs = u"NTFS";
        const uint32_t total = 12 + static_cast<uint32_t>(fs.size() * 2);
        if (len < total) return STATUS_BUFFER_OVERFLOW;
        *reinterpret_cast<uint32_t*>(buf) = 0x00000007 | 0x00008000;  // case preserving+sensitive search, unicode, ...
        *reinterpret_cast<uint32_t*>(buf + 4) = 255;
        *reinterpret_cast<uint32_t*>(buf + 8) = static_cast<uint32_t>(fs.size() * 2);
        std::memcpy(buf + 12, fs.data(), fs.size() * 2);
        SetIosb(iosb, STATUS_SUCCESS, total);
        return STATUS_SUCCESS;
    }
    case 3:    // FileFsSizeInformation
    case 7: {  // FileFsFullSizeInformation
        const uint32_t need = cls == 3 ? 24 : 32;
        if (len < need) return STATUS_INFO_LENGTH_MISMATCH;
        auto* q = reinterpret_cast<int64_t*>(buf);
        q[0] = static_cast<int64_t>(sf.f_blocks);
        q[1] = static_cast<int64_t>(sf.f_bavail);
        if (cls == 7) { q[2] = static_cast<int64_t>(sf.f_bfree); *reinterpret_cast<uint32_t*>(buf + 24) = 8; *reinterpret_cast<uint32_t*>(buf + 28) = static_cast<uint32_t>(sf.f_bsize / 8); }
        else { *reinterpret_cast<uint32_t*>(buf + 16) = 8; *reinterpret_cast<uint32_t*>(buf + 20) = static_cast<uint32_t>(sf.f_bsize / 8); }
        SetIosb(iosb, STATUS_SUCCESS, need);
        return STATUS_SUCCESS;
    }
    case 1: {  // FileFsVolumeInformation
        const std::u16string label = u"Utsuro";
        const uint32_t total = 18 + static_cast<uint32_t>(label.size() * 2);
        if (len < total) return STATUS_BUFFER_OVERFLOW;
        std::memset(buf, 0, total);
        *reinterpret_cast<uint32_t*>(buf + 8) = 0x1234ABCD;
        *reinterpret_cast<uint32_t*>(buf + 12) = static_cast<uint32_t>(label.size() * 2);
        std::memcpy(buf + 18, label.data(), label.size() * 2);
        SetIosb(iosb, STATUS_SUCCESS, total);
        return STATUS_SUCCESS;
    }
    case 11: {  // FileFsSectorSizeInformation
        if (len < 28) return STATUS_INFO_LENGTH_MISMATCH;
        std::memset(buf, 0, 28);
        *reinterpret_cast<uint32_t*>(buf + 4) = 4096;
        *reinterpret_cast<uint32_t*>(buf + 8) = 4096;
        *reinterpret_cast<uint32_t*>(buf + 12) = 4096;
        *reinterpret_cast<uint32_t*>(buf + 16) = 4096;
        SetIosb(iosb, STATUS_SUCCESS, 28);
        return STATUS_SUCCESS;
    }
    default:
        UT_DEBUG("NtQueryVolumeInformationFile class %u not handled", cls);
        return STATUS_INVALID_INFO_CLASS;
    }
}

NTSTATUS NtFlushBuffersFile(Kernel& k, NtThread&, SyscallArgs& a) {
    auto f = k.Get<FileObject>(a[0], ObjType::File);
    if (!f) return STATUS_INVALID_HANDLE;
    fsync(f->fd);
    SetIosb(a.Ptr<IO_STATUS_BLOCK>(1), STATUS_SUCCESS, 0);
    return STATUS_SUCCESS;
}

// NtDeleteFile(POBJECT_ATTRIBUTES)
NTSTATUS NtDeleteFile(Kernel& k, NtThread&, SyscallArgs& a) {
    std::string nt_path;
    if (!NameFromAttributes(k, a.Ptr<OBJECT_ATTRIBUTES>(0), &nt_path)) return STATUS_OBJECT_NAME_INVALID;
    std::string host;
    bool exists = false;
    if (!k.ResolvePath(nt_path, &host, &exists) || !exists) return STATUS_OBJECT_NAME_NOT_FOUND;
    struct stat st;
    if (stat(host.c_str(), &st) != 0) return FromErrno(errno);
    const int rc = S_ISDIR(st.st_mode) ? rmdir(host.c_str()) : unlink(host.c_str());
    return rc == 0 ? STATUS_SUCCESS : FromErrno(errno);
}

NTSTATUS Unsupported(Kernel&, NtThread&, SyscallArgs&) { return STATUS_NOT_SUPPORTED; }

// NtDeviceIoControlFile(Handle, Event, ApcRoutine, ApcContext, IOSB, IoControlCode, InBuf, InLen, OutBuf, OutLen):
// the CNG device answers requests for random bytes; anything else is logged and refused.
NTSTATUS NtDeviceIoControlFile(Kernel& k, NtThread&, SyscallArgs& a) {
    auto f = k.Get<FileObject>(a[0], ObjType::File);
    if (!f) return STATUS_INVALID_HANDLE;
    auto* iosb = a.Ptr<IO_STATUS_BLOCK>(4);
    const uint32_t code = static_cast<uint32_t>(a[5]);
    auto* out = a.Ptr<uint8_t>(8);
    const uint32_t out_len = static_cast<uint32_t>(a[9]);
    if (f->is_logan) {
        uint64_t returned = 0;
        const NTSTATUS status = logan::Ioctl(k, code, a.Ptr<uint8_t>(6), static_cast<uint32_t>(a[7]), out, out_len, &returned);
        SetIosb(iosb, status, returned);
        return status;
    }
    if (f->is_device) {
        UT_DEBUG("DeviceIoControl %s code %#x in %u out %u", f->nt_path.c_str(), code, (uint32_t)a[7], out_len);
        if (const auto* in = a.Ptr<uint8_t>(6)) {
            std::string hex;
            for (uint32_t i = 0; i < std::min<uint32_t>(static_cast<uint32_t>(a[7]), 112); ++i) { char b[4]; std::snprintf(b, sizeof b, "%02x", in[i]); hex += b; if (i % 8 == 7) hex += ' '; }
            UT_DEBUG("  in: %s", hex.c_str());
        }
        if (out != nullptr && out_len != 0) {
            // 0x390402 is a CNG call whose 8-byte answer starts with a status: zero is success.
            for (uint32_t i = 0; i < out_len; ++i) out[i] = code == 0x390402 ? 0 : static_cast<uint8_t>(arc4random());
            SetIosb(iosb, STATUS_SUCCESS, out_len);
        } else {
            SetIosb(iosb, STATUS_SUCCESS, 0);
        }
        return STATUS_SUCCESS;
    }
    return STATUS_NOT_SUPPORTED;
}

NTSTATUS Succeed(Kernel&, NtThread&, SyscallArgs&) { return STATUS_SUCCESS; }

// NtNotifyChangeDirectoryFile never completes here.
NTSTATUS NtNotifyChangeDirectoryFile(Kernel&, NtThread&, SyscallArgs& a) {
    SetIosb(a.Ptr<IO_STATUS_BLOCK>(4), STATUS_PENDING, 0);
    return STATUS_PENDING;
}

NTSTATUS NtLockFile(Kernel&, NtThread&, SyscallArgs& a) {
    SetIosb(a.Ptr<IO_STATUS_BLOCK>(4), STATUS_SUCCESS, 0);
    return STATUS_SUCCESS;
}

} // namespace

void RegisterFileSyscalls(std::unordered_map<std::string, SyscallFn>& t) {
    t["NtCreateFile"] = NtCreateFile;
    t["NtOpenFile"] = NtOpenFile;
    t["NtReadFile"] = NtReadFile;
    t["NtWriteFile"] = NtWriteFile;
    t["NtQueryInformationFile"] = NtQueryInformationFile;
    t["NtSetInformationFile"] = NtSetInformationFile;
    t["NtQueryDirectoryFile"] = NtQueryDirectoryFile;
    t["NtQueryAttributesFile"] = NtQueryAttributesFile;
    t["NtQueryFullAttributesFile"] = NtQueryFullAttributesFile;
    t["NtQueryVolumeInformationFile"] = NtQueryVolumeInformationFile;
    t["NtFlushBuffersFile"] = NtFlushBuffersFile;
    t["NtFlushBuffersFileEx"] = NtFlushBuffersFile;
    t["NtDeleteFile"] = NtDeleteFile;
    t["NtNotifyChangeDirectoryFile"] = NtNotifyChangeDirectoryFile;
    t["NtLockFile"] = NtLockFile;
    t["NtUnlockFile"] = NtLockFile;
    t["NtCancelIoFile"] = Succeed;
    t["NtCancelIoFileEx"] = Succeed;
    t["NtCancelSynchronousIoFile"] = Succeed;
    t["NtFsControlFile"] = Unsupported;
    t["NtDeviceIoControlFile"] = NtDeviceIoControlFile;
}

} // namespace utsuro::nt
