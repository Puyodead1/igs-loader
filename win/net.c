/*
 * IGS loader, Windows core: IP sockets.
 *
 * TCP is on Winsock, only with IGS_NET=host[:port] (port 10000), the game's
 * own server (igs-online): connects to the ports in IGS_NET_PORTS (default
 * 10000, We Dance's game server) go to host:port whatever address the game
 * asked for; every other connection and name lookup is refused, as on Linux
 * (core.c).
 *
 * UDP is the cabinet alone on its LAN, in-process: a datagram socket binds a
 * port (0: one of its own); sendto an address of this cabinet (127.x,
 * 0.0.0.0, the 172.16.1.1 its ifconfig reports) queues the datagram on the
 * socket bound to that port, and to any other address goes nowhere;
 * recvfrom takes from the queue (blocking, or EAGAIN).  The Speed engine's
 * RakNet needs it: a solo race's results go from the game to itself over
 * the cabinet network (speed.net.sendServerCommand -> pushNetCmd).  The
 * interface ioctls answer as before (SIOCGIFFLAGS: up and running; no MAC).
 *
 * Sockets are Linux fds of type FD_SOCK in libc_sys.c; read/write/close,
 * fcntl(O_NONBLOCK), ioctl, poll and select (x11.c) come here for them.
 */
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <stdlib.h>
#include <string.h>

#include "lx.h"

#define L_EAGAIN       11
#define L_EACCES       13
#define L_ENODEV       19
#define L_EINVAL       22
#define L_ENOTTY       25
#define L_ENOTSOCK     88
#define L_ECONNABORTED 103
#define L_ECONNRESET   104
#define L_EISCONN      106
#define L_ENOTCONN     107
#define L_ECONNREFUSED 111
#define L_EHOSTUNREACH 113
#define L_EALREADY     114
#define L_EINPROGRESS  115
#define L_ENETUNREACH  101

#define L_EADDRINUSE   98

#define NFD 1024
struct dgram { struct dgram *next; struct sockaddr_in from; int n; char data[]; };
static struct {
    SOCKET s;
    int dgram, nonblock;
    /* datagram sockets: the bound port (host order), the connected peer, the queue */
    int port;
    struct sockaddr_in peer;
    struct dgram *head, *tail;
    int queued;
} socks[NFD];

static CRITICAL_SECTION dlock;
static CONDITION_VARIABLE dcond;
static INIT_ONCE donce = INIT_ONCE_STATIC_INIT;

static BOOL CALLBACK dinit(PINIT_ONCE o, void *p, void **c)
{
    (void)o; (void)p; (void)c;
    InitializeCriticalSection(&dlock);
    InitializeConditionVariable(&dcond);
    return TRUE;
}

static void dlock_on(void)
{
    InitOnceExecuteOnce(&donce, dinit, NULL, NULL);
    EnterCriticalSection(&dlock);
}

static void dlock_off(void) { LeaveCriticalSection(&dlock); }

static struct {
    int on;                       /* 1 on, -1 IGS_NET given but unusable */
    struct sockaddr_in to;
    int ports[8], nports;
} net;

static void net_init(void)
{
    static int done;
    const char *v = lx_env("IGS_NET", NULL), *p;
    char host[256];
    struct addrinfo hints, *ai;
    WSADATA wsa;
    int port = 10000;
    size_t n;

    if (done++ || !v)
        return;
    if (WSAStartup(MAKEWORD(2, 2), &wsa)) {
        LOG("IGS_NET: no Winsock; no network");
        net.on = -1;
        return;
    }
    p = strrchr(v, ':');
    n = p ? (size_t)(p - v) : strlen(v);
    if (n >= sizeof(host))
        n = sizeof(host) - 1;
    memcpy(host, v, n);
    host[n] = 0;
    if (p)
        port = atoi(p + 1);
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, NULL, &hints, &ai)) {
        LOG("IGS_NET=%s: cannot resolve %s; no network", v, host);
        net.on = -1;
        return;
    }
    net.to = *(struct sockaddr_in *)ai->ai_addr;
    net.to.sin_port = htons((u_short)port);
    freeaddrinfo(ai);
    for (p = lx_env("IGS_NET_PORTS", "10000"); *p && net.nports < 8; ) {
        net.ports[net.nports++] = atoi(p);
        p += strcspn(p, ",");
        p += *p == ',';
    }
    net.on = 1;
    LOG("IGS_NET: game server %s:%d (ports %s)", inet_ntoa(net.to.sin_addr), port,
        lx_env("IGS_NET_PORTS", "10000"));
}

