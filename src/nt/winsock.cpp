#include "nt/winsock.h"

#include "engine/log.h"
#include "nt/winrt.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <cstring>
#include <mutex>
#include <set>
#include <string>

namespace utsuro::nt::winsock {
namespace {

constexpr uint64_t kInvalidSocket = ~0ull;
constexpr uint64_t kSocketBase = 0x1000;   // guest SOCKET = base + 4 * host descriptor

thread_local int t_error = 0;
std::mutex g_mutex;
std::set<int> g_fds;

enum : int {
    WSAEINTR = 10004, WSAEBADF = 10009, WSAEACCES = 10013, WSAEFAULT = 10014, WSAEINVAL = 10022, WSAEMFILE = 10024, WSAEWOULDBLOCK = 10035,
    WSAEINPROGRESS = 10036, WSAEALREADY = 10037, WSAENOTSOCK = 10038, WSAEDESTADDRREQ = 10039, WSAEMSGSIZE = 10040, WSAEPROTOTYPE = 10041,
    WSAENOPROTOOPT = 10042, WSAEPROTONOSUPPORT = 10043, WSAEOPNOTSUPP = 10045, WSAEAFNOSUPPORT = 10047, WSAEADDRINUSE = 10048,
    WSAEADDRNOTAVAIL = 10049, WSAENETDOWN = 10050, WSAENETUNREACH = 10051, WSAECONNABORTED = 10053, WSAECONNRESET = 10054, WSAENOBUFS = 10055,
    WSAEISCONN = 10056, WSAENOTCONN = 10057, WSAETIMEDOUT = 10060, WSAECONNREFUSED = 10061, WSAEHOSTUNREACH = 10065, WSAHOST_NOT_FOUND = 11001,
};

int ToWsa(int e) {
    switch (e) {
    case EINTR: return WSAEINTR;
    case EBADF: return WSAENOTSOCK;
    case EACCES: return WSAEACCES;
    case EFAULT: return WSAEFAULT;
    case EINVAL: return WSAEINVAL;
    case EMFILE: return WSAEMFILE;
    case EWOULDBLOCK: return WSAEWOULDBLOCK;
    case EINPROGRESS: return WSAEWOULDBLOCK;
    case EALREADY: return WSAEALREADY;
    case ENOTSOCK: return WSAENOTSOCK;
    case EDESTADDRREQ: return WSAEDESTADDRREQ;
    case EMSGSIZE: return WSAEMSGSIZE;
    case EPROTOTYPE: return WSAEPROTOTYPE;
    case ENOPROTOOPT: return WSAENOPROTOOPT;
    case EPROTONOSUPPORT: return WSAEPROTONOSUPPORT;
    case EOPNOTSUPP: return WSAEOPNOTSUPP;
    case EAFNOSUPPORT: return WSAEAFNOSUPPORT;
    case EADDRINUSE: return WSAEADDRINUSE;
    case EADDRNOTAVAIL: return WSAEADDRNOTAVAIL;
    case ENETDOWN: return WSAENETDOWN;
    case ENETUNREACH: return WSAENETUNREACH;
    case ECONNABORTED: case EPIPE: return WSAECONNABORTED;
    case ECONNRESET: return WSAECONNRESET;
    case ENOBUFS: return WSAENOBUFS;
    case EISCONN: return WSAEISCONN;
    case ENOTCONN: return WSAENOTCONN;
    case ETIMEDOUT: return WSAETIMEDOUT;
    case ECONNREFUSED: return WSAECONNREFUSED;
    case EHOSTUNREACH: return WSAEHOSTUNREACH;
    default: return WSAEINVAL;
    }
}

uint64_t Fail(int wsa_error = 0) {
    const int host_errno = errno;
    t_error = wsa_error != 0 ? wsa_error : ToWsa(errno);
    if (static const bool log = std::getenv("UTSURO_WSLOG") != nullptr; log && t_error != WSAEWOULDBLOCK) {
        static std::mutex m;
        static std::set<int> seen;
        std::scoped_lock lock {m};
        if (seen.insert(t_error).second) UT_INFO("winsock: a call failed with WSA error %d (host errno %d)", t_error, host_errno);
    }
    return 0xFFFFFFFFull;   // SOCKET_ERROR as the int the title reads
}

int HostFd(uint64_t socket) {
    if (socket < kSocketBase || ((socket - kSocketBase) & 3) != 0) return -1;
    const int fd = static_cast<int>((socket - kSocketBase) / 4);
    std::scoped_lock lock {g_mutex};
    return g_fds.count(fd) ? fd : -1;
}

uint64_t Register(int fd) {
    std::scoped_lock lock {g_mutex};
    g_fds.insert(fd);
    return kSocketBase + 4ull * static_cast<uint64_t>(fd);
}

int HostFamily(int af) { return af == 2 ? AF_INET : af == 23 ? AF_INET6 : af == 0 ? AF_INET : -1; }

/// Windows' sockaddr (family as a 16-bit value, no length byte) to the host's.
bool ToHost(const uint8_t* win, int length, sockaddr_storage* out, socklen_t* out_length) {
    std::memset(out, 0, sizeof *out);
    if (win == nullptr || length < 8) return false;
    uint16_t family;
    std::memcpy(&family, win, 2);
    if (family == 2 && length >= 16) {
        auto* in = reinterpret_cast<sockaddr_in*>(out);
        in->sin_len = sizeof(sockaddr_in);
        in->sin_family = AF_INET;
        std::memcpy(&in->sin_port, win + 2, 2);
        std::memcpy(&in->sin_addr, win + 4, 4);
        *out_length = sizeof(sockaddr_in);
        return true;
    }
    if (family == 23 && length >= 28) {
        auto* in6 = reinterpret_cast<sockaddr_in6*>(out);
        in6->sin6_len = sizeof(sockaddr_in6);
        in6->sin6_family = AF_INET6;
        std::memcpy(&in6->sin6_port, win + 2, 2);
        std::memcpy(&in6->sin6_flowinfo, win + 4, 4);
        std::memcpy(&in6->sin6_addr, win + 8, 16);
        std::memcpy(&in6->sin6_scope_id, win + 24, 4);
        *out_length = sizeof(sockaddr_in6);
        return true;
    }
    return false;
}

int FromHost(const sockaddr* host, uint8_t* win, int capacity) {
    if (host->sa_family == AF_INET && capacity >= 16) {
        const auto* in = reinterpret_cast<const sockaddr_in*>(host);
        std::memset(win, 0, 16);
        const uint16_t family = 2;
        std::memcpy(win, &family, 2);
        std::memcpy(win + 2, &in->sin_port, 2);
        std::memcpy(win + 4, &in->sin_addr, 4);
        return 16;
    }
    if (host->sa_family == AF_INET6 && capacity >= 28) {
        const auto* in6 = reinterpret_cast<const sockaddr_in6*>(host);
        std::memset(win, 0, 28);
        const uint16_t family = 23;
        std::memcpy(win, &family, 2);
        std::memcpy(win + 2, &in6->sin6_port, 2);
        std::memcpy(win + 4, &in6->sin6_flowinfo, 4);
        std::memcpy(win + 8, &in6->sin6_addr, 16);
        std::memcpy(win + 24, &in6->sin6_scope_id, 4);
        return 28;
    }
    return 0;
}

uint64_t Create(int af, int type, int protocol) {
    const int family = HostFamily(af);
    if (family < 0) { t_error = WSAEAFNOSUPPORT; return kInvalidSocket; }
    if (type != 1 && type != 2) { t_error = WSAEPROTONOSUPPORT; return kInvalidSocket; }
    const int fd = ::socket(family, type == 1 ? SOCK_STREAM : SOCK_DGRAM, protocol == 6 || protocol == 17 ? protocol : 0);
    if (fd < 0) { t_error = ToWsa(errno); return kInvalidSocket; }
    if (family == AF_INET6) { int off = 0; ::setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &off, sizeof off); }
    ::signal(SIGPIPE, SIG_IGN);
    static std::once_flag once;
    (void)once;
    return Register(fd);
}

int MapFlags(int flags) { return (flags & 2 ? MSG_PEEK : 0) | (flags & 1 ? MSG_OOB : 0); }

} // namespace

