// src/platform/socket_compat.h
// Cross-platform socket abstraction for SyncText.
//
// On Windows:  wraps Winsock2 (ws2_32.lib must be linked).
// On POSIX:    thin inline wrappers around the existing POSIX socket API.
//
// Exposed uniform API used by network.cpp:
//   platformInit()           — WSAStartup on Windows, no-op on POSIX
//   platformCleanup()        — WSACleanup on Windows, no-op on POSIX
//   platformSocket(...)      — socket()
//   platformBind(...)        — bind()
//   platformConnect(...)     — connect()
//   platformSend(...)        — send()
//   platformRecv(...)        — recv()
//   platformSendTo(...)      — sendto()
//   platformRecvFrom(...)    — recvfrom()
//   platformClose(fd)        — closesocket() / close()
//   platformSetNonBlocking(fd) — ioctlsocket() / fcntl()
//   platformPoll(...)        — WSAPoll() / poll()
//   platformInetPton(...)    — InetPton() / inet_pton()
//   platformInetNtoa(...)    — inet_ntoa()
//   platformGetError()       — WSAGetLastError() / errno
//   platformWouldBlock()     — true if last error is EWOULDBLOCK / WSAEWOULDBLOCK
//
// Socket handle type:
//   PlatformSocket            — SOCKET (UINT_PTR) on Windows, int on POSIX
//   INVALID_PLATFORM_SOCKET   — INVALID_SOCKET on Windows, -1 on POSIX
//
// Additional portability defines:
//   PLATFORM_MSG_NOSIGNAL     — MSG_NOSIGNAL on POSIX, 0 on Windows
//   platform_ssize_t          — SSIZE_T on Windows, ssize_t on POSIX
//   platform_socklen_t        — int on Windows, socklen_t on POSIX

#pragma once

// ============================================================
// Windows
// ============================================================
#ifdef _WIN32

#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif

#  include <winsock2.h>
#  include <ws2tcpip.h>
#  include <BaseTsd.h>   // SSIZE_T

#  pragma comment(lib, "ws2_32.lib")

#  include <cerrno>
#  include <cstring>

namespace syntext {
namespace platform {

// ---- Type aliases ---------------------------------------------------------
using PlatformSocket  = SOCKET;       // UINT_PTR — NOT int
using platform_ssize_t = SSIZE_T;     // __int64 on 64-bit Windows
using platform_socklen_t = int;        // Winsock uses int for socklen

static constexpr PlatformSocket INVALID_PLATFORM_SOCKET = INVALID_SOCKET;

// MSG_NOSIGNAL does not exist on Windows (no SIGPIPE).
static constexpr int PLATFORM_MSG_NOSIGNAL = 0;

// ---- WSAStartup / WSACleanup ----------------------------------------------
inline bool platformInit() {
  WSADATA wsa;
  return ::WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
}

inline void platformCleanup() {
  ::WSACleanup();
}

// ---- Error reporting -------------------------------------------------------
inline int platformGetError() {
  return ::WSAGetLastError();
}

inline bool platformWouldBlock() {
  int err = ::WSAGetLastError();
  return err == WSAEWOULDBLOCK || err == WSAEINPROGRESS;
}

// ---- Socket creation / teardown -------------------------------------------
inline PlatformSocket platformSocket(int af, int type, int proto) {
  return ::socket(af, type, proto);
}

inline void platformClose(PlatformSocket fd) {
  if (fd != INVALID_SOCKET) {
    ::closesocket(fd);
  }
}

// ---- Non-blocking mode ----------------------------------------------------
inline bool platformSetNonBlocking(PlatformSocket fd) {
  u_long mode = 1;
  return ::ioctlsocket(fd, FIONBIO, &mode) == 0;
}

// ---- Bind / listen / accept -----------------------------------------------
inline int platformBind(PlatformSocket fd,
                        const struct sockaddr* addr, int addrlen) {
  return ::bind(fd, addr, addrlen);
}

inline int platformListen(PlatformSocket fd, int backlog) {
  return ::listen(fd, backlog);
}

inline PlatformSocket platformAccept(PlatformSocket fd,
                                      struct sockaddr* addr,
                                      platform_socklen_t* addrlen) {
  return ::accept(fd, addr, addrlen);
}

// ---- Connect ---------------------------------------------------------------
inline int platformConnect(PlatformSocket fd,
                           const struct sockaddr* addr, int addrlen) {
  return ::connect(fd, addr, addrlen);
}

// ---- Send / recv -----------------------------------------------------------
// Returns number of bytes sent/received, or SOCKET_ERROR (-1) on error.
inline platform_ssize_t platformSend(PlatformSocket fd,
                                      const void* buf, int len, int flags) {
  return static_cast<platform_ssize_t>(
      ::send(fd, static_cast<const char*>(buf), len, flags));
}

inline platform_ssize_t platformRecv(PlatformSocket fd,
                                      void* buf, int len, int flags) {
  return static_cast<platform_ssize_t>(
      ::recv(fd, static_cast<char*>(buf), len, flags));
}

inline platform_ssize_t platformSendTo(PlatformSocket fd,
                                        const void* buf, int len, int flags,
                                        const struct sockaddr* dest,
                                        platform_socklen_t destlen) {
  return static_cast<platform_ssize_t>(
      ::sendto(fd, static_cast<const char*>(buf), len, flags, dest, destlen));
}

inline platform_ssize_t platformRecvFrom(PlatformSocket fd,
                                          void* buf, int len, int flags,
                                          struct sockaddr* src,
                                          platform_socklen_t* srclen) {
  return static_cast<platform_ssize_t>(
      ::recvfrom(fd, static_cast<char*>(buf), len, flags, src, srclen));
}

// ---- setsockopt ------------------------------------------------------------
// Winsock setsockopt() requires const char* for optval — we cast for you.
inline int platformSetSockOpt(PlatformSocket fd, int level, int optname,
                               const void* optval, platform_socklen_t optlen) {
  return ::setsockopt(fd, level, optname,
                      static_cast<const char*>(optval), optlen);
}

// ---- Poll ------------------------------------------------------------------
// WSAPoll() has the same signature as POSIX poll() for our usage.
inline int platformPoll(WSAPOLLFD* fds, ULONG nfds, int timeout_ms) {
  return ::WSAPoll(fds, nfds, timeout_ms);
}

// Alias so network.cpp can use pollfd uniformly.
using platform_pollfd = WSAPOLLFD;

// ---- Address helpers -------------------------------------------------------
// inet_pton: available in ws2tcpip.h on Vista+.
inline int platformInetPton(int af, const char* src, void* dst) {
  return ::InetPtonA(af, src, dst);
}

// inet_ntoa: available in winsock2.h.
inline const char* platformInetNtoa(struct in_addr in) {
  return ::inet_ntoa(in);
}

}  // namespace platform
}  // namespace syntext

