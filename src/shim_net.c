/*
 * Networking shims for the SDK's Ethernet device path (asio on epoll).
 *
 *  - epoll is emulated with kqueue.
 *  - eventfd/timerfd are reported unavailable; asio falls back to a pipe
 *    interrupter and epoll_wait timeouts.
 *  - Socket constants, sockaddr layout (macOS has sa_len) and errno differ.
 */
#include "shim.h"

#include <errno.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <string.h>
#include <sys/event.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

/* ---- epoll over kqueue ------------------------------------------------- */

enum {
    LX_EPOLLIN = 0x001, LX_EPOLLPRI = 0x002, LX_EPOLLOUT = 0x004, LX_EPOLLERR = 0x008,
    LX_EPOLLHUP = 0x010, LX_EPOLLRDHUP = 0x2000, LX_EPOLLONESHOT = 1u << 30,
    LX_EPOLLET = 1u << 31,
    LX_EPOLL_CTL_ADD = 1, LX_EPOLL_CTL_DEL = 2, LX_EPOLL_CTL_MOD = 3,
    LX_EPOLL_CLOEXEC = 0x80000,
};

struct lx_epoll_event { /* not packed on aarch64 */
    uint32_t events;
    uint64_t data;
};

/* HTRAAPI_TRACE_NET=1: log socket reads and epoll wakeups with timestamps. */
static int net_trace = -1;
static double trace_t0;
#define NET_TRACE(...)                                                                \
    do {                                                                              \
        if (net_trace < 0) {                                                          \
            net_trace = getenv("HTRAAPI_TRACE_NET") != NULL;                          \
            struct timespec ts_;                                                      \
            clock_gettime(CLOCK_MONOTONIC, &ts_);                                     \
            trace_t0 = ts_.tv_sec + ts_.tv_nsec * 1e-9;                               \
        }                                                                             \
        if (net_trace) {                                                              \
            struct timespec ts_;                                                      \
            clock_gettime(CLOCK_MONOTONIC, &ts_);                                     \
            fprintf(stderr, "[net %8.3f] ", ts_.tv_sec + ts_.tv_nsec * 1e-9 - trace_t0); \
            fprintf(stderr, __VA_ARGS__);                                             \
        }                                                                             \
    } while (0)

static int lx_epoll_create1(int flags) {
    int kq = kqueue();
    if (kq < 0) return (int)shim_fail();
    if (flags & LX_EPOLL_CLOEXEC) fcntl(kq, F_SETFD, FD_CLOEXEC);
    return kq;
}
static int lx_epoll_create(int size) {
    (void)size;
    return lx_epoll_create1(0);
}

static int kq_change(int kq, int fd, int16_t filter, uint16_t flags, uint64_t data) {
    struct kevent kev;
    EV_SET(&kev, fd, filter, flags, 0, 0, (void *)(uintptr_t)data);
    return kevent(kq, &kev, 1, NULL, 0, NULL);
}

static int lx_epoll_ctl(int kq, int op, int fd, struct lx_epoll_event *ev) {
    if (op == LX_EPOLL_CTL_DEL) {
        kq_change(kq, fd, EVFILT_READ, EV_DELETE, 0);
        kq_change(kq, fd, EVFILT_WRITE, EV_DELETE, 0);
        return 0;
    }
    if (op != LX_EPOLL_CTL_ADD && op != LX_EPOLL_CTL_MOD) {
        errno = EINVAL;
        return -1;
    }
    uint16_t common = EV_ADD | EV_ENABLE;
    if (ev->events & LX_EPOLLET) common |= EV_CLEAR;
    if (ev->events & LX_EPOLLONESHOT) common |= EV_ONESHOT;
    int want_read = (ev->events & (LX_EPOLLIN | LX_EPOLLPRI | LX_EPOLLRDHUP)) != 0;
    int want_write = (ev->events & LX_EPOLLOUT) != 0;
    if (want_read) {
        if (kq_change(kq, fd, EVFILT_READ, common, ev->data) < 0) return (int)shim_fail();
    } else if (op == LX_EPOLL_CTL_MOD) {
        kq_change(kq, fd, EVFILT_READ, EV_DELETE, 0);
    }
    if (want_write) {
        if (kq_change(kq, fd, EVFILT_WRITE, common, ev->data) < 0) return (int)shim_fail();
    } else if (op == LX_EPOLL_CTL_MOD) {
        kq_change(kq, fd, EVFILT_WRITE, EV_DELETE, 0);
    }
    return 0;
}