uint64_t WSAStartup(Kernel&, NtThread&, SyscallArgs& a) {
    auto* data = a.Ptr<uint8_t>(1);
    if (data == nullptr) return WSAEFAULT;
    std::memset(data, 0, 408);
    const uint16_t version = 0x0202;
    std::memcpy(data, &version, 2);
    std::memcpy(data + 2, &version, 2);
    const uint16_t sockets = 0x7FFF, datagram = 65467;
    std::memcpy(data + 4, &sockets, 2);
    std::memcpy(data + 6, &datagram, 2);
    std::strcpy(reinterpret_cast<char*>(data + 16), "Utsuro Winsock over BSD sockets");
    std::strcpy(reinterpret_cast<char*>(data + 16 + 257), "Running");
    return 0;
}
uint64_t WSACleanup(Kernel&, NtThread&, SyscallArgs&) { return 0; }
uint64_t WSAGetLastError(Kernel&, NtThread&, SyscallArgs&) { return static_cast<uint64_t>(t_error); }

uint64_t socket(Kernel&, NtThread&, SyscallArgs& a) { return Create(static_cast<int>(a[0]), static_cast<int>(a[1]), static_cast<int>(a[2])); }
uint64_t WSASocketW(Kernel&, NtThread&, SyscallArgs& a) { return Create(static_cast<int>(a[0]), static_cast<int>(a[1]), static_cast<int>(a[2])); }

