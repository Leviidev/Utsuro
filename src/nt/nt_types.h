// nt_types.h -- the NT constants and structures the kernel half has to speak.
//
// Only what the Xbox Era userland actually touches. Offsets are x64 Windows 10 layouts.
#pragma once

#include <cstddef>
#include <cstdint>

namespace utsuro::nt {

using NTSTATUS = int32_t;
using HANDLE = uint64_t;

constexpr NTSTATUS STATUS_SUCCESS = 0;
constexpr NTSTATUS STATUS_WAIT_0 = 0;
constexpr NTSTATUS STATUS_ABANDONED_WAIT_0 = 0x80;
constexpr NTSTATUS STATUS_USER_APC = 0xC0;
constexpr NTSTATUS STATUS_TIMEOUT = 0x102;
constexpr NTSTATUS STATUS_PENDING = 0x103;
constexpr NTSTATUS STATUS_IMAGE_NOT_AT_BASE = 0x40000003;
constexpr NTSTATUS STATUS_BUFFER_OVERFLOW = static_cast<NTSTATUS>(0x80000005);
constexpr NTSTATUS STATUS_NO_MORE_FILES = static_cast<NTSTATUS>(0x80000006);
constexpr NTSTATUS STATUS_NO_MORE_ENTRIES = static_cast<NTSTATUS>(0x8000001A);
constexpr NTSTATUS STATUS_UNSUCCESSFUL = static_cast<NTSTATUS>(0xC0000001);
constexpr NTSTATUS STATUS_NOT_IMPLEMENTED = static_cast<NTSTATUS>(0xC0000002);
constexpr NTSTATUS STATUS_INVALID_INFO_CLASS = static_cast<NTSTATUS>(0xC0000003);
constexpr NTSTATUS STATUS_INFO_LENGTH_MISMATCH = static_cast<NTSTATUS>(0xC0000004);
constexpr NTSTATUS STATUS_ACCESS_VIOLATION = static_cast<NTSTATUS>(0xC0000005);
constexpr NTSTATUS STATUS_INVALID_HANDLE = static_cast<NTSTATUS>(0xC0000008);
constexpr NTSTATUS STATUS_INVALID_PARAMETER = static_cast<NTSTATUS>(0xC000000D);
constexpr NTSTATUS STATUS_NO_SUCH_FILE = static_cast<NTSTATUS>(0xC000000F);
constexpr NTSTATUS STATUS_END_OF_FILE = static_cast<NTSTATUS>(0xC0000011);
constexpr NTSTATUS STATUS_NO_MEMORY = static_cast<NTSTATUS>(0xC0000017);
constexpr NTSTATUS STATUS_CONFLICTING_ADDRESSES = static_cast<NTSTATUS>(0xC0000018);
constexpr NTSTATUS STATUS_ACCESS_DENIED = static_cast<NTSTATUS>(0xC0000022);
constexpr NTSTATUS STATUS_BUFFER_TOO_SMALL = static_cast<NTSTATUS>(0xC0000023);
constexpr NTSTATUS STATUS_OBJECT_TYPE_MISMATCH = static_cast<NTSTATUS>(0xC0000024);
constexpr NTSTATUS STATUS_OBJECT_NAME_INVALID = static_cast<NTSTATUS>(0xC0000033);
constexpr NTSTATUS STATUS_OBJECT_NAME_NOT_FOUND = static_cast<NTSTATUS>(0xC0000034);
constexpr NTSTATUS STATUS_OBJECT_NAME_COLLISION = static_cast<NTSTATUS>(0xC0000035);
constexpr NTSTATUS STATUS_OBJECT_PATH_NOT_FOUND = static_cast<NTSTATUS>(0xC000003A);
constexpr NTSTATUS STATUS_SHARING_VIOLATION = static_cast<NTSTATUS>(0xC0000043);
constexpr NTSTATUS STATUS_MUTANT_NOT_OWNED = static_cast<NTSTATUS>(0xC0000046);
constexpr NTSTATUS STATUS_SEMAPHORE_LIMIT_EXCEEDED = static_cast<NTSTATUS>(0xC0000047);
constexpr NTSTATUS STATUS_PROCESS_IS_TERMINATING = static_cast<NTSTATUS>(0xC000010A);
constexpr NTSTATUS STATUS_NOT_SUPPORTED = static_cast<NTSTATUS>(0xC00000BB);
constexpr NTSTATUS STATUS_INVALID_IMAGE_FORMAT = static_cast<NTSTATUS>(0xC000007B);
constexpr NTSTATUS STATUS_NOT_MAPPED_VIEW = static_cast<NTSTATUS>(0xC0000019);
constexpr NTSTATUS STATUS_UNABLE_TO_FREE_VM = static_cast<NTSTATUS>(0xC000001A);
constexpr NTSTATUS STATUS_FREE_VM_NOT_AT_BASE = static_cast<NTSTATUS>(0xC000009F);
constexpr NTSTATUS STATUS_NOT_COMMITTED = static_cast<NTSTATUS>(0xC000002D);
constexpr NTSTATUS STATUS_INVALID_ADDRESS = static_cast<NTSTATUS>(0xC0000141);
constexpr NTSTATUS STATUS_DLL_NOT_FOUND = static_cast<NTSTATUS>(0xC0000135);
constexpr NTSTATUS STATUS_NOT_FOUND = static_cast<NTSTATUS>(0xC0000225);
constexpr NTSTATUS STATUS_NO_YIELD_PERFORMED = 0x40000024;
constexpr NTSTATUS STATUS_DIRECTORY_NOT_EMPTY = static_cast<NTSTATUS>(0xC0000101);
constexpr NTSTATUS STATUS_NOT_A_DIRECTORY = static_cast<NTSTATUS>(0xC0000103);
constexpr NTSTATUS STATUS_FILE_IS_A_DIRECTORY = static_cast<NTSTATUS>(0xC00000BA);
constexpr NTSTATUS STATUS_DISK_FULL = static_cast<NTSTATUS>(0xC000007F);
constexpr NTSTATUS STATUS_UNWIND = static_cast<NTSTATUS>(0xC0000027);
constexpr NTSTATUS STATUS_BREAKPOINT = static_cast<NTSTATUS>(0x80000003);
constexpr NTSTATUS STATUS_PRIVILEGE_NOT_HELD = static_cast<NTSTATUS>(0xC0000061);
constexpr NTSTATUS STATUS_THREAD_IS_TERMINATING = static_cast<NTSTATUS>(0xC000004B);
constexpr NTSTATUS STATUS_NO_TOKEN = static_cast<NTSTATUS>(0xC000007C);
constexpr NTSTATUS STATUS_KEY_NOT_FOUND = STATUS_OBJECT_NAME_NOT_FOUND;

// Memory
constexpr uint32_t MEM_COMMIT = 0x1000;
constexpr uint32_t MEM_RESERVE = 0x2000;
constexpr uint32_t MEM_DECOMMIT = 0x4000;
constexpr uint32_t MEM_RELEASE = 0x8000;
constexpr uint32_t MEM_FREE = 0x10000;
constexpr uint32_t MEM_RESET = 0x80000;
constexpr uint32_t MEM_TOP_DOWN = 0x100000;
constexpr uint32_t MEM_WRITE_WATCH = 0x200000;
constexpr uint32_t MEM_PHYSICAL = 0x400000;
constexpr uint32_t MEM_LARGE_PAGES = 0x20000000;
constexpr uint32_t MEM_PRIVATE = 0x20000;
constexpr uint32_t MEM_MAPPED = 0x40000;
constexpr uint32_t MEM_IMAGE = 0x1000000;
constexpr uint32_t MEM_COALESCE_PLACEHOLDERS = 0x1;
constexpr uint32_t MEM_PRESERVE_PLACEHOLDER = 0x2;

constexpr uint32_t PAGE_NOACCESS = 0x01;
constexpr uint32_t PAGE_READONLY = 0x02;
constexpr uint32_t PAGE_READWRITE = 0x04;
constexpr uint32_t PAGE_WRITECOPY = 0x08;
constexpr uint32_t PAGE_EXECUTE = 0x10;
constexpr uint32_t PAGE_EXECUTE_READ = 0x20;
constexpr uint32_t PAGE_EXECUTE_READWRITE = 0x40;
constexpr uint32_t PAGE_EXECUTE_WRITECOPY = 0x80;
constexpr uint32_t PAGE_GUARD = 0x100;
constexpr uint32_t PAGE_NOCACHE = 0x200;
constexpr uint32_t PAGE_WRITECOMBINE = 0x400;

constexpr uint32_t SEC_IMAGE = 0x1000000;
constexpr uint32_t SEC_COMMIT = 0x8000000;
constexpr uint32_t SEC_RESERVE = 0x4000000;
constexpr uint32_t SEC_FILE = 0x800000;

// Generic access
constexpr uint32_t DELETE_ACCESS = 0x00010000;
constexpr uint32_t SYNCHRONIZE = 0x00100000;
constexpr uint32_t GENERIC_READ = 0x80000000;
constexpr uint32_t GENERIC_WRITE = 0x40000000;
constexpr uint32_t GENERIC_EXECUTE = 0x20000000;
constexpr uint32_t GENERIC_ALL = 0x10000000;
constexpr uint32_t FILE_READ_DATA = 0x1;
constexpr uint32_t FILE_WRITE_DATA = 0x2;
constexpr uint32_t FILE_APPEND_DATA = 0x4;
constexpr uint32_t FILE_LIST_DIRECTORY = 0x1;

// CreateDisposition
constexpr uint32_t FILE_SUPERSEDE = 0;
constexpr uint32_t FILE_OPEN = 1;
constexpr uint32_t FILE_CREATE = 2;
constexpr uint32_t FILE_OPEN_IF = 3;
constexpr uint32_t FILE_OVERWRITE = 4;
constexpr uint32_t FILE_OVERWRITE_IF = 5;

// CreateOptions
constexpr uint32_t FILE_DIRECTORY_FILE = 0x1;
constexpr uint32_t FILE_NON_DIRECTORY_FILE = 0x40;
constexpr uint32_t FILE_DELETE_ON_CLOSE = 0x1000;
constexpr uint32_t FILE_SYNCHRONOUS_IO_NONALERT = 0x20;
constexpr uint32_t FILE_SYNCHRONOUS_IO_ALERT = 0x10;

// IoStatus information values
constexpr uint64_t FILE_SUPERSEDED = 0;
constexpr uint64_t FILE_OPENED = 1;
constexpr uint64_t FILE_CREATED = 2;
constexpr uint64_t FILE_OVERWRITTEN = 3;
constexpr uint64_t FILE_EXISTS = 4;
constexpr uint64_t FILE_DOES_NOT_EXIST = 5;

constexpr uint32_t OBJ_CASE_INSENSITIVE = 0x40;

#pragma pack(push, 1)
struct UNICODE_STRING {
    uint16_t Length;         // bytes
    uint16_t MaximumLength;  // bytes
    uint32_t _pad;
    uint64_t Buffer;         // PWSTR (guest pointer)
};
static_assert(sizeof(UNICODE_STRING) == 16);

struct OBJECT_ATTRIBUTES {
    uint32_t Length;
    uint32_t _pad;
    HANDLE RootDirectory;
    uint64_t ObjectName;  // PUNICODE_STRING
    uint32_t Attributes;
    uint32_t _pad2;
    uint64_t SecurityDescriptor;
    uint64_t SecurityQualityOfService;
};
static_assert(sizeof(OBJECT_ATTRIBUTES) == 48);

struct IO_STATUS_BLOCK {
    uint64_t Status;       // NTSTATUS or PVOID
    uint64_t Information;  // ULONG_PTR
};
static_assert(sizeof(IO_STATUS_BLOCK) == 16);

struct LARGE_INTEGER_ {
    int64_t QuadPart;
};

// x64 CONTEXT (0x4d0 bytes)
struct M128A {
    uint64_t Low;
    int64_t High;
};

struct CONTEXT {
    uint64_t P1Home, P2Home, P3Home, P4Home, P5Home, P6Home;
    uint32_t ContextFlags;
    uint32_t MxCsr;
    uint16_t SegCs, SegDs, SegEs, SegFs, SegGs, SegSs;
    uint32_t EFlags;
    uint64_t Dr0, Dr1, Dr2, Dr3, Dr6, Dr7;
    uint64_t Rax, Rcx, Rdx, Rbx, Rsp, Rbp, Rsi, Rdi, R8, R9, R10, R11, R12, R13, R14, R15;
    uint64_t Rip;
    // XMM_SAVE_AREA32 (512 bytes)
    uint16_t ControlWord, StatusWord;
    uint8_t TagWord, Reserved1;
    uint16_t ErrorOpcode;
    uint32_t ErrorOffset;
    uint16_t ErrorSelector, Reserved2;
    uint32_t DataOffset;
    uint16_t DataSelector, Reserved3;
    uint32_t MxCsr2;
    uint32_t MxCsr_Mask;
    M128A FloatRegisters[8];
    M128A XmmRegisters[16];
    uint8_t Reserved4[96];
    M128A VectorRegister[26];
    uint64_t VectorControl;
    uint64_t DebugControl;
    uint64_t LastBranchToRip;
    uint64_t LastBranchFromRip;
    uint64_t LastExceptionToRip;
    uint64_t LastExceptionFromRip;
};
static_assert(sizeof(CONTEXT) == 0x4d0);
static_assert(offsetof(CONTEXT, Rax) == 0x78);
static_assert(offsetof(CONTEXT, Rip) == 0xf8);
static_assert(offsetof(CONTEXT, XmmRegisters) == 0x1a0);

constexpr uint32_t CONTEXT_AMD64 = 0x100000;
constexpr uint32_t CONTEXT_CONTROL = CONTEXT_AMD64 | 0x1;
constexpr uint32_t CONTEXT_INTEGER = CONTEXT_AMD64 | 0x2;
constexpr uint32_t CONTEXT_SEGMENTS = CONTEXT_AMD64 | 0x4;
constexpr uint32_t CONTEXT_FLOATING_POINT = CONTEXT_AMD64 | 0x8;
constexpr uint32_t CONTEXT_DEBUG_REGISTERS = CONTEXT_AMD64 | 0x10;
constexpr uint32_t CONTEXT_FULL = CONTEXT_CONTROL | CONTEXT_INTEGER | CONTEXT_FLOATING_POINT;
constexpr uint32_t CONTEXT_ALL = CONTEXT_FULL | CONTEXT_SEGMENTS | CONTEXT_DEBUG_REGISTERS;

struct EXCEPTION_RECORD {
    uint32_t ExceptionCode;
    uint32_t ExceptionFlags;
    uint64_t ExceptionRecord;
    uint64_t ExceptionAddress;
    uint32_t NumberParameters;
    uint32_t _pad;
    uint64_t ExceptionInformation[15];
};
static_assert(sizeof(EXCEPTION_RECORD) == 0x98);

struct CLIENT_ID {
    uint64_t UniqueProcess;
    uint64_t UniqueThread;
};
#pragma pack(pop)

// TEB offsets (x64)
namespace teb {
constexpr size_t kExceptionList = 0x0;
constexpr size_t kStackBase = 0x8;
constexpr size_t kStackLimit = 0x10;
constexpr size_t kSelf = 0x30;
constexpr size_t kEnvironmentPointer = 0x38;
constexpr size_t kClientIdProcess = 0x40;
constexpr size_t kClientIdThread = 0x48;
constexpr size_t kProcessEnvironmentBlock = 0x60;
constexpr size_t kLastErrorValue = 0x68;
constexpr size_t kLastStatusValue = 0x1250;
constexpr size_t kDeallocationStack = 0x1478;
constexpr size_t kTlsSlots = 0x1480;
constexpr size_t kSize = 0x2000;
} // namespace teb

// PEB offsets (x64)
namespace peb {
constexpr size_t kBeingDebugged = 0x2;
constexpr size_t kBitField = 0x3;
constexpr size_t kImageBaseAddress = 0x10;
constexpr size_t kLdr = 0x18;
constexpr size_t kProcessParameters = 0x20;
constexpr size_t kProcessHeap = 0x30;
constexpr size_t kNumberOfProcessors = 0xb8;
constexpr size_t kNtGlobalFlag = 0xbc;
constexpr size_t kCriticalSectionTimeout = 0xc0;
constexpr size_t kHeapSegmentReserve = 0xc8;
constexpr size_t kHeapSegmentCommit = 0xd0;
constexpr size_t kHeapDeCommitTotalFreeThreshold = 0xd8;
constexpr size_t kHeapDeCommitFreeBlockThreshold = 0xe0;
constexpr size_t kOSMajorVersion = 0x118;
constexpr size_t kOSMinorVersion = 0x11c;
constexpr size_t kOSBuildNumber = 0x120;
constexpr size_t kOSCSDVersion = 0x122;
constexpr size_t kOSPlatformId = 0x124;
constexpr size_t kImageSubsystem = 0x128;
constexpr size_t kImageSubsystemMajorVersion = 0x12c;
constexpr size_t kImageSubsystemMinorVersion = 0x130;
constexpr size_t kActiveProcessAffinityMask = 0x138;
constexpr size_t kSessionId = 0x2c0;
constexpr size_t kSize = 0x1000;
} // namespace peb

} // namespace utsuro::nt