int lx_net_on(void)
{
    net_init();
    return net.on == 1;
}

static int lerr(int e)
{
    switch (e) {
    case WSAEWOULDBLOCK: return L_EAGAIN;
    case WSAEINPROGRESS: return L_EINPROGRESS;
    case WSAEALREADY: return L_EALREADY;
    case WSAECONNREFUSED: return L_ECONNREFUSED;
    case WSAECONNRESET: return L_ECONNRESET;
    case WSAECONNABORTED: return L_ECONNABORTED;
    case WSAENOTCONN: return L_ENOTCONN;
    case WSAEISCONN: return L_EISCONN;
    case WSAETIMEDOUT: return L_ETIMEDOUT;
    case WSAENETUNREACH: case WSAENETDOWN: return L_ENETUNREACH;
    case WSAEHOSTUNREACH: return L_EHOSTUNREACH;
    case WSAEINVAL: return L_EINVAL;
    case WSAENOTSOCK: return L_ENOTSOCK;
    case 0: return 0;
    default: return 5;                         /* EIO */
    }
}

static int fail(int e)
{
    SET_ERRNO(e);
    return -1;
}

/* 1 if fd is a socket, else 0 (errno set) */
static int sock_of(int fd)
{
    if (fd < 0 || fd >= NFD || !lx_fd_is_sock(fd)) {
        SET_ERRNO(L_ENOTSOCK);
        return 0;
    }
    return 1;
}

int l_socket(int d, int t, int p)
{
    int fd;
    SOCKET s = INVALID_SOCKET;

    (void)p;
    if (d != AF_INET || ((t & 0xf) != 1 && (t & 0xf) != 2) || ((t & 0xf) == 1 && !lx_net_on())) {
        static int once;

        if (!once++)
            LOG("socket(%d, %d, %d): no network (logged once)", d, t, p);
        return fail(lx_net_on() || d == AF_INET ? L_EACCES : L_EAFNOSUPPORT);
    }
    if ((t & 0xf) == 1 && (s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP)) == INVALID_SOCKET)
        return fail(lerr(WSAGetLastError()));
    if ((fd = lx_fd_new_sock()) < 0) {
        if (s != INVALID_SOCKET)
            closesocket(s);
        return -1;
    }
    dlock_on();
    memset(&socks[fd], 0, sizeof(socks[fd]));
    socks[fd].s = s;
    socks[fd].dgram = (t & 0xf) == 2;
    dlock_off();
    if (t & L_O_NONBLOCK)                       /* SOCK_NONBLOCK */
        lx_net_nonblock(fd, 1);
    return fd;
}

/* ---- in-process datagrams ---------------------------------------------------------------- */

/* an address of this cabinet: loopback, any, its own (ifconfig's 172.16.1.1) */
static int ours(uint32_t a)
{
    a = ntohl(a);
    return (a >> 24) == 127 || a == 0 || a == 0xAC100101u;
}

/* the datagram socket bound to port, or -1 (dlock held) */
static int bound(int port)
{
    int fd;

    for (fd = 0; fd < NFD; fd++)
        if (socks[fd].dgram && socks[fd].port == port && lx_fd_is_sock(fd))
            return fd;
    return -1;
}

int l_bind(int fd, const struct sockaddr_in *a, int len)
{
    int port;

    (void)len;
    if (!sock_of(fd))
        return -1;
    if (!socks[fd].dgram || !a || a->sin_family != AF_INET)
        return fail(L_EACCES);                  /* TCP servers: none */
    dlock_on();
    port = ntohs(a->sin_port);
    if (!port)                                  /* an ephemeral port */
        for (port = 49152; port < 65535 && bound(port) >= 0; port++)
            ;
    if (bound(port) >= 0 && bound(port) != fd) {
        dlock_off();
        return fail(L_EADDRINUSE);
    }
    socks[fd].port = port;
    dlock_off();
    if (lx_verbose)
        LOG("udp: fd %d bound to port %d", fd, port);
    return 0;
}