uint64_t closesocket(Kernel&, NtThread&, SyscallArgs& a) {
    const int fd = HostFd(a[0]);
    if (fd < 0) return Fail(WSAENOTSOCK);
    { std::scoped_lock lock {g_mutex}; g_fds.erase(fd); }
    ::close(fd);
    return 0;
}

uint64_t shutdown(Kernel&, NtThread&, SyscallArgs& a) {
    const int fd = HostFd(a[0]);
    if (fd < 0) return Fail(WSAENOTSOCK);
    const int how = static_cast<int>(a[1]);
    return ::shutdown(fd, how == 0 ? SHUT_RD : how == 1 ? SHUT_WR : SHUT_RDWR) == 0 ? 0 : Fail();
}

uint64_t bind(Kernel&, NtThread&, SyscallArgs& a) {
    const int fd = HostFd(a[0]);
    if (fd < 0) return Fail(WSAENOTSOCK);
    sockaddr_storage addr; socklen_t length;
    if (!ToHost(a.Ptr<uint8_t>(1), static_cast<int>(a[2]), &addr, &length)) return Fail(WSAEAFNOSUPPORT);
    return ::bind(fd, reinterpret_cast<sockaddr*>(&addr), length) == 0 ? 0 : Fail();
}

uint64_t connect(Kernel&, NtThread&, SyscallArgs& a) {
    const int fd = HostFd(a[0]);
    if (fd < 0) return Fail(WSAENOTSOCK);
    sockaddr_storage addr; socklen_t length;
    if (!ToHost(a.Ptr<uint8_t>(1), static_cast<int>(a[2]), &addr, &length)) return Fail(WSAEAFNOSUPPORT);
    return ::connect(fd, reinterpret_cast<sockaddr*>(&addr), length) == 0 ? 0 : Fail();
}

uint64_t listen(Kernel&, NtThread&, SyscallArgs& a) {
    const int fd = HostFd(a[0]);
    if (fd < 0) return Fail(WSAENOTSOCK);
    return ::listen(fd, static_cast<int>(a[1])) == 0 ? 0 : Fail();
}

uint64_t accept(Kernel&, NtThread&, SyscallArgs& a) {
    const int fd = HostFd(a[0]);
    if (fd < 0) { t_error = WSAENOTSOCK; return kInvalidSocket; }
    sockaddr_storage addr;
    socklen_t length = sizeof addr;
    const int client = ::accept(fd, reinterpret_cast<sockaddr*>(&addr), &length);
    if (client < 0) { t_error = ToWsa(errno); return kInvalidSocket; }
    if (auto* out = a.Ptr<uint8_t>(1)) {
        auto* out_length = a.Ptr<int32_t>(2);
        const int n = FromHost(reinterpret_cast<sockaddr*>(&addr), out, out_length != nullptr ? *out_length : 16);
        if (out_length != nullptr) *out_length = n;
    }
    return Register(client);
}