// ============================================================
// POSIX (Linux / macOS)
// ============================================================
#else  // !_WIN32

#  include <arpa/inet.h>
#  include <fcntl.h>
#  include <netinet/in.h>
#  include <poll.h>
#  include <sys/socket.h>
#  include <unistd.h>
#  include <cerrno>

namespace syntext {
namespace platform {

// ---- Type aliases ---------------------------------------------------------
using PlatformSocket     = int;
using platform_ssize_t   = ssize_t;
using platform_socklen_t = socklen_t;

static constexpr PlatformSocket INVALID_PLATFORM_SOCKET = -1;

static constexpr int PLATFORM_MSG_NOSIGNAL = MSG_NOSIGNAL;

// ---- Init / cleanup (no-ops on POSIX) -------------------------------------
inline bool platformInit()    { return true; }
inline void platformCleanup() {}

// ---- Error reporting -------------------------------------------------------
inline int  platformGetError()  { return errno; }
inline bool platformWouldBlock() {
  return errno == EWOULDBLOCK || errno == EAGAIN || errno == EINPROGRESS;
}

// ---- Socket creation / teardown -------------------------------------------
inline PlatformSocket platformSocket(int af, int type, int proto) {
  return ::socket(af, type, proto);
}

inline void platformClose(PlatformSocket fd) {
  if (fd != INVALID_PLATFORM_SOCKET) {
    ::close(fd);
  }
}

// ---- Non-blocking mode ----------------------------------------------------
inline bool platformSetNonBlocking(PlatformSocket fd) {
  int flags = ::fcntl(fd, F_GETFL, 0);
  if (flags < 0) return false;
  return ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

// ---- Bind / listen / accept -----------------------------------------------
inline int platformBind(PlatformSocket fd,
                        const struct sockaddr* addr, int addrlen) {
  return ::bind(fd, addr, static_cast<socklen_t>(addrlen));
}

inline int platformListen(PlatformSocket fd, int backlog) {
  return ::listen(fd, backlog);
}

inline PlatformSocket platformAccept(PlatformSocket fd,
                                      struct sockaddr* addr,
                                      platform_socklen_t* addrlen) {
  return ::accept(fd, addr, addrlen);
}

// ---- Connect ---------------------------------------------------------------
inline int platformConnect(PlatformSocket fd,
                           const struct sockaddr* addr, int addrlen) {
  return ::connect(fd, addr, static_cast<socklen_t>(addrlen));
}

// ---- Send / recv -----------------------------------------------------------
inline platform_ssize_t platformSend(PlatformSocket fd,
                                      const void* buf, int len, int flags) {
  return ::send(fd, buf, static_cast<size_t>(len), flags);
}

inline platform_ssize_t platformRecv(PlatformSocket fd,
                                      void* buf, int len, int flags) {
  return ::recv(fd, buf, static_cast<size_t>(len), flags);
}

inline platform_ssize_t platformSendTo(PlatformSocket fd,
                                        const void* buf, int len, int flags,
                                        const struct sockaddr* dest,
                                        platform_socklen_t destlen) {
  return ::sendto(fd, buf, static_cast<size_t>(len), flags, dest, destlen);
}

inline platform_ssize_t platformRecvFrom(PlatformSocket fd,
                                          void* buf, int len, int flags,
                                          struct sockaddr* src,
                                          platform_socklen_t* srclen) {
  return ::recvfrom(fd, buf, static_cast<size_t>(len), flags, src, srclen);
}

// ---- setsockopt ------------------------------------------------------------
inline int platformSetSockOpt(PlatformSocket fd, int level, int optname,
                               const void* optval, platform_socklen_t optlen) {
  return ::setsockopt(fd, level, optname, optval, optlen);
}

// ---- Poll ------------------------------------------------------------------
inline int platformPoll(struct pollfd* fds, nfds_t nfds, int timeout_ms) {
  return ::poll(fds, nfds, timeout_ms);
}

using platform_pollfd = struct pollfd;

// ---- Address helpers -------------------------------------------------------
inline int platformInetPton(int af, const char* src, void* dst) {
  return ::inet_pton(af, src, dst);
}

inline const char* platformInetNtoa(struct in_addr in) {
  return ::inet_ntoa(in);
}

}  // namespace platform
}  // namespace syntext

#endif  // _WIN32