static int lx_epoll_wait(int kq, struct lx_epoll_event *out, int max, int timeout_ms) {
    struct kevent kev[64];
    if (max > 64) max = 64;
    struct timespec ts, *tp = NULL;
    if (timeout_ms >= 0) {
        ts.tv_sec = timeout_ms / 1000;
        ts.tv_nsec = (long)(timeout_ms % 1000) * 1000000L;
        tp = &ts;
    }
    int n = kevent(kq, NULL, 0, kev, max, tp);
    if (n < 0) return (int)shim_fail();
    for (int i = 0; i < n; i++)
        NET_TRACE("epoll_wait(%d ms): fd %lu %s%s%s data %ld\n", timeout_ms, (unsigned long)kev[i].ident,
                  kev[i].filter == EVFILT_READ ? "READ" : kev[i].filter == EVFILT_WRITE ? "WRITE" : "?",
                  kev[i].flags & EV_EOF ? " EOF" : "", kev[i].flags & EV_ERROR ? " ERROR" : "",
                  (long)kev[i].data);
    if (!n) NET_TRACE("epoll_wait(%d ms): timeout\n", timeout_ms);
    for (int i = 0; i < n; i++) {
        uint32_t e = 0;
        if (kev[i].flags & EV_ERROR) e |= LX_EPOLLERR;
        if (kev[i].filter == EVFILT_READ) {
            e |= LX_EPOLLIN;
            if (kev[i].flags & EV_EOF) e |= LX_EPOLLRDHUP;
        } else if (kev[i].filter == EVFILT_WRITE) {
            e |= LX_EPOLLOUT;
            if (kev[i].flags & EV_EOF) e |= LX_EPOLLHUP;
        }
        if ((kev[i].flags & EV_EOF) && kev[i].fflags) e |= LX_EPOLLERR;
        out[i].events = e;
        out[i].data = (uint64_t)(uintptr_t)kev[i].udata;
    }
    return n;
}

static int lx_unavailable(void) {
    errno = 38; /* Linux ENOSYS */
    return -1;
}

/* ---- sockets ----------------------------------------------------------- */

enum {
    LX_AF_INET6 = 10, LX_SOCK_NONBLOCK = 0x800, LX_SOCK_CLOEXEC = 0x80000,
    LX_SOL_SOCKET = 1, LX_MSG_DONTWAIT = 0x40, LX_MSG_NOSIGNAL = 0x4000,
};

static int af_from_linux(int af) { return af == LX_AF_INET6 ? AF_INET6 : af; }
static int af_to_linux(int af) { return af == AF_INET6 ? LX_AF_INET6 : af; }

static int lx_socket(int domain, int type, int proto) {
    int nonblock = type & LX_SOCK_NONBLOCK, cloexec = type & LX_SOCK_CLOEXEC;
    int fd = socket(af_from_linux(domain), type & ~(LX_SOCK_NONBLOCK | LX_SOCK_CLOEXEC), proto);
    if (fd < 0) return (int)shim_fail();
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
    /* HTRAAPI_NET_IF=en7 pins every socket to that network interface. Needed when two
     * interfaces are on the same subnet (the analyzer's Ethernet adapter and a Wi-Fi network
     * that also uses 192.168.1.x): the routing table then picks one of them, not
     * necessarily the one the analyzer is on. Read at each call so it can be set after
     * the library is loaded. */
    const char *ifname = getenv("HTRAAPI_NET_IF");
    if (ifname && *ifname) {
        unsigned idx = if_nametoindex(ifname);
        int r = -1;
        if (idx) {
            if (af_from_linux(domain) == AF_INET6)
                r = setsockopt(fd, IPPROTO_IPV6, IPV6_BOUND_IF, &idx, sizeof idx);
            else
                r = setsockopt(fd, IPPROTO_IP, IP_BOUND_IF, &idx, sizeof idx);
        }
        NET_TRACE("socket bound to %s (index %u) -> %d\n", ifname, idx, r);
    }
    if (nonblock) fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    if (cloexec) fcntl(fd, F_SETFD, FD_CLOEXEC);
    return fd;
}