uint64_t ioctlsocket(Kernel&, NtThread&, SyscallArgs& a) {
    const int fd = HostFd(a[0]);
    if (fd < 0) return Fail(WSAENOTSOCK);
    auto* arg = a.Ptr<uint32_t>(2);
    switch (static_cast<uint32_t>(a[1])) {
    case 0x8004667E: {   // FIONBIO
        const int flags = ::fcntl(fd, F_GETFL, 0);
        if (arg == nullptr || flags < 0) return Fail(WSAEFAULT);
        return ::fcntl(fd, F_SETFL, *arg != 0 ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK)) == 0 ? 0 : Fail();
    }
    case 0x4004667F: {   // FIONREAD
        int n = 0;
        if (arg == nullptr || ::ioctl(fd, FIONREAD, &n) != 0) return Fail();
        *arg = static_cast<uint32_t>(n);
        return 0;
    }
    default: return Fail(WSAEINVAL);
    }
}

uint64_t setsockopt(Kernel&, NtThread&, SyscallArgs& a) {
    const int fd = HostFd(a[0]);
    if (fd < 0) return Fail(WSAENOTSOCK);
    const int level = static_cast<int>(a[1]), option = static_cast<int>(a[2]);
    const auto* value = a.Ptr<uint8_t>(3);
    const int length = static_cast<int>(a[4]);
    if (value == nullptr) return Fail(WSAEFAULT);
    int host_level = -1, host_option = -1;
    if (level == 0xFFFF) {
        host_level = SOL_SOCKET;
        switch (option) {
        case 4: host_option = SO_REUSEADDR; break;
        case 8: host_option = SO_KEEPALIVE; break;
        case 0x20: host_option = SO_BROADCAST; break;
        case 0x1001: host_option = SO_SNDBUF; break;
        case 0x1002: host_option = SO_RCVBUF; break;
        default: break;
        }
    } else if (level == 6 && option == 1) { host_level = IPPROTO_TCP; host_option = TCP_NODELAY; }
    if (host_level < 0 || host_option < 0 || length < 4) return 0;   // options the host does not need are accepted and ignored
    int v;
    std::memcpy(&v, value, 4);
    return ::setsockopt(fd, host_level, host_option, &v, sizeof v) == 0 ? 0 : Fail();
}

uint64_t getsockopt(Kernel&, NtThread&, SyscallArgs& a) {
    const int fd = HostFd(a[0]);
    if (fd < 0) return Fail(WSAENOTSOCK);
    const int level = static_cast<int>(a[1]), option = static_cast<int>(a[2]);
    auto* value = a.Ptr<uint8_t>(3);
    auto* length = a.Ptr<int32_t>(4);
    if (value == nullptr || length == nullptr || *length < 4) return Fail(WSAEFAULT);
    int host_option = -1;
    if (level == 0xFFFF) {
        switch (option) {
        case 0x1007: host_option = SO_ERROR; break;
        case 0x1008: host_option = SO_TYPE; break;
        case 0x1001: host_option = SO_SNDBUF; break;
        case 0x1002: host_option = SO_RCVBUF; break;
        default: break;
        }
    }
    int v = 0;
    socklen_t n = sizeof v;
    if (host_option >= 0 && ::getsockopt(fd, SOL_SOCKET, host_option, &v, &n) != 0) return Fail();
    if (host_option == SO_ERROR && v != 0) v = ToWsa(v);
    if (host_option == SO_TYPE) v = v == SOCK_STREAM ? 1 : 2;
    std::memcpy(value, &v, 4);
    *length = 4;
    return 0;
}

uint64_t getsockname(Kernel&, NtThread&, SyscallArgs& a) {
    const int fd = HostFd(a[0]);
    if (fd < 0) return Fail(WSAENOTSOCK);
    sockaddr_storage addr;
    socklen_t n = sizeof addr;
    if (::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &n) != 0) return Fail();
    auto* length = a.Ptr<int32_t>(2);
    if (length == nullptr) return Fail(WSAEFAULT);
    *length = FromHost(reinterpret_cast<sockaddr*>(&addr), a.Ptr<uint8_t>(1), *length);
    return 0;
}