static int dg_send(int fd, const void *buf, size_t n, const struct sockaddr_in *to)
{
    int dst;

    if (!to || to->sin_family != AF_INET)
        return fail(L_ENOTCONN);
    dlock_on();
    if (!socks[fd].port) {                      /* sending binds, as on Linux */
        int p = 49152;

        while (bound(p) >= 0)
            p++;
        socks[fd].port = p;
    }
    if (ours(to->sin_addr.s_addr) && (dst = bound(ntohs(to->sin_port))) >= 0 && socks[dst].queued < 4096) {
        struct dgram *d = malloc(sizeof(*d) + n);

        if (d) {
            memset(&d->from, 0, sizeof(d->from));
            d->from.sin_family = AF_INET;
            d->from.sin_addr.s_addr = htonl(0x7F000001u);
            d->from.sin_port = htons((u_short)socks[fd].port);
            d->n = (int)n;
            memcpy(d->data, buf, n);
            d->next = NULL;
            if (socks[dst].tail)
                socks[dst].tail->next = d;
            else
                socks[dst].head = d;
            socks[dst].tail = d;
            socks[dst].queued++;
            WakeAllConditionVariable(&dcond);
        }
    }
    dlock_off();
    return (int)n;                              /* anywhere else: gone, as on a quiet LAN */
}

static int dg_recv(int fd, void *buf, size_t n, int flags, struct sockaddr_in *from, int *len)
{
    struct dgram *d;
    int r;
    dlock_on();
    while (!(d = socks[fd].head)) {
        if (socks[fd].nonblock || (flags & 0x40) || !lx_fd_is_sock(fd) || !socks[fd].dgram) {   /* MSG_DONTWAIT */
            dlock_off();
            return fail(L_EAGAIN);
        }
        SleepConditionVariableCS(&dcond, &dlock, 100);
    }
    r = d->n < (int)n ? d->n : (int)n;
    memcpy(buf, d->data, (size_t)r);
    if (from && len && *len >= (int)sizeof(*from)) {
        *from = d->from;
        *len = sizeof(*from);
    }
    if (!(flags & 2)) {                         /* not MSG_PEEK */
        socks[fd].head = d->next;
        if (!d->next)
            socks[fd].tail = NULL;
        socks[fd].queued--;
        free(d);
    }
    dlock_off();
    return r;
}

int l_connect(int fd, const struct sockaddr_in *a, int len)
{
    int i, port;

    (void)len;
    if (!sock_of(fd))
        return -1;
    if (socks[fd].dgram && a && a->sin_family == AF_INET) {
        socks[fd].peer = *a;                    /* send / recv without an address */
        return 0;
    }
    if (socks[fd].dgram || !a || a->sin_family != AF_INET)
        return fail(L_ECONNREFUSED);
    port = ntohs(a->sin_port);
    for (i = 0; i < net.nports; i++)
        if (port == net.ports[i])
            break;
    if (i == net.nports) {
        LOG("connect %s:%d refused (IGS_NET)", inet_ntoa(a->sin_addr), port);
        return fail(L_ECONNREFUSED);
    }
    LOG("connect %s:%d -> IGS_NET", inet_ntoa(a->sin_addr), port);
    if (connect(socks[fd].s, (const struct sockaddr *)&net.to, sizeof(net.to))) {
        int e = WSAGetLastError();

        return fail(e == WSAEWOULDBLOCK ? L_EINPROGRESS : lerr(e));
    }
    return 0;
}

int l_send(int fd, const void *buf, size_t n, int flags)
{
    int r;

    (void)flags;                                /* MSG_NOSIGNAL, MSG_DONTWAIT */
    if (!sock_of(fd))
        return -1;
    if (socks[fd].dgram)
        return dg_send(fd, buf, n, socks[fd].peer.sin_family ? &socks[fd].peer : NULL);
    r = send(socks[fd].s, buf, (int)n, 0);
    return r == SOCKET_ERROR ? fail(lerr(WSAGetLastError())) : r;
}

int l_recv(int fd, void *buf, size_t n, int flags)
{
    int r;

    if (!sock_of(fd))
        return -1;
    if (socks[fd].dgram)
        return dg_recv(fd, buf, n, flags, NULL, NULL);
    r = recv(socks[fd].s, buf, (int)n, flags & 2 ? MSG_PEEK : 0);
    return r == SOCKET_ERROR ? fail(lerr(WSAGetLastError())) : r;
}

int l_sendto(int fd, const void *buf, size_t n, int flags, const void *a, int len)
{
    (void)len;
    if (!sock_of(fd))
        return -1;
    if (socks[fd].dgram)
        return dg_send(fd, buf, n, a ? a : socks[fd].peer.sin_family ? &socks[fd].peer : NULL);
    if (a)
        return fail(L_EACCES);
    return l_send(fd, buf, n, flags);
}

int l_recvfrom(int fd, void *buf, size_t n, int flags, void *a, int *len)
{
    if (!sock_of(fd))
        return -1;
    if (socks[fd].dgram)
        return dg_recv(fd, buf, n, flags, a, len);
    if (a && len)
        memset(a, 0, (size_t)*len);
    return l_recv(fd, buf, n, flags);
}