/* Linux sockaddr: uint16 family. macOS: uint8 len, uint8 family. */
static socklen_t sa_from_linux(const void *in, socklen_t len, struct sockaddr_storage *out) {
    if (len > sizeof *out) len = sizeof *out;
    memcpy(out, in, len);
    uint16_t fam;
    memcpy(&fam, in, 2);
    out->ss_len = (uint8_t)len;
    out->ss_family = (sa_family_t)af_from_linux(fam);
    return len;
}

static void sa_to_linux(const struct sockaddr_storage *in, socklen_t len, void *out, socklen_t *outlen) {
    if (!out || !outlen) return;
    socklen_t n = len < *outlen ? len : *outlen;
    uint8_t tmp[sizeof(struct sockaddr_storage)];
    memcpy(tmp, in, len);
    uint16_t fam = (uint16_t)af_to_linux(in->ss_family);
    memcpy(tmp, &fam, 2);
    memcpy(out, tmp, n);
    *outlen = len;
}

static int lx_connect(int fd, const void *addr, socklen_t len) {
    struct sockaddr_storage ss;
    socklen_t n = sa_from_linux(addr, len, &ss);
    return connect(fd, (struct sockaddr *)&ss, n) ? (int)shim_fail() : 0;
}

static int lx_bind(int fd, const void *addr, socklen_t len) {
    struct sockaddr_storage ss;
    socklen_t n = sa_from_linux(addr, len, &ss);
    return bind(fd, (struct sockaddr *)&ss, n) ? (int)shim_fail() : 0;
}

static int lx_getsockname(int fd, void *addr, socklen_t *alen) {
    struct sockaddr_storage ss;
    socklen_t sl = sizeof ss;
    if (getsockname(fd, (struct sockaddr *)&ss, &sl)) return (int)shim_fail();
    sa_to_linux(&ss, sl, addr, alen);
    return 0;
}

static int lx_inet_pton(int af, const char *src, void *dst) {
    int r = inet_pton(af_from_linux(af), src, dst);
    return r < 0 ? (int)shim_fail() : r;
}

static const char *lx_inet_ntop(int af, const void *src, char *dst, socklen_t size) {
    const char *r = inet_ntop(af_from_linux(af), src, dst, size);
    if (!r) shim_fail();
    return r;
}

/* getifaddrs: struct ifaddrs has the same layout on both systems, but the
 * sockaddrs it points to, the flag bits and ifa_data do not. Build a Linux
 * copy holding only IPv4/IPv6 entries; each entry is one allocation. */
struct lx_ifaddrs {
    struct lx_ifaddrs *ifa_next;
    char *ifa_name;
    unsigned int ifa_flags;
    void *ifa_addr, *ifa_netmask, *ifa_broadaddr; /* ifa_ifu union */
    void *ifa_data;
};

enum { LX_IFF_MULTICAST = 0x1000 };

static void *sa_copy_linux(const struct sockaddr *sa, int family, uint8_t *out) {
    if (!sa) return NULL;
    size_t n = sa->sa_len < sizeof(struct sockaddr_storage) ? sa->sa_len : sizeof(struct sockaddr_storage);
    memcpy(out, sa, n); /* the rest of out is zero: macOS netmasks may be truncated */
    uint16_t fam = (uint16_t)af_to_linux(family);
    memcpy(out, &fam, 2);
    return out;
}