uint64_t getpeername(Kernel&, NtThread&, SyscallArgs& a) {
    const int fd = HostFd(a[0]);
    if (fd < 0) return Fail(WSAENOTSOCK);
    sockaddr_storage addr;
    socklen_t n = sizeof addr;
    if (::getpeername(fd, reinterpret_cast<sockaddr*>(&addr), &n) != 0) return Fail();
    auto* length = a.Ptr<int32_t>(2);
    if (length == nullptr) return Fail(WSAEFAULT);
    *length = FromHost(reinterpret_cast<sockaddr*>(&addr), a.Ptr<uint8_t>(1), *length);
    return 0;
}

uint64_t send(Kernel&, NtThread&, SyscallArgs& a) {
    const int fd = HostFd(a[0]);
    if (fd < 0) return Fail(WSAENOTSOCK);
    const ssize_t n = ::send(fd, a.Ptr<void>(1), static_cast<size_t>(static_cast<int>(a[2])), MapFlags(static_cast<int>(a[3])) & ~MSG_PEEK);
    return n >= 0 ? static_cast<uint64_t>(n) : Fail();
}

uint64_t recv(Kernel&, NtThread&, SyscallArgs& a) {
    const int fd = HostFd(a[0]);
    if (fd < 0) return Fail(WSAENOTSOCK);
    const ssize_t n = ::recv(fd, a.Ptr<void>(1), static_cast<size_t>(static_cast<int>(a[2])), MapFlags(static_cast<int>(a[3])));
    return n >= 0 ? static_cast<uint64_t>(n) : Fail();
}

uint64_t sendto(Kernel&, NtThread&, SyscallArgs& a) {
    const int fd = HostFd(a[0]);
    if (fd < 0) return Fail(WSAENOTSOCK);
    sockaddr_storage addr; socklen_t length = 0;
    const bool have = a[4] != 0 && ToHost(a.Ptr<uint8_t>(4), static_cast<int>(a[5]), &addr, &length);
    if (a[4] != 0 && !have) return Fail(WSAEAFNOSUPPORT);
    const ssize_t n = ::sendto(fd, a.Ptr<void>(1), static_cast<size_t>(static_cast<int>(a[2])), MapFlags(static_cast<int>(a[3])) & ~MSG_PEEK,
                               have ? reinterpret_cast<sockaddr*>(&addr) : nullptr, have ? length : 0);
    return n >= 0 ? static_cast<uint64_t>(n) : Fail();
}

uint64_t recvfrom(Kernel&, NtThread&, SyscallArgs& a) {
    const int fd = HostFd(a[0]);
    if (fd < 0) return Fail(WSAENOTSOCK);
    sockaddr_storage addr;
    socklen_t length = sizeof addr;
    const ssize_t n = ::recvfrom(fd, a.Ptr<void>(1), static_cast<size_t>(static_cast<int>(a[2])), MapFlags(static_cast<int>(a[3])), reinterpret_cast<sockaddr*>(&addr), &length);
    if (n < 0) return Fail();
    if (auto* out = a.Ptr<uint8_t>(4)) {
        auto* out_length = a.Ptr<int32_t>(5);
        const int w = FromHost(reinterpret_cast<sockaddr*>(&addr), out, out_length != nullptr ? *out_length : 16);
        if (out_length != nullptr) *out_length = w;
    }
    return static_cast<uint64_t>(n);
}

