/**
 * Very simple NTP client with CAP_SYS_TIME support.
 */

#include <stddef.h>         /* NULL */
#include <stdint.h>         /* uint32_t, uint64_t */
#include <stdio.h>          /* fprintf(), perror() */
#include <stdlib.h>         /* strtol(), malloc(), free() */
#include <string.h>         /* memset(), memcpy() */
#include <unistd.h>         /* close(), alarm() */
#include <math.h>           /* lround() */

#include <arpa/inet.h>      /* htonl(), ntohl(), ntohs() */
#include <sys/types.h>      /* time_t, getaddrinfo(), freeaddrinfo(),
                             * gai_strerror(), send(), recv(),
                             * cap_set_flag() */
#include <sys/time.h>       /* timeval, gettimeofday(), settimeofday(),
                             * adjtime() */
#include <sys/socket.h>     /* socket(), getaddrinfo(), freeaddrinfo(),
                             * gai_strerror(), setsockopt(), bind(),
                             * getsockname(), connect(), send(),
                             * recv() */
#include <netdb.h>          /* addrinfo, getaddrinfo(), freeaddrinfo(),
                             * gai_strerror() */
#include <netinet/in.h>     /* AF_INET, SOCK_STREAM, sockaddr_in */
#include <netinet/tcp.h>    /* IPPROTO_TCP, TCP_NODELAY */

#include <sys/capability.h> /* cap_t, cap_value_t, cap_flag_value_t,
                             * CAP_SYS_TIME, CAP_PERMITTED,
                             * CAP_EFFECTIVE, CAP_SET, CAP_CLEAR,
                             * cap_get_proc(), cap_get_flag(),
                             * cap_clear(), cap_set_flag(),
                             * cap_set_proc(), cap_free() */

#define JAN1970 ((uint32_t)2208988800UL) /* 1970 - 1900 in seconds,
                                          * see RFC 5905, Appendix
                                          * A.4. */

/* 64-bit NTP timestamp: */
struct ntpts64 {
   uint32_t s;  /* seconds */
   uint32_t f;  /* fraction */
};

struct ntppkt {
    uint32_t hdr;
    uint32_t rdelay;
    uint32_t rdisp;
    union {
        uint32_t refid;
        char     refname[4];
    };
    struct ntpts64 tref;
    struct ntpts64 torig;
    struct ntpts64 trecv;
    struct ntpts64 txmit;
};

struct ntpxpkt {
    uint32_t hdr;
    uint32_t rdelay;
    uint32_t rdisp;
    union {
        uint32_t refid;
        char     refname[4];
    };
    struct ntpts64 tref;
    struct ntpts64 torig;
    struct ntpts64 trecv;
    struct ntpts64 txmit;
    uint32_t ext[64];
};

static void norm_tv(struct timeval *tv) {
    if (tv->tv_usec >= 1000000) {
        tv->tv_sec += tv->tv_usec / 1000000;
        tv->tv_usec %= 1000000;
    }
}

static void tv2ntp(const struct timeval *tv, struct ntpts64 *ntp) {
    ntp->s = htonl((uint32_t)tv->tv_sec + JAN1970);
    ntp->f = htonl((uint32_t)lround((double)tv->tv_usec / 1000000
                * 0x10000 * 0x10000));
}

static long double tv2ldbl(const struct timeval *tv) {
    return (long double)tv->tv_sec
        + (long double)tv->tv_usec / 1000000;
}

static long double ntp2ldbl(const struct ntpts64 *ntp) {
    return (long double)((uint32_t)ntohl(ntp->s) - JAN1970)
        + (long double)ntohl(ntp->f) / 0x10000 / 0x10000;
}

static void ldbl2tv(long double t, struct timeval *tv) {
    long double ti = floorl(t);
    if (sizeof(tv->tv_sec) > 4) {
        tv->tv_sec = llroundl(ti);
    } else {
        tv->tv_sec = lroundl(ti);
    }
    tv->tv_usec = lroundl((t - ti) * 1000000);
    norm_tv(tv);
}

static int is_dec_number(const char *str) {
    char *endptr;
    strtol(str, &endptr, 10);
    return endptr[0] == '\0';
}

/* Send all data from the buffer to the specified socket. Return 1 on
 * success, 0 on EOF, -1 on error. Write number of sent bytes into
 * size_t variable pointed to by psentsz, if this pointer is not
 * NULL. */
static int send_buf(int fd, void *buf, size_t sz, size_t *psentsz) {
    ssize_t ssret;
    void *p = buf;
    while (p - buf < sz) {
        ssret = send(fd, p, buf + sz - p, 0);
        if ((ssret == -1) || ssret == 0) goto r;
        p += ssret;
    }
    ssret = 1;
r:  if (psentsz != NULL) *psentsz = p - buf;
    return (int)ssret;
}

