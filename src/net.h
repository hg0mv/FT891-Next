/* Hálózati fejlécek és apró eltérések: Linux és Windows (Winsock) */
#ifndef NET_H
#define NET_H
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#define MSG_NOSIGNAL 0
#define SOCK_CLOEXEC 0
#define sock_close(s) closesocket((SOCKET)(s))
#define sock_poll(p, n, t) WSAPoll((p), (ULONG)(n), (t))
static inline int sock_accept(int ls, struct sockaddr *a, socklen_t *l) {
    SOCKET s = accept((SOCKET)ls, a, l);
    return s == INVALID_SOCKET ? -1 : (int)s;
}
static inline void sock_rcvtimeo(int s, int ms) {
    DWORD t = (DWORD)ms;
    setsockopt((SOCKET)s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&t, sizeof t);
}
/* Windowson a SO_REUSEADDR más porton ülő programot is kiszoríthatna */
static inline void sock_reuse(int s) {
    int one = 1;
    setsockopt((SOCKET)s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char *)&one, sizeof one);
}
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#define sock_close(s) close(s)
#define sock_poll(p, n, t) poll((p), (n), (t))
static inline int sock_accept(int ls, struct sockaddr *a, socklen_t *l) { return accept4(ls, a, l, SOCK_CLOEXEC); }
static inline void sock_rcvtimeo(int s, int ms) {
    struct timeval tv = {ms / 1000, (ms % 1000) * 1000};
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
}
static inline void sock_reuse(int s) {
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
}
#endif
static inline void sock_nodelay(int s) {
    int one = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const void *)&one, sizeof one);
}
#endif