// WSASendTo(s, WSABUF* buffers, count, DWORD* sent, flags, to, tolen, overlapped, completion): done at once.
uint64_t WSASendTo(Kernel&, NtThread&, SyscallArgs& a) {
    const int fd = HostFd(a[0]);
    if (fd < 0) return Fail(WSAENOTSOCK);
    const auto* bufs = a.Ptr<uint8_t>(1);
    const uint32_t count = static_cast<uint32_t>(a[2]);
    std::string data;
    for (uint32_t i = 0; i < count && bufs != nullptr; ++i) {
        uint32_t len; uint64_t ptr;
        std::memcpy(&len, bufs + 16 * i, 4);
        std::memcpy(&ptr, bufs + 16 * i + 8, 8);
        data.append(reinterpret_cast<const char*>(ptr), len);
    }
    sockaddr_storage addr; socklen_t length = 0;
    const bool have = a[5] != 0 && ToHost(a.Ptr<uint8_t>(5), static_cast<int>(a[6]), &addr, &length);
    const ssize_t n = ::sendto(fd, data.data(), data.size(), MapFlags(static_cast<int>(a[4])) & ~MSG_PEEK, have ? reinterpret_cast<sockaddr*>(&addr) : nullptr, have ? length : 0);
    if (n < 0) return Fail();
    if (auto* sent = a.Ptr<uint32_t>(3)) *sent = static_cast<uint32_t>(n);
    return 0;
}

// WSARecvFrom(s, WSABUF* buffers, count, DWORD* received, DWORD* flags, from, fromlen, overlapped, completion): into the first buffer.
uint64_t WSARecvFrom(Kernel&, NtThread&, SyscallArgs& a) {
    const int fd = HostFd(a[0]);
    if (fd < 0) return Fail(WSAENOTSOCK);
    const auto* bufs = a.Ptr<uint8_t>(1);
    if (bufs == nullptr || static_cast<uint32_t>(a[2]) == 0) return Fail(WSAEFAULT);
    uint32_t len; uint64_t ptr;
    std::memcpy(&len, bufs, 4);
    std::memcpy(&ptr, bufs + 8, 8);
    sockaddr_storage addr;
    socklen_t length = sizeof addr;
    const auto* flags_in = a.Ptr<uint32_t>(4);
    const ssize_t n = ::recvfrom(fd, reinterpret_cast<void*>(ptr), len, flags_in != nullptr ? MapFlags(static_cast<int>(*flags_in)) : 0, reinterpret_cast<sockaddr*>(&addr), &length);
    if (n < 0) return Fail();
    if (auto* got = a.Ptr<uint32_t>(3)) *got = static_cast<uint32_t>(n);
    if (auto* out = a.Ptr<uint8_t>(5)) {
        auto* out_length = a.Ptr<int32_t>(6);
        const int w = FromHost(reinterpret_cast<sockaddr*>(&addr), out, out_length != nullptr ? *out_length : 16);
        if (out_length != nullptr) *out_length = w;
    }
    return 0;
}

// select(nfds, readfds, writefds, exceptfds, timeout): Winsock's fd_set is a count and an array of 64-bit SOCKETs.
uint64_t select(Kernel&, NtThread&, SyscallArgs& a) {
    uint8_t* sets[3] = {a.Ptr<uint8_t>(1), a.Ptr<uint8_t>(2), a.Ptr<uint8_t>(3)};
    std::vector<pollfd> fds;
    std::vector<std::pair<int, uint64_t>> index;   // which set and which guest socket each poll entry came from
    for (int s = 0; s < 3; ++s) {
        if (sets[s] == nullptr) continue;
        uint32_t count;
        std::memcpy(&count, sets[s], 4);
        for (uint32_t i = 0; i < count && i < 64; ++i) {
            uint64_t sock;
            std::memcpy(&sock, sets[s] + 8 + 8 * i, 8);
            const int fd = HostFd(sock);
            if (fd < 0) return Fail(WSAENOTSOCK);
            pollfd p {};
            p.fd = fd;
            p.events = s == 0 ? POLLIN : s == 1 ? POLLOUT : POLLPRI;
            fds.push_back(p);
            index.push_back({s, sock});
        }
    }
    int timeout_ms = -1;
    if (const auto* tv = a.Ptr<int32_t>(4)) timeout_ms = tv[0] * 1000 + tv[1] / 1000;
    const int n = ::poll(fds.data(), static_cast<nfds_t>(fds.size()), timeout_ms);
    if (n < 0) return Fail();
    uint32_t counts[3] = {0, 0, 0};
    for (size_t i = 0; i < fds.size(); ++i) {
        const int s = index[i].first;
        const short want = s == 0 ? (POLLIN | POLLHUP | POLLERR) : s == 1 ? POLLOUT : POLLPRI;
        if (!(fds[i].revents & want)) continue;
        std::memcpy(sets[s] + 8 + 8 * counts[s]++, &index[i].second, 8);
    }
    for (int s = 0; s < 3; ++s) if (sets[s] != nullptr) std::memcpy(sets[s], &counts[s], 4);
    return static_cast<uint64_t>(counts[0] + counts[1] + counts[2]);
}