static int lx_getifaddrs(struct lx_ifaddrs **out) {
    struct ifaddrs *list;
    if (getifaddrs(&list)) return (int)shim_fail();
    struct lx_ifaddrs *head = NULL, **tail = &head;
    const size_t ss = sizeof(struct sockaddr_storage);
    for (struct ifaddrs *i = list; i; i = i->ifa_next) {
        if (!i->ifa_addr || (i->ifa_addr->sa_family != AF_INET && i->ifa_addr->sa_family != AF_INET6))
            continue;
        size_t name_len = strlen(i->ifa_name) + 1;
        uint8_t *block = calloc(1, sizeof(struct lx_ifaddrs) + 3 * ss + name_len);
        if (!block) {
            freeifaddrs(list);
            errno = ENOMEM;
            return (int)shim_fail();
        }
        struct lx_ifaddrs *e = (struct lx_ifaddrs *)block;
        uint8_t *addrs = block + sizeof *e;
        int fam = i->ifa_addr->sa_family;
        e->ifa_name = (char *)(addrs + 3 * ss);
        memcpy(e->ifa_name, i->ifa_name, name_len);
        e->ifa_flags = (i->ifa_flags & 0x3FF) | ((i->ifa_flags & IFF_MULTICAST) ? LX_IFF_MULTICAST : 0);
        e->ifa_addr = sa_copy_linux(i->ifa_addr, fam, addrs);
        e->ifa_netmask = sa_copy_linux(i->ifa_netmask, fam, addrs + ss);
        e->ifa_broadaddr = sa_copy_linux(i->ifa_dstaddr, fam, addrs + 2 * ss);
        *tail = e;
        tail = &e->ifa_next;
    }
    freeifaddrs(list);
    *out = head;
    return 0;
}

static void lx_freeifaddrs(struct lx_ifaddrs *list) {
    while (list) {
        struct lx_ifaddrs *next = list->ifa_next;
        free(list);
        list = next;
    }
}

static int msg_flags(int f) {
    int m = f & (MSG_OOB | MSG_PEEK | MSG_WAITALL);
    if (f & LX_MSG_DONTWAIT) m |= MSG_DONTWAIT;
    return m; /* MSG_NOSIGNAL: SO_NOSIGPIPE is set on every socket */
}

static ssize_t lx_send(int fd, const void *buf, size_t n, int flags) {
    ssize_t r = send(fd, buf, n, msg_flags(flags));
    NET_TRACE("send(fd %d, %zu) -> %zd\n", fd, n, r);
    return r < 0 ? shim_fail() : r;
}
static ssize_t lx_sendto(int fd, const void *buf, size_t n, int flags, const void *addr, socklen_t alen) {
    struct sockaddr_storage ss;
    socklen_t sl = addr ? sa_from_linux(addr, alen, &ss) : 0;
    ssize_t r = sendto(fd, buf, n, msg_flags(flags), addr ? (struct sockaddr *)&ss : NULL, sl);
    return r < 0 ? shim_fail() : r;
}
static ssize_t lx_recv(int fd, void *buf, size_t n, int flags) {
    ssize_t r = recv(fd, buf, n, msg_flags(flags));
    NET_TRACE("recv(fd %d, %zu) -> %zd%s\n", fd, n, r, r < 0 && errno == EAGAIN ? " EAGAIN" : "");
    return r < 0 ? shim_fail() : r;
}
static ssize_t lx_recvfrom(int fd, void *buf, size_t n, int flags, void *addr, socklen_t *alen) {
    struct sockaddr_storage ss;
    socklen_t sl = sizeof ss;
    ssize_t r = recvfrom(fd, buf, n, msg_flags(flags), (struct sockaddr *)&ss, &sl);
    if (r < 0) return shim_fail();
    sa_to_linux(&ss, sl, addr, alen);
    return r;
}
static int lx_shutdown(int fd, int how) {
    int r = shutdown(fd, how);
    NET_TRACE("shutdown(fd %d, %d) -> %d%s\n", fd, how, r, r ? strerror(errno) : "");
    return r ? (int)shim_fail() : 0;
}
static int lx_poll(struct pollfd *fds, nfds_t n, int timeout) {
    int r = poll(fds, n, timeout); /* struct and POLL* bits match */
    return r < 0 ? (int)shim_fail() : r;
}