int l_shutdown(int fd, int how)
{
    if (!sock_of(fd))
        return -1;
    if (!socks[fd].dgram)
        shutdown(socks[fd].s, how);
    return 0;
}

/* Linux SOL_SOCKET (1) and IPPROTO_TCP (6) options to Winsock's */
int l_setsockopt(int fd, int level, int opt, const void *v, int len)
{
    int wl = SOL_SOCKET, wo;

    if (!sock_of(fd))
        return -1;
    if (socks[fd].dgram)
        return 0;
    if (level == 1) {
        switch (opt) {
        case 2: wo = SO_REUSEADDR; break;
        case 7: wo = SO_SNDBUF; break;
        case 8: wo = SO_RCVBUF; break;
        case 9: wo = SO_KEEPALIVE; break;
        default: return 0;                      /* linger, timeouts: not needed */
        }
    } else if (level == 6 && opt == 1) {
        wl = IPPROTO_TCP;
        wo = TCP_NODELAY;
    } else
        return 0;
    setsockopt(socks[fd].s, wl, wo, v, len);
    return 0;
}

int l_getsockopt(int fd, int level, int opt, void *v, int *len)
{
    int x = 0, xl = sizeof(x);

    if (!sock_of(fd))
        return -1;
    if (level == 1 && opt == 4 && v) {          /* SO_ERROR */
        if (!socks[fd].dgram)
            getsockopt(socks[fd].s, SOL_SOCKET, SO_ERROR, (char *)&x, &xl);
        *(int *)v = lerr(x);
        if (len)
            *len = 4;
        return 0;
    }
    if (v && len && *len >= 4) {
        *(int *)v = 0;
        *len = 4;
    }
    return 0;
}

int l_getpeername(int fd, void *a, int *len)
{
    if (!sock_of(fd))
        return -1;
    if (socks[fd].dgram || getpeername(socks[fd].s, a, len))
        return fail(L_ENOTCONN);
    return 0;
}

int l_getsockname(int fd, void *a, int *len)
{
    if (!sock_of(fd))
        return -1;
    if (socks[fd].dgram && a && len && *len >= (int)sizeof(struct sockaddr_in)) {
        struct sockaddr_in *s = a;

        memset(s, 0, sizeof(*s));
        s->sin_family = AF_INET;
        s->sin_port = htons((u_short)socks[fd].port);
        *len = sizeof(*s);
        return 0;
    }
    if (socks[fd].dgram || getsockname(socks[fd].s, a, len))
        return fail(L_EINVAL);
    return 0;
}

/* ---- the fd side (libc_sys.c) ------------------------------------------------------------ */

int lx_net_read(int fd, void *buf, size_t n) { return l_recv(fd, buf, n, 0); }
int lx_net_write(int fd, const void *buf, size_t n) { return l_send(fd, buf, n, 0); }

void lx_net_close(int fd)
{
    struct dgram *d, *n;

    if (fd < 0 || fd >= NFD)
        return;
    if (socks[fd].s != INVALID_SOCKET && socks[fd].s)
        closesocket(socks[fd].s);
    dlock_on();
    for (d = socks[fd].head; d; d = n) {
        n = d->next;
        free(d);
    }
    memset(&socks[fd], 0, sizeof(socks[fd]));
    WakeAllConditionVariable(&dcond);          /* a recvfrom waiting on it returns */
    dlock_off();
}

int lx_net_nonblocking(int fd) { return fd >= 0 && fd < NFD && socks[fd].nonblock; }

void lx_net_nonblock(int fd, int on)
{
    u_long v = on != 0;

    socks[fd].nonblock = on != 0;
    if (!socks[fd].dgram)
        ioctlsocket(socks[fd].s, FIONBIO, &v);
}

int lx_net_ioctl(int fd, unsigned long req, void *arg)
{
    u_long v = 0;
    switch (req) {
    case 0x541B:                                /* FIONREAD: a datagram socket's next one */
        if (!socks[fd].dgram)
            ioctlsocket(socks[fd].s, FIONREAD, &v);
        else {
            dlock_on();
            v = socks[fd].head ? (u_long)socks[fd].head->n : 0;
            dlock_off();
        }
        *(int *)arg = (int)v;
        return 0;
    case 0x5421:                                /* FIONBIO */
        lx_net_nonblock(fd, *(int *)arg);
        return 0;
    case 0x8913:                                /* SIOCGIFFLAGS: ifreq +16 */
        *(short *)((char *)arg + 16) = 0x41;    /* IFF_UP | IFF_RUNNING */
        return 0;
    default:                                    /* SIOCGIFHWADDR, addresses: none */
        return fail(req >= 0x8900 && req < 0x8a00 ? L_ENODEV : L_ENOTTY);
    }
}