uint64_t gethostname(Kernel&, NtThread&, SyscallArgs& a) {
    auto* name = a.Ptr<char>(0);
    const int length = static_cast<int>(a[1]);
    static const char kName[] = "XBOXONE";
    if (name == nullptr || length < static_cast<int>(sizeof kName)) return Fail(WSAEFAULT);
    std::memcpy(name, kName, sizeof kName);
    return 0;
}

namespace {
uint64_t HostEntry(const hostent* h) {
    static thread_local uint64_t block = 0;
    if (block == 0) block = winrt::Runtime::Get().AllocGuest(0x200, 16);
    if (h == nullptr || h->h_addrtype != AF_INET) { t_error = WSAHOST_NOT_FOUND; return 0; }
    auto* b = reinterpret_cast<uint8_t*>(block);
    std::memset(b, 0, 0x200);
    char* name = reinterpret_cast<char*>(b + 0x80);
    std::strncpy(name, h->h_name != nullptr ? h->h_name : "", 0x3F);
    auto* addrs = reinterpret_cast<uint64_t*>(b + 0x40);      // h_addr_list: up to 3 addresses and a terminator
    for (int i = 0; i < 3 && h->h_addr_list[i] != nullptr; ++i) {
        std::memcpy(b + 0xC0 + 4 * i, h->h_addr_list[i], 4);
        addrs[i] = reinterpret_cast<uint64_t>(b + 0xC0 + 4 * i);
    }
    auto* aliases = reinterpret_cast<uint64_t*>(b + 0x60);    // none
    aliases[0] = 0;
    *reinterpret_cast<uint64_t*>(b + 0) = reinterpret_cast<uint64_t>(name);
    *reinterpret_cast<uint64_t*>(b + 8) = reinterpret_cast<uint64_t>(aliases);
    *reinterpret_cast<int16_t*>(b + 16) = 2;
    *reinterpret_cast<int16_t*>(b + 18) = 4;
    *reinterpret_cast<uint64_t*>(b + 24) = reinterpret_cast<uint64_t>(addrs);
    return block;
}
} // namespace

uint64_t gethostbyname(Kernel&, NtThread&, SyscallArgs& a) {
    const char* name = a.Ptr<const char>(0);
    if (name == nullptr) { t_error = WSAEFAULT; return 0; }
    return HostEntry(::gethostbyname(name));
}

uint64_t gethostbyaddr(Kernel&, NtThread&, SyscallArgs& a) {
    const auto* addr = a.Ptr<uint8_t>(0);
    if (addr == nullptr || static_cast<int>(a[1]) != 4) { t_error = WSAEFAULT; return 0; }
    return HostEntry(::gethostbyaddr(addr, 4, AF_INET));
}

