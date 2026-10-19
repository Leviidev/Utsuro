// logan.cpp -- see logan.h.
#include "nt/logan.h"

#include "engine/log.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <map>
#include <mutex>
#include <thread>

namespace utsuro::nt::logan {

namespace {

constexpr uint32_t kIoctlAllocMap = 0x8000E007, kIoctlAttachChannel = 0x8000E00B, kIoctlGetMemoryStats = 0x8000E00F,
                   kIoctlGetNuiBuffer = 0x8000E013, kIoctlSetNuiOffset = 0x8000E017, kIoctlLoadFirmware = 0x8000E01B,
                   kIoctlSetUsbCapture = 0x8000E01F, kIoctlGetDriverMemory = 0x8000E023;

constexpr uint32_t kLoganCommandAcpInit = 0xD0000000;

constexpr int32_t kInternalConnect = -2147483648, kInternalDisconnect = -2147483647, kInternalRegisterContextArrays = -2147483646,
                  kInternalEventLogInit = -2147483645, kInternalEventLogEnable = -2147483644, kInternalEventLogDisable = -2147483643,
                  kInternalTerminate = -2147483642;

constexpr uint32_t kCmdRegisterMessage = 0x2, kCmdUnregisterMessage = 0x3;
constexpr uint32_t kMsgFlowgraphCompleted = 0x2, kMsgCommandCompleted = 0x10;

#pragma pack(push, 1)
struct RingDescriptor {
    uint32_t offset_read;
    uint32_t reserved0[63];
    uint32_t offset_write;
    uint32_t apu_address;
    uint32_t size_in_blocks;
    uint32_t reserved1[61];
};
struct Channel {
    RingDescriptor commands;
    RingDescriptor messages;
    uint8_t trace[256];
};
struct LoganCommand { uint32_t type; uint32_t apu_address; uint32_t reserved[62]; };

struct AcpState {
    uint32_t internal_read_pointer, internal_send_counter, internal_read_counter;
    uint32_t client_read_pointer[4], client_send_counter[4], client_read_counter[4];
    uint32_t client_message_write_pointer[4];
};
struct ConnectCommand {
    uint32_t instance, index, num_commands, num_messages;
    uint32_t command_queue, pending_command_list, message_queue;
};
struct AcpCommand {
    uint32_t frame;
    uint64_t command_id;
    uint32_t command_type;
    uint32_t notification;
    union {
        uint32_t message;            // register / unregister message
        ConnectCommand connect;
        uint32_t raw[28];
    };
};
struct QueueEntry { uint32_t state; AcpCommand command; uint8_t pad[120]; };
struct AcpMessage {
    uint32_t type, dropped, usec;
    union {
        struct { uint32_t flowgraph; } flowgraph_completed;
        struct { uint32_t command_type; uint64_t command_id; uint32_t audio_frame; } command_completed;
        uint32_t raw[4];
    };
};
struct MessageEntry { uint32_t state; AcpMessage message; uint8_t pad[224]; };

struct InIoctlAllocMap { uint32_t size, flags; uint64_t address; uint64_t reserved[2]; };
struct OutIoctlAllocMap { uint32_t allocated, reserved0; uint64_t address; uint32_t apu_address; uint32_t reserved1[3]; };
struct InIoctlAttach { uint32_t core; uint32_t apu_address; };
struct InIoctlDriverMemory { uint32_t type; };
struct OutIoctlDriverMemory { uint64_t address; uint32_t size; uint32_t apu_address; uint32_t reserved[2]; };
#pragma pack(pop)
static_assert(sizeof(QueueEntry) == 256 && sizeof(MessageEntry) == 256 && sizeof(LoganCommand) == 256, "ACP ring entries are 256 bytes");

struct Mapping { uint32_t apu; uint32_t size; uint64_t va; };

std::mutex g_mutex;
std::vector<Mapping> g_maps;
uint32_t g_bump = 0x10000;
Mapping g_driver[3] {};
std::atomic<bool> g_acp_running {false};

uint32_t Bump(uint32_t size) {
    const uint32_t at = g_bump;
    g_bump += (size + 0xFFFF) & ~0xFFFFu;
    return at;
}

/// The host pointer for `bytes` at APU address `apu`, or null.
void* Host(uint32_t apu, uint64_t bytes) {
    if (apu == 0) return nullptr;
    std::scoped_lock lock {g_mutex};
    for (const Mapping& m : g_maps) {
        if (apu >= m.apu && static_cast<uint64_t>(apu) + bytes <= static_cast<uint64_t>(m.apu) + m.size) return reinterpret_cast<void*>(m.va + (apu - m.apu));
    }
    return nullptr;
}

template <typename T>
T* HostAs(uint32_t apu, uint64_t count = 1) { return static_cast<T*>(Host(apu, sizeof(T) * count)); }

// ---- the ACP -------------------------------------------------------------------------------------------------------------

struct AcpEmulator {
    uint32_t acp_command_queue {0}, acp_state_apu {0};
    ConnectCommand clients[4] {};
    bool connected[4] {};
    uint32_t enabled_messages {0};
    uint64_t completed {0};
    uint32_t audio_frame {0};
    uint32_t dropped {0};
    uint32_t flowgraph {0};
    uint64_t sent {0};
    std::chrono::steady_clock::time_point last_report {};
};
AcpEmulator g_acp;

void Send(const AcpMessage& message, MessageEntry* queue, AcpState* state, uint32_t client) {
    const uint32_t slot = state->client_message_write_pointer[client];
    if (queue[slot].state != 0) { ++g_acp.dropped; return; }
    queue[slot].message = message;
    ++g_acp.sent;
    std::atomic_thread_fence(std::memory_order_release);
    queue[slot].state = 1;
    state->client_message_write_pointer[client] = slot + 1 < g_acp.clients[client].num_messages ? slot + 1 : 0;
}

void DispatchInternal(const AcpCommand& c) {
    switch (static_cast<int32_t>(c.command_type)) {
    case kInternalConnect:
        if (c.connect.index < 4) {
            g_acp.clients[c.connect.index] = c.connect;
            g_acp.connected[c.connect.index] = true;
            UT_INFO("logan: client %u connected (%u commands, %u messages)", c.connect.index, c.connect.num_commands, c.connect.num_messages);
        }
        break;
    case kInternalRegisterContextArrays: UT_INFO("logan: context arrays registered"); break;
    case kInternalDisconnect: case kInternalEventLogInit: case kInternalEventLogEnable: case kInternalEventLogDisable: case kInternalTerminate: break;
    default: UT_INFO("logan: unknown internal ACP command %#x", c.command_type);
    }
}

void DispatchClient(const AcpCommand& c) {
    {
        static uint32_t counts[64] {};
        const uint32_t type = c.command_type < 64 ? c.command_type : 63;
        if (counts[type]++ == 0) UT_INFO("logan: first client command of type %#x (message mask now %#x)", c.command_type, g_acp.enabled_messages);
    }
    if (c.command_type == kCmdRegisterMessage) g_acp.enabled_messages |= c.message;
    else if (c.command_type == kCmdUnregisterMessage) g_acp.enabled_messages &= ~c.message;
    else if (c.command_type == 0x1) g_acp.flowgraph = c.raw[1];   // LOAD_SHAPE_FLOWGRAPH: {numCommands, flowgraph, ...}
    // Everything else (XMA, DMA, SRC, EQ, filter contexts) is accepted without being acted on: there is no audio path behind it yet.
}

void RunChannel(Channel* channel) {
    auto* commands = HostAs<LoganCommand>(channel->commands.apu_address, channel->commands.size_in_blocks);
    AcpState* state = nullptr;
    LoganCommand* init_data = nullptr;
    struct InitData { uint32_t unknown0, acp_command_queue, acp_state; };
    const auto start = std::chrono::steady_clock::now();
    auto last_frame = start;
    while (g_acp_running.load()) {
        // The HAL's own ring: only the ACP_INIT command matters.
        while (commands != nullptr && channel->commands.offset_read != channel->commands.offset_write) {
            std::atomic_thread_fence(std::memory_order_acquire);
            const LoganCommand cmd = commands[channel->commands.offset_read];
            channel->commands.offset_read = channel->commands.offset_read + 1 < channel->commands.size_in_blocks ? channel->commands.offset_read + 1 : 0;
            if (cmd.type == kLoganCommandAcpInit) {
                if (const auto* data = HostAs<InitData>(cmd.apu_address)) {
                    g_acp.acp_command_queue = data->acp_command_queue;
                    g_acp.acp_state_apu = data->acp_state;
                    UT_INFO("logan: ACP init (queue %#x, state %#x)", data->acp_command_queue, data->acp_state);
                }
            } else {
                UT_INFO("logan: unimplemented command %#x", cmd.type);
            }
        }
        (void)init_data;

        if (g_acp.acp_state_apu != 0) {
            if (state == nullptr) state = HostAs<AcpState>(g_acp.acp_state_apu);
            auto* internal_queue = HostAs<QueueEntry>(g_acp.acp_command_queue, 16);
            if (state != nullptr && internal_queue != nullptr) {
                while (state->internal_read_counter != state->internal_send_counter) {
                    std::atomic_thread_fence(std::memory_order_acquire);
                    const AcpCommand c = internal_queue[state->internal_read_pointer].command;
                    internal_queue[state->internal_read_pointer].state = 0;
                    state->internal_read_pointer = state->internal_read_pointer + 1 >= 16 ? 0 : state->internal_read_pointer + 1;
                    ++state->internal_read_counter;
                    if (c.command_type != 0) DispatchInternal(c);
                }
            }

            // One audio frame is 128 samples at 48 kHz.
            const auto now = std::chrono::steady_clock::now();
            const auto frame_time = std::chrono::nanoseconds(1000000000ll * 128 / 48000);
            uint32_t due = 0;
            while (now - last_frame >= frame_time && due < 8) { last_frame += frame_time; ++due; }
            if (now - last_frame > frame_time * 8) last_frame = now;
            const uint32_t frame_base = g_acp.audio_frame;
            g_acp.audio_frame += due;

            for (uint32_t i = 0; state != nullptr && i < 4; ++i) {
                if (!g_acp.connected[i]) continue;
                const ConnectCommand& cc = g_acp.clients[i];
                auto* queue = HostAs<QueueEntry>(cc.command_queue, cc.num_commands);
                auto* messages = HostAs<MessageEntry>(cc.message_queue, cc.num_messages);
                if (queue == nullptr || messages == nullptr) continue;
                while (state->client_read_counter[i] != state->client_send_counter[i]) {
                    std::atomic_thread_fence(std::memory_order_acquire);
                    const AcpCommand c = queue[state->client_read_pointer[i]].command;
                    queue[state->client_read_pointer[i]].state = 0;
                    state->client_read_pointer[i] = state->client_read_pointer[i] + 1 >= cc.num_commands ? 0 : state->client_read_pointer[i] + 1;
                    ++state->client_read_counter[i];
                    if (c.command_type == 0) continue;
                    DispatchClient(c);
                    const uint64_t sequence = ++g_acp.completed;
                    if (g_acp.enabled_messages & kMsgCommandCompleted) {
                        AcpMessage m {};
                        m.type = kMsgCommandCompleted;
                        m.dropped = g_acp.dropped;
                        m.command_completed.command_type = c.command_type;
                        m.command_completed.command_id = c.command_id != 0 ? c.command_id : sequence;
                        m.command_completed.audio_frame = g_acp.audio_frame;
                        Send(m, messages, state, i);
                    }
                }
                // The beat the title's audio engine runs on: one flowgraph completion per frame.
                if (g_acp.enabled_messages & kMsgFlowgraphCompleted) {
                    for (uint32_t beat = 0; beat < due; ++beat) {
                        AcpMessage m {};
                        m.type = kMsgFlowgraphCompleted;
                        m.dropped = g_acp.dropped;
                        m.flowgraph_completed.flowgraph = g_acp.flowgraph;
                        Send(m, messages, state, i);
                    }
                }
            }
            (void)frame_base;
        }
        {
            const auto now = std::chrono::steady_clock::now();
            if (g_acp.last_report == std::chrono::steady_clock::time_point {}) g_acp.last_report = now;
            if (now - g_acp.last_report > std::chrono::seconds(3)) {
                g_acp.last_report = now;
                UT_INFO("logan: %llu messages sent, %u dropped, frame %u, %llu commands completed, client0 write %u / title read %u", (unsigned long long)g_acp.sent,
                        g_acp.dropped, g_acp.audio_frame, (unsigned long long)g_acp.completed,
                        state ? state->client_message_write_pointer[0] : 0u, state ? state->client_read_counter[0] : 0u);
            }
        }
        std::this_thread::sleep_for(std::chrono::microseconds(500));
    }
}

} // namespace

bool IsDevicePath(const std::string& nt_path) {
    std::string low = nt_path;
    for (auto& c : low) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return low.find("acpi_hal#logan#") != std::string::npos;
}

uint32_t MapMemory(uint64_t va, uint32_t size) {
    std::scoped_lock lock {g_mutex};
    for (const Mapping& m : g_maps) if (m.va == va) return m.apu;
    Mapping m {Bump(size), (size + 0xFFFFu) & ~0xFFFFu, va};
    g_maps.push_back(m);
    return m.apu;
}

uint32_t ApuFor(uint64_t va, uint32_t size) {
    {
        std::scoped_lock lock {g_mutex};
        for (const Mapping& m : g_maps) if (va >= m.va && va < m.va + m.size) return m.apu + static_cast<uint32_t>(va - m.va);
    }
    return MapMemory(va, size);
}

void UnmapMemory(uint64_t va) {
    std::scoped_lock lock {g_mutex};
    g_maps.erase(std::remove_if(g_maps.begin(), g_maps.end(), [&](const Mapping& m) { return m.va == va; }), g_maps.end());
}

NTSTATUS Ioctl(Kernel& kernel, uint32_t code, const uint8_t* in, uint32_t in_len, uint8_t* out, uint32_t out_len, uint64_t* returned) {
    *returned = 0;
    switch (code) {
    case kIoctlAllocMap: {
        if (in == nullptr || out == nullptr || in_len < sizeof(InIoctlAllocMap) || out_len < sizeof(OutIoctlAllocMap)) return STATUS_INVALID_PARAMETER;
        const auto* req = reinterpret_cast<const InIoctlAllocMap*>(in);
        uint64_t va = req->address;
        const uint32_t size = (req->size + 0xFFFFu) & ~0xFFFFu;
        // The HAL names the address to map into; if nothing is committed there yet, make the memory.
        if (va == 0 || !kernel.Vm().Contains(va, size)) {
            uint64_t base = va, len = size;
            if (kernel.Vm().Allocate(&base, &len, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE) != STATUS_SUCCESS) return STATUS_NO_MEMORY;
            va = base;
        }
        OutIoctlAllocMap res {};
        res.allocated = req->size;
        res.address = va;
        res.apu_address = MapMemory(va, req->size);
        std::memcpy(out, &res, sizeof res);
        *returned = 32;
        return STATUS_SUCCESS;
    }
    case kIoctlAttachChannel: {
        if (in == nullptr || in_len < sizeof(InIoctlAttach)) return STATUS_INVALID_PARAMETER;
        const auto* req = reinterpret_cast<const InIoctlAttach*>(in);
        Channel* channel = HostAs<Channel>(req->apu_address);
        if (channel == nullptr) return STATUS_INVALID_PARAMETER;
        UT_INFO("logan: channel attached for core %u", req->core);
        // Only the ACP's channel carries the command rings the emulation serves; the others (ASP, AVP) are accepted and ignored.
        if (req->core == 0x03 && !g_acp_running.exchange(true)) std::thread(RunChannel, channel).detach();
        return STATUS_SUCCESS;
    }
    case kIoctlGetDriverMemory: {
        if (in == nullptr || out == nullptr || in_len < sizeof(InIoctlDriverMemory) || out_len < sizeof(OutIoctlDriverMemory)) return STATUS_INVALID_PARAMETER;
        const uint32_t index = reinterpret_cast<const InIoctlDriverMemory*>(in)->type;
        if (index >= 3) return STATUS_INVALID_PARAMETER;
        std::unique_lock lock {g_mutex};
        if (g_driver[index].va == 0) {
            lock.unlock();
            uint64_t base = 0, len = 0x100000;
            if (kernel.Vm().Allocate(&base, &len, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE) != STATUS_SUCCESS) return STATUS_NO_MEMORY;
            const uint32_t apu = MapMemory(base, 0x100000);
            lock.lock();
            g_driver[index] = {apu, 0x100000, base};
        }
        OutIoctlDriverMemory res {};
        res.address = g_driver[index].va;
        res.size = g_driver[index].size;
        res.apu_address = g_driver[index].apu;
        std::memcpy(out, &res, sizeof res);
        *returned = sizeof res;
        return STATUS_SUCCESS;
    }
    case kIoctlLoadFirmware: case kIoctlGetMemoryStats: case kIoctlGetNuiBuffer: case kIoctlSetNuiOffset: case kIoctlSetUsbCapture:
        return STATUS_SUCCESS;
    default:
        UT_WARN("logan: unknown IOCTL %#x", code);
        return STATUS_SUCCESS;
    }
}

} // namespace utsuro::nt::logan