/*
 * select() over the socket fds among n, without waiting: ready bits into
 * the out sets (Linux fd_set layout), the count returned.  A failed
 * non-blocking connect is writable, as on Linux (Winsock: exceptfds).
 */
int lx_net_select(int n, const uint32_t *rd, const uint32_t *wr, uint32_t *ord, uint32_t *owr)
{
    fd_set r, w, e;
    TIMEVAL tv = { 0, 0 };
    int fd, count = 0, any = 0;

    FD_ZERO(&r);
    FD_ZERO(&w);
    FD_ZERO(&e);
    for (fd = 0; fd < n && fd < NFD; fd++) {
        int inr = rd && rd[fd / 32] >> (fd % 32) & 1, inw = wr && wr[fd / 32] >> (fd % 32) & 1;

        if (!(inr || inw) || !lx_fd_is_sock(fd))
            continue;
        if (socks[fd].dgram) {                  /* readable with a datagram queued, always writable */
            if (inr && socks[fd].head) {
                ord[fd / 32] |= 1u << (fd % 32);
                count++;
            }
            if (inw) {
                owr[fd / 32] |= 1u << (fd % 32);
                count++;
            }
            continue;
        }
        if (inr)
            FD_SET(socks[fd].s, &r);
        if (inw) {
            FD_SET(socks[fd].s, &w);
            FD_SET(socks[fd].s, &e);
        }
        any = 1;
    }
    if (!any || select(0, &r, &w, &e, &tv) <= 0)
        return count;
    for (fd = 0; fd < n && fd < NFD; fd++) {
        if (!lx_fd_is_sock(fd) || socks[fd].dgram)
            continue;
        if (rd && rd[fd / 32] >> (fd % 32) & 1 && FD_ISSET(socks[fd].s, &r)) {
            ord[fd / 32] |= 1u << (fd % 32);
            count++;
        }
        if (wr && wr[fd / 32] >> (fd % 32) & 1 && (FD_ISSET(socks[fd].s, &w) || FD_ISSET(socks[fd].s, &e))) {
            owr[fd / 32] |= 1u << (fd % 32);
            count++;
        }
    }
    return count;
}

/* poll() for socket fds: Linux POLLIN 1, POLLOUT 4, POLLERR 8 */
struct l_pollfd { int fd; short events, revents; };

int lx_net_poll(struct l_pollfd *p, unsigned n, int timeout)
{
    double end = lx_now() + (timeout < 0 ? 1e9 : timeout / 1000.0);
    uint32_t rd[32], wr[32], ord[32], owr[32];
    unsigned i;
    int count;

    for (;;) {
        memset(rd, 0, sizeof(rd));
        memset(wr, 0, sizeof(wr));
        memset(ord, 0, sizeof(ord));
        memset(owr, 0, sizeof(owr));
        for (i = 0; i < n; i++) {
            p[i].revents = 0;
            if (p[i].fd < 0 || p[i].fd >= NFD)
                continue;
            if (p[i].events & 1)
                rd[p[i].fd / 32] |= 1u << (p[i].fd % 32);
            if (p[i].events & 4)
                wr[p[i].fd / 32] |= 1u << (p[i].fd % 32);
        }
        lx_net_select(NFD, rd, wr, ord, owr);
        count = 0;
        for (i = 0; i < n; i++) {
            int fd = p[i].fd;

            if (fd < 0 || fd >= NFD)
                continue;
            if (!lx_fd_is_sock(fd))
                p[i].revents = p[i].events & 5;  /* other fds: ready */
            else {
                if (ord[fd / 32] >> (fd % 32) & 1)
                    p[i].revents |= 1;
                if (owr[fd / 32] >> (fd % 32) & 1)
                    p[i].revents |= 4;
            }
            count += p[i].revents != 0;
        }
        if (count || lx_now() >= end)
            return count;
        lx_usleep(1000);
    }
}

const struct nsym net_syms[] = {
    N(socket), N(bind), N(connect), N(send), N(recv), N(sendto), N(recvfrom), N(shutdown),
    N(setsockopt), N(getsockopt), N(getpeername), N(getsockname),
    { NULL, NULL },
};