// getaddrinfo(node, service, hints, ADDRINFOA** result): numeric addresses and names the host resolves; the list lives in guest memory.
uint64_t getaddrinfo(Kernel&, NtThread&, SyscallArgs& a) {
    const char* node = a.Ptr<const char>(0);
    const char* service = a.Ptr<const char>(1);
    const auto* hints_in = a.Ptr<const int32_t>(2);
    auto* result = a.Ptr<uint64_t>(3);
    if (result == nullptr) return WSAEFAULT;
    addrinfo hints {};
    hints.ai_family = AF_INET;
    if (hints_in != nullptr) {
        hints.ai_flags = hints_in[0] & (AI_PASSIVE | AI_NUMERICHOST);
        if (hints_in[1] == 23) hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = hints_in[2] == 1 ? SOCK_STREAM : hints_in[2] == 2 ? SOCK_DGRAM : 0;
    }
    addrinfo* list = nullptr;
    if (::getaddrinfo(node, service, &hints, &list) != 0 || list == nullptr) return WSAHOST_NOT_FOUND;
    uint64_t first = 0, previous = 0;
    for (addrinfo* p = list; p != nullptr; p = p->ai_next) {
        if (p->ai_family != AF_INET && p->ai_family != AF_INET6) continue;
        const uint64_t at = winrt::Runtime::Get().AllocGuest(48 + 32, 16);
        auto* e = reinterpret_cast<uint8_t*>(at);
        std::memset(e, 0, 80);
        const int32_t family = p->ai_family == AF_INET ? 2 : 23, type = p->ai_socktype == SOCK_STREAM ? 1 : 2, proto = p->ai_protocol;
        std::memcpy(e + 4, &family, 4);
        std::memcpy(e + 8, &type, 4);
        std::memcpy(e + 12, &proto, 4);
        const uint64_t length = FromHost(p->ai_addr, e + 48, 32);
        std::memcpy(e + 16, &length, 8);
        const uint64_t sockaddr_at = at + 48;
        std::memcpy(e + 32, &sockaddr_at, 8);
        if (previous != 0) std::memcpy(reinterpret_cast<uint8_t*>(previous) + 40, &at, 8);
        if (first == 0) first = at;
        previous = at;
    }
    ::freeaddrinfo(list);
    if (first == 0) return WSAHOST_NOT_FOUND;
    *result = first;
    return 0;
}

// WSAStringToAddressW(string, family, protocol info, SOCKADDR* address, int* length): "a.b.c.d" or "a.b.c.d:port".
uint64_t WSAStringToAddressW(Kernel&, NtThread&, SyscallArgs& a) {
    const auto* wide = a.Ptr<const char16_t>(0);
    auto* out = a.Ptr<uint8_t>(3);
    auto* length = a.Ptr<int32_t>(4);
    if (wide == nullptr || out == nullptr || length == nullptr || *length < 16) return Fail(WSAEINVAL);
    std::string text;
    for (const char16_t* c = wide; *c != 0 && text.size() < 64; ++c) text += static_cast<char>(*c);
    unsigned port = 0;
    const auto colon = text.find(':');
    if (colon != std::string::npos) { port = static_cast<unsigned>(std::atoi(text.c_str() + colon + 1)); text.resize(colon); }
    in_addr addr {};
    if (inet_pton(AF_INET, text.c_str(), &addr) != 1) return Fail(WSAEINVAL);
    std::memset(out, 0, 16);
    const uint16_t family = 2, net_port = htons(static_cast<uint16_t>(port));
    std::memcpy(out, &family, 2);
    std::memcpy(out + 2, &net_port, 2);
    std::memcpy(out + 4, &addr, 4);
    *length = 16;
    return 0;
}

// WSAAddressToStringW(SOCKADDR* address, length, protocol info, LPWSTR string, DWORD* string length)
uint64_t WSAAddressToStringW(Kernel&, NtThread&, SyscallArgs& a) {
    const auto* in = a.Ptr<const uint8_t>(0);
    auto* out = a.Ptr<char16_t>(3);
    auto* length = a.Ptr<uint32_t>(4);
    if (in == nullptr || out == nullptr || length == nullptr) return Fail(WSAEINVAL);
    char text[64] = {};
    uint16_t family;
    std::memcpy(&family, in, 2);
    if (family == 2) {
        char ip[32];
        inet_ntop(AF_INET, in + 4, ip, sizeof ip);
        uint16_t port;
        std::memcpy(&port, in + 2, 2);
        if (port != 0) std::snprintf(text, sizeof text, "%s:%u", ip, ntohs(port)); else std::snprintf(text, sizeof text, "%s", ip);
    } else {
        std::snprintf(text, sizeof text, "::");
    }
    const uint32_t need = static_cast<uint32_t>(std::strlen(text)) + 1;
    if (*length < need) { *length = need; return Fail(WSAEFAULT); }
    for (uint32_t i = 0; i < need; ++i) out[i] = static_cast<char16_t>(text[i]);
    *length = need;
    return 0;
}

} // namespace utsuro::nt::winsock
