// winsock.h -- ws2_32 implemented over the host's BSD sockets (winsock.cpp).
//
// The console OS reaches sockets through the AFD driver (\Device\Afd), which the host does not have; the title's thirty-odd ws2_32
// calls are answered here instead, so it can create the UDP/TCP sockets its online layers want (and talk to a LAN if there is one).
#pragma once

#include "nt/kernel.h"

namespace utsuro::nt::winsock {

#define UT_WINSOCK_FUNCTIONS(X) \
    X(WSAStartup) X(WSACleanup) X(WSAGetLastError) X(socket) X(WSASocketW) X(closesocket) X(shutdown) X(bind) X(connect) X(listen) X(accept) \
    X(ioctlsocket) X(setsockopt) X(getsockopt) X(getsockname) X(getpeername) X(send) X(recv) X(sendto) X(recvfrom) X(WSASendTo) X(WSARecvFrom) \
    X(select) X(gethostname) X(gethostbyname) X(gethostbyaddr) X(getaddrinfo) X(WSAStringToAddressW) X(WSAAddressToStringW)

#define UT_DECLARE(name) uint64_t name(Kernel&, NtThread&, SyscallArgs&);
UT_WINSOCK_FUNCTIONS(UT_DECLARE)
#undef UT_DECLARE

} // namespace utsuro::nt::winsock