/* Receive at least minrecv bytes from the socket into the buffer of
 * size sz. Return 1 on success, 0 on EOF, -1 on error. Write number of
 * received bytes into size_t variable pointed to by prcvdsz, if this
 * pointer is not NULL. */
static int recv_buf(int fd, void *buf, size_t sz, size_t minrecv,
        size_t *prcvdsz) {
    ssize_t ssret;
    void *p = buf;
    if (sz < minrecv) minrecv = sz;
    while (p - buf < minrecv) {
        ssret = recv(fd, p, (buf + sz) - p, 0);
        if (ssret == -1 || ssret == 0) goto r;
        p += ssret;
    }
    ssret = 1;
r:  if (prcvdsz != NULL) *prcvdsz = p - buf;
    return (int)ssret;
}

int main(int argc, char *argv[]) {
    static const char *srvname = "ntp";
    int optval, ret, fd;
    size_t retsz;
    struct addrinfo ah, *pi;
    int dst_family, dst_socktype, dst_protocol;
    socklen_t dst_addrlen, src_addrlen;
    struct sockaddr *pdst_addr, *psrc_addr;
    struct timeval tv0, tv3, tvnew;
    long double t[5];
    struct ntppkt ntpreq;
    struct ntpxpkt ntpreply;
    cap_t caps;
    cap_flag_value_t cap_sys_time0;
    /* 1. Check arguments: */
    if (argc < 2 || argc > 3) {
        fprintf(stderr, "USAGE: %s host [port]\n"
            "  host - NTP server's hostname, IPv4 or IPv6 address\n"
            "         (for example, \"1.debian.pool.ntp.org\")\n"
            "  port - NTP server's decimal port number or service"
                " name\n"
            "         (by default, \"ntp\" is assumed)\n",
            argv[0]);
        goto e1;
    }
    if (argc >= 3) srvname = argv[2];
    /* 2. Drop all caps except CAP_SYS_TIME,p as early as possible: */
    caps = cap_get_proc(); /* get initial set of caps */
    if (caps == NULL) perror("cap_get_proc()");
    else {
        char *captxt = cap_to_text(caps, NULL);
        if (captxt != NULL) {
            /* Print initial set of caps: */
            fprintf(stdout, "caps0 %s\n", captxt);
            cap_free(captxt);
        } else {
            /* Report the error but ignore it: */
            perror("cap_to_text()");
        }
        /* Find out whether CAP_SYS_TIME is initially permitted: */
        ret = cap_get_flag(caps, CAP_SYS_TIME, CAP_PERMITTED,
                &cap_sys_time0);
        if (ret != 0) {
            /* Report the error and assume no CAP_SYS_TIME: */
            perror("cap_get_flag(CAP_SYS_TIME,p)");
            cap_sys_time0 = CAP_CLEAR;
        }
        ret = cap_clear(caps); /* clear all caps */
        if (ret != 0) perror("cap_clear()");
        else {
            /* Leave CAP_SYS_TIME as permitted if it had been
             * permitted initially. Don't enable it yet: */
            if (cap_sys_time0 == CAP_SET) {
                cap_value_t capv[1] = {CAP_SYS_TIME};
                ret = cap_set_flag(caps, CAP_PERMITTED, 1, capv,
                        CAP_SET);
                if (ret != 0) perror("cap_set_flag(CAP_SYS_TIME,p)");
            }
            /* XXX: ideally we should only call cap_set_proc() if new
             * set of caps differs from the old one, but cap_compare()
             * is Linux specific extension, so we avoid it and call
             * cap_set_proc() anyway for the sake of compatibility to
             * POSIX.1e draft: */
            ret = cap_set_proc(caps); /* drop other caps */
            if (ret != 0) perror("cap_set_proc()");
            else {
                /* Print new set of caps: */
                captxt = cap_to_text(caps, NULL);
                if (captxt == NULL) perror("cap_to_text()");
                else {
                    fprintf(stdout, "caps1 %s\n", captxt);
                    cap_free(captxt);
                }
            }
        }
    }
    /* 3. Set alarm for resolv+connect+recv timeout: */
    alarm(3);
    /* 4. Resolve NTP host and port: */
    memset(&ah, 0, sizeof(ah));
    ah.ai_flags     = AI_ADDRCONFIG; /* don't return IPv6 addresses
                                      * on IPv4-only system
                                      * and vice versa */
    if (is_dec_number(srvname)) ah.ai_flags |= AI_NUMERICSERV;
    ah.ai_family    = AF_UNSPEC;     /* accept AF_INET or AF_INET6 */
    ah.ai_socktype  = SOCK_DGRAM;    /* UDP */
    /* BTW, the code will work just fine with SOCK_STREAM, but NTP
     * servers won't, despite the fact that IANA designated port 123
     * to NTP for both UDP and TCP. */
    ah.ai_protocol  = 0;             /* whatever */
    /* set ah's pointers to NULL for systems where NULL != 0: */
    ah.ai_addr      = NULL;
    ah.ai_canonname = NULL;
    ah.ai_next      = NULL;
    ret = getaddrinfo(argv[1], srvname, &ah, &pi);
    if (ret != 0) {
        fprintf(stderr, "getaddrinfo(%s, %s): %s\n", argv[1], srvname,
            gai_strerror(ret));
        goto e1;
    }
    dst_addrlen  = pi->ai_addrlen;
    dst_family   = pi->ai_family;
    dst_socktype = pi->ai_socktype;
    dst_protocol = pi->ai_protocol;
    pdst_addr = malloc((size_t)dst_addrlen);
    if (pdst_addr == NULL) {
        perror("malloc()");
        freeaddrinfo(pi);
        goto e1;
    }
    memcpy(pdst_addr, pi->ai_addr, (size_t)dst_addrlen);
    freeaddrinfo(pi);
    /* 5. Create socket: */
    fd = socket(dst_family, dst_socktype, dst_protocol);
    if (fd < 0) {
        perror("socket()");
        goto e2;
    }
#ifdef TCP_NODELAY
    /* 6. Disable Nagle's delay if we use a TCP socket: */
    if (dst_socktype == SOCK_STREAM) {
        optval = 1;
        ret = setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &optval,
            sizeof(optval));
        if (ret != 0) perror("setsockopt(TCP_NODELAY)");
    }