static int sockopt_from_linux(int *level, int *opt) {
    if (*level == LX_SOL_SOCKET) {
        static const int map[][2] = {
            {2, SO_REUSEADDR}, {3, SO_TYPE}, {4, SO_ERROR}, {5, SO_DONTROUTE},
            {6, SO_BROADCAST}, {7, SO_SNDBUF}, {8, SO_RCVBUF}, {9, SO_KEEPALIVE},
            {10, SO_OOBINLINE}, {13, SO_LINGER}, {15, SO_REUSEPORT}, {18, SO_RCVLOWAT},
            {19, SO_SNDLOWAT}, {20, SO_RCVTIMEO}, {21, SO_SNDTIMEO},
        };
        *level = SOL_SOCKET;
        for (size_t i = 0; i < sizeof map / sizeof map[0]; i++)
            if (map[i][0] == *opt) {
                *opt = map[i][1];
                return 0;
            }
        return -1;
    }
    if (*level == IPPROTO_IP) {
        static const int map[][2] = {
            {1, IP_TOS}, {2, IP_TTL}, {32, IP_MULTICAST_IF}, {33, IP_MULTICAST_TTL},
            {34, IP_MULTICAST_LOOP}, {35, IP_ADD_MEMBERSHIP}, {36, IP_DROP_MEMBERSHIP},
        };
        for (size_t i = 0; i < sizeof map / sizeof map[0]; i++)
            if (map[i][0] == *opt) {
                *opt = map[i][1];
                return 0;
            }
        return -1;
    }
    if (*level == IPPROTO_IPV6 && *opt == 26) *opt = IPV6_V6ONLY;
    return 0; /* IPPROTO_TCP: TCP_NODELAY matches */
}

static int lx_setsockopt(int fd, int level, int opt, const void *val, socklen_t len) {
    if (sockopt_from_linux(&level, &opt)) {
        errno = ENOPROTOOPT;
        return (int)shim_fail();
    }
    int r = setsockopt(fd, level, opt, val, len);
    /* Linux silently caps SO_RCVBUF/SO_SNDBUF at the system maximum; macOS
     * rejects values above kern.ipc.maxsockbuf (ENOBUFS). The SDK asks for
     * 32 MB, so cap it the Linux way instead of leaving the default buffer. */
    if (r && errno == ENOBUFS && level == SOL_SOCKET && (opt == SO_RCVBUF || opt == SO_SNDBUF) &&
        len == sizeof(int)) {
        for (int size = *(const int *)val / 2; r && size >= 65536; size /= 2)
            r = setsockopt(fd, level, opt, &size, sizeof size);
    }
    NET_TRACE("setsockopt(level %d, opt 0x%x, %d) -> %d\n", level, opt,
              len >= sizeof(int) ? *(const int *)val : -1, r);
    return r ? (int)shim_fail() : 0;
}

static int lx_getsockopt(int fd, int level, int opt, void *val, socklen_t *len) {
    int is_error = level == LX_SOL_SOCKET && opt == 4;
    if (sockopt_from_linux(&level, &opt)) {
        errno = ENOPROTOOPT;
        return (int)shim_fail();
    }
    if (getsockopt(fd, level, opt, val, len)) return (int)shim_fail();
    if (is_error && *len >= sizeof(int)) *(int *)val = shim_errno_to_linux(*(int *)val);
    return 0;
}

/* Only the FIONBIO/FIONREAD requests matter off Linux; the PCIe driver
 * ioctls fail as they would on a Linux host without the card. */
static int lx_ioctl(int fd, unsigned long req, void *arg) {
    int r;
    if (req == 0x5421) r = ioctl(fd, FIONBIO, arg);
    else if (req == 0x541B) r = ioctl(fd, FIONREAD, arg);
    else {
        errno = ENOTTY;
        r = -1;
    }
    return r < 0 ? (int)shim_fail() : r;
}

const struct shim_sym shim_net_syms[] = {
    {"epoll_create", lx_epoll_create},
    {"epoll_create1", lx_epoll_create1},
    {"epoll_ctl", lx_epoll_ctl},
    {"epoll_wait", lx_epoll_wait},
    {"eventfd", lx_unavailable},
    {"timerfd_create", lx_unavailable},
    {"timerfd_settime", lx_unavailable},
    {"socket", lx_socket},
    {"connect", lx_connect},
    {"bind", lx_bind},
    {"getsockname", lx_getsockname},
    {"sendto", lx_sendto},
    {"inet_pton", lx_inet_pton},
    {"inet_ntop", lx_inet_ntop},
    {"getifaddrs", lx_getifaddrs},
    {"freeifaddrs", lx_freeifaddrs},
    {"if_nametoindex", if_nametoindex}, /* same ABI */
    {"send", lx_send},
    {"recv", lx_recv},
    {"recvfrom", lx_recvfrom},
    {"shutdown", lx_shutdown},
    {"poll", lx_poll},
    {"setsockopt", lx_setsockopt},
    {"getsockopt", lx_getsockopt},
    {"ioctl", lx_ioctl},
    {NULL, NULL},
};