#endif
    /* 7. Connect to NTP server: */
    ret = connect(fd, pdst_addr, dst_addrlen);
    if (ret != 0) {
        perror("connect()");
        goto e3;
    }
    /* 8. Print local port and remote host:port: */
    psrc_addr = malloc((size_t)dst_addrlen);
    if (psrc_addr == NULL) {
        perror("malloc()");
        goto e3;
    }
    memset(psrc_addr, 0, (size_t)dst_addrlen);
    psrc_addr->sa_family = dst_family;
    src_addrlen = dst_addrlen;
    ret = getsockname(fd, psrc_addr, &src_addrlen);
    if (ret != 0) {
        perror("getsockname()");
        goto e4;
    }
    if (dst_family == AF_INET) {
        struct sockaddr_in *pdst4 = (struct sockaddr_in *)pdst_addr;
        struct sockaddr_in *psrc4 = (struct sockaddr_in *)psrc_addr;
        unsigned long da4 = ntohl(pdst4->sin_addr.s_addr);
        unsigned long sa4 = ntohl(psrc4->sin_addr.s_addr);
        unsigned short sport = ntohs(psrc4->sin_port);
        unsigned short dport = ntohs(pdst4->sin_port);
        fprintf(stdout, "connected %lu.%lu.%lu.%lu:%hu -> %lu.%lu.%lu.%lu:%hu\n",
            sa4 >> 24 & 0xFF, sa4 >> 16 & 0xFF, sa4 >> 8 & 0xFF,
            sa4 & 0xFF, sport,
            da4 >> 24 & 0xFF, da4 >> 16 & 0xFF, da4 >> 8 & 0xFF,
            da4 & 0xFF, dport);
    }
    /* 9. Send NTP request: */
    memset(&tv0, 0, sizeof(tv0));
    ret = gettimeofday(&tv0, NULL);
    if (ret != 0) {
        perror("gettimeofday()");
        goto e4;
    }
    memset(&ntpreq, 0, sizeof(ntpreq));
    ntpreq.hdr = htonl(
        0 << 30       /* LI: no warning (no leap second) */
        | 4 << 27     /* VN: NTPv4 */
        | 3 << 24     /* Mode: client */
        | 0 << 16     /* Stratum: unspecified or invalid */
        | 12 << 8     /* Poll interval: 1h8m16s */
        | -20 & 0xFF  /* Sys clock precision: about 0.954us */
        );
    ntpreq.refname[0] = 'L';
    ntpreq.refname[1] = 'O';
    ntpreq.refname[2] = 'C';
    ntpreq.refname[3] = 'L';
    ntpreq.rdelay  = htonl(0x10000); /* 1sec */
    ntpreq.rdisp   = htonl(0x10000); /* 1sec */
    tv2ntp(&tv0, &ntpreq.txmit);
    ret = send_buf(fd, &ntpreq, sizeof(ntpreq), NULL);
    switch (ret) {
        case -1:
            perror("send()");
            goto e4;
        case 0:
            fprintf(stderr, "send(): EOF\n");
            goto e4;
    }
    /* 10. Receive NTP reply: */
    ret = recv_buf(fd, &ntpreply, sizeof(ntpreply),
            sizeof(ntpreq), &retsz);
    switch (ret) {
        case -1:
            perror("recv()");
            goto e4;
        case 0:
            fprintf(stderr, "recv(): EOF\n");
            goto e4;
    }
    memset(&tv3, 0, sizeof(tv3));
    ret = gettimeofday(&tv3, NULL);
    if (ret != 0) {
        perror("gettimeofday()");
        goto e4;
    }
    fprintf(stdout, "received %u bytes NTPv%u stratum %u reply,"
            " prec %.4fus\n",
            (unsigned)retsz,
            (unsigned)(ntohl(ntpreply.hdr) >> 27 & 0x7),
            (unsigned)(ntohl(ntpreply.hdr) >> 16 & 0xFF),
            pow(2.0, (signed char)(ntohl(ntpreply.hdr) & 0xFF))
            * 1000000.0);
    /* 11. Check .torig timestamp in NTP reply: */
    if (ntpreply.torig.s != ntpreq.txmit.s
            || ntpreply.torig.f != ntpreq.txmit.f) {
        fprintf(stderr, "Invalid reply: orig != request's xmit\n");
        goto e4;
    }
    /* 12. Calculate time offset: */
    t[0] = tv2ldbl(&tv0);
    t[1] = ntp2ldbl(&ntpreply.trecv);
    t[2] = ntp2ldbl(&ntpreply.txmit);
    t[3] = tv2ldbl(&tv3);
    t[4] = (t[1] - t[0] + t[2] - t[3]) / 2;
    fprintf(stdout, "clock offset: %+20.9Lfs\n", -t[4]);
    fprintf(stdout, "   orig time: %20.9Lfs\n",  t[0]);
    fprintf(stdout, "   recv time: %20.9Lfs\n",  t[1]);
    fprintf(stdout, "   xmit time: %20.9Lfs\n",  t[2]);
    fprintf(stdout, "     t4 time: %20.9Lfs\n",  t[3]);
    fprintf(stdout, "   rtt delay: %19.4Lfms\n",
            (t[3] - t[0] - t[2] + t[1]) * 1000);
    /* 13. Enable CAP_SYS_TIME just before calling adjtime() or
     *     settimeofday(). CAP_SYS_TIME must have been inherited or
     *     permitted beforehand by running ntpc as root or adding
     *     it to ntpc binary with '# setcap cap_sys_time=p ./ntpc'. */
    if (caps != NULL && cap_sys_time0 == CAP_SET) {
        cap_value_t capv[1] = {CAP_SYS_TIME};
        ret = cap_set_flag(caps, CAP_EFFECTIVE, 1, capv, CAP_SET);
        if (ret != 0) perror("cap_set_flag(CAP_SYS_TIME,e)");
        else {
            ret = cap_set_proc(caps);
            if (ret != 0) perror("cap_set_proc()");
        }
    }
    /* 14. Adjust system time: */
    if (t[4] < -0.25 || t[4] > 0.25) {
        /* Linux "man adjtime(3)", 2016-03-15:
         * adjtime() is intended to be used to make small adjustments
         * to the system time. Most systems impose a limit on the
         * adjustment that can be specified in delta. In the glibc
         * implementation, delta must be less than or equal to
         * (INT_MAX / 1000000 - 2) and greater than or equal to
         * (INT_MIN / 1000000 + 2) (respectively 2145 and -2145 seconds
         * on i386). */
        ldbl2tv(t[4] + t[3], &tvnew);
        ret = settimeofday(&tvnew, NULL);
        if (ret != 0) {
            perror("settimeofday()");
            goto e4;
        }
    } else {
        ldbl2tv(t[4], &tvnew);
        ret = adjtime(&tvnew, NULL);
        if (ret != 0) {
            perror("adjtime()");
            goto e4;
        }
    }
    free(psrc_addr);
    close(fd);
    free(pdst_addr);
    if (caps != NULL) cap_free(caps);
    return 0;
e4: free(psrc_addr);
e3: close(fd);
e2: free(pdst_addr);
e1: if (caps != NULL) cap_free(caps);
    return 1;
}

/* vi:set sw=4 et ts=8 tw=71: */
