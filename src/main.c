#include "common.h"

#include "db.h"
#include "ip.h"
#include "option.h"
#include "str.h"
#include "tun.h"
#include "iface.h"

#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <sys/socket.h>
#include <sys/time.h>

#include <arpa/inet.h>
#include <netdb.h>

#include "../mud/mud.h"

#ifndef O_CLOEXEC
#define O_CLOEXEC 0
#endif

#ifndef PACKAGE_VERSION
#define PACKAGE_VERSION "unknown"
#endif

#define GT_MTU(X) ((X)-28)

static struct {
    volatile sig_atomic_t quit;
    volatile sig_atomic_t reload;
    char *dev;
    char *keyfile;
    char *host;
    long port;
    struct {
        char *list;
        char *backup;
        long port;
    } bind;
    long mtu;
    long timeout;
    long time_tolerance;
    int ipv4;
    int ipv6;
    int mtu_auto;
    int chacha20;
    int version;
    int keygen;
    int persist;
    struct {
        unsigned char *data;
        long size;
    } buf;
} gt = {
    .port = 5000,
    .bind = {
        .port = 5000,
    },
    .mtu = 1500,
    .timeout = 5000,
    .ipv4 = 1,
#ifdef __linux__
    .ipv6 = 1,
#endif
    .buf = {
        .size = 64 * 1024,
    },
};

static void
fd_set_nonblock(int fd)
{
    int ret;

    do {
        ret = fcntl(fd, F_GETFL, 0);
    } while (ret == -1 && errno == EINTR);

    int flags = (ret == -1) ? 0 : ret;

    do {
        ret = fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    } while (ret == -1 && errno == EINTR);

    if (ret == -1)
        perror("fcntl O_NONBLOCK");
}

static void
gt_quit_handler(int sig)
{
    gt.reload = (sig == SIGHUP);
    gt.quit = 1;
}

static void
gt_set_signal(void)
{
    struct sigaction sa = {
        .sa_flags = 0,
    };

    sigemptyset(&sa.sa_mask);

    sa.sa_handler = gt_quit_handler;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGQUIT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);

    sa.sa_handler = SIG_IGN;
    sigaction(SIGPIPE, &sa, NULL);
    sigaction(SIGUSR1, &sa, NULL);
    sigaction(SIGUSR2, &sa, NULL);
}

static void
gt_print_secretkey(struct mud *mud)
{
    unsigned char key[32];
    size_t size = sizeof(key);

    if (mud_get_key(mud, key, &size))
        return;

    char buf[2 * sizeof(key) + 1];

    gt_tohex(buf, sizeof(buf), key, size);
    gt_print("%s\n", buf);
}

static int
gt_setup_secretkey(struct mud *mud, char *keyfile)
{
    int fd;

    do {
        fd = open(keyfile, O_RDONLY | O_CLOEXEC);
    } while (fd == -1 && errno == EINTR);

    if (fd == -1) {
        perror("open keyfile");
        return -1;
    }

    unsigned char key[32];
    char buf[2 * sizeof(key)];
    size_t size = 0;

    while (size < sizeof(buf)) {
        ssize_t r = read(fd, &buf[size], sizeof(buf) - size);

        if (r <= (ssize_t)0) {
            if (r && (errno == EAGAIN || errno == EINTR))
                continue;
            break;
        }

        size += r;
    }

    close(fd);

    if (size != sizeof(buf)) {
        gt_log("unable to read secret key\n");
        return -1;
    }

    if (gt_fromhex(key, sizeof(key), buf, sizeof(buf))) {
        gt_log("secret key is not valid\n");
        return -1;
    }

    mud_set_key(mud, key, sizeof(key));

    return 0;
}

static int
gt_setup_option(int argc, char **argv)
{
    // clang-format off

    struct option opts[] = {
        { "host",           &gt.host,           option_str    },
        { "port",           &gt.port,           option_long   },
        { "bind",           &gt.bind.list,      option_str    },
        { "bind-backup",    &gt.bind.backup,    option_str    },
        { "bind-port",      &gt.bind.port,      option_long   },
        { "dev",            &gt.dev,            option_str    },
        { "persist",        NULL,               option_option },
        { "mtu",            &gt.mtu,            option_long   },
        { "mtu-auto",       NULL,               option_option },
        { "keyfile",        &gt.keyfile,        option_str    },
        { "keygen",         NULL,               option_option },
        { "timeout",        &gt.timeout,        option_long   },
        { "time-tolerance", &gt.time_tolerance, option_long   },
        { "v4only",         NULL,               option_option },
        { "v6only",         NULL,               option_option },
        { "chacha20",       NULL,               option_option },
        { "buf-size",       &gt.buf.size,       option_long   },
        { "version",        NULL,               option_option },
        {  NULL                                               },
    };

    // clang-format on

    if (option(opts, argc, argv))
        return 1;

    int v4only = option_is_set(opts, "v4only");
    int v6only = option_is_set(opts, "v6only");

    if (v4only && v6only) {
        gt_log("v4only and v6only cannot be both set\n");
        return 1;
    }

    if ((int)gt.timeout <= 0) {
        gt_log("bad timeout\n");
        return 1;
    }

    if (gt.buf.size <= 0) {
        gt_log("bad buf-size\n");
        return 1;
    }

    if (v4only) {
        gt.ipv4 = 1;
        gt.ipv6 = 0;
    }

    if (v6only) {
        gt.ipv4 = 0;
        gt.ipv6 = 1;
    }

    gt.mtu_auto = option_is_set(opts, "mtu-auto");
    gt.chacha20 = option_is_set(opts, "chacha20");
    gt.version = option_is_set(opts, "version");
    gt.keygen = option_is_set(opts, "keygen");
    gt.persist = option_is_set(opts, "persist");

    gt.buf.data = malloc(gt.buf.size);

    return 0;
}

static void
gt_setup_mtu(struct mud *mud, char *tun_name)
{
    int mtu = mud_get_mtu(mud);

    if (mtu == (int)gt.mtu)
        return;

    gt.mtu = mtu;

    gt_log("setup MTU to %i on interface %s\n", mtu, tun_name);

    if (iface_set_mtu(tun_name, mtu) == -1)
        perror("tun_set_mtu");
}

int
main(int argc, char **argv)
{
    gt_set_signal();

    if (gt_setup_option(argc, argv))
        return 1;

    if (gt.version) {
        gt_print(PACKAGE_VERSION "\n");
        return 0;
    }

    int icmp_fd = -1;

    if (gt.ipv4 && gt.mtu_auto) {
        icmp_fd = socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);

        if (icmp_fd == -1)
            gt_log("couldn't create ICMP socket\n");
    }

    struct mud *mud = mud_create(gt.bind.port, gt.ipv4, gt.ipv6);

    if (!mud) {
        gt_log("couldn't create mud\n");
        return 1;
    }

    if (gt.keygen || str_empty(gt.keyfile)) {
        if (mud_new_key(mud)) {
            gt_log("couldn't generate a new key\n");
            return 1;
        }
    }

    if (gt.keygen) {
        gt_print_secretkey(mud);
        return 0;
    }

    if (!gt.chacha20 && mud_set_aes(mud))
        gt_log("AES is not available\n");

    if (gt.timeout > 0)
        mud_set_send_timeout_msec(mud, gt.timeout);

    if (gt.time_tolerance > 0)
        mud_set_time_tolerance_sec(mud, gt.time_tolerance);

    mud_set_mtu(mud, GT_MTU(gt.mtu));

    char *tun_name = NULL;

    int tun_fd = tun_create(gt.dev, &tun_name);

    if (tun_fd == -1) {
        gt_log("couldn't create tun device\n");
        return 1;
    }

    if (tun_set_persist(tun_fd, gt.persist) == -1)
        perror("tun_set_persist");

    if (str_empty(gt.keyfile)) {
        gt_print("here is your new secret key:\n");
        gt_print_secretkey(mud);
    } else {
        if (gt_setup_secretkey(mud, gt.keyfile))
            return 1;
    }

    if (gt.host && gt.port) {
        if (gt.bind.backup) {
            if (mud_peer(mud, gt.bind.backup, gt.host, gt.port, 1)) {
                perror("mud_peer (backup)");
                return 1;
            }
        }

        if (gt.bind.list) {
            char tmp[1024];
            char *name = &tmp[0];

            str_cpy(tmp, gt.bind.list, sizeof(tmp) - 1);

            while (*name) {
                char *p = name;

                while (*p && *p != ',')
                    p++;

                if (*p)
                    *p++ = 0;

                if (mud_peer(mud, name, gt.host, gt.port, 0)) {
                    perror("mud_peer");
                    return 1;
                }

                name = p;
            }
        }
    }

    gt_setup_mtu(mud, tun_name);

    int mud_fd = mud_get_fd(mud);

    fd_set_nonblock(tun_fd);
    fd_set_nonblock(mud_fd);

    if (icmp_fd != -1)
        fd_set_nonblock(icmp_fd);

    gt_log("running...\n");

    fd_set rfds;
    FD_ZERO(&rfds);

    int last_fd = 1 + MAX(tun_fd, MAX(mud_fd, icmp_fd));

    while (!gt.quit) {
        FD_SET(tun_fd, &rfds);
        FD_SET(mud_fd, &rfds);

        if (icmp_fd != -1)
            FD_SET(icmp_fd, &rfds);

        if (select(last_fd, &rfds, NULL, NULL, NULL) == -1) {
            if (errno != EBADF)
                continue;
            perror("select");
            return 1;
        }

        if (icmp_fd != -1 && FD_ISSET(icmp_fd, &rfds)) {
            struct sockaddr_storage ss;
            socklen_t sl = sizeof(ss);
            ssize_t r = recvfrom(icmp_fd, gt.buf.data, gt.buf.size, 0,
                                 (struct sockaddr *)&ss, &sl);
            if (r >= 8) {
                struct ip_common ic;
                if (!ip_get_common(&ic, gt.buf.data, r) && ic.proto == 1) {
                    unsigned char *data = &gt.buf.data[ic.hdr_size];
                    if (data[0] == 3) {
                        int mtu = (data[6] << 8) | data[7];
                        if (mtu) {
                            gt_log("received MTU from ICMP: %i\n", mtu);
                            mud_set_mtu(mud, GT_MTU(mtu));
                        }
                    }
                }
            }
        }

        if (FD_ISSET(tun_fd, &rfds)) {
            size_t size = 0;

            while (gt.buf.size - size >= gt.mtu) {
                const int r = tun_read(tun_fd, &gt.buf.data[size], gt.buf.size - size);

                if (r <= 0 || r > gt.mtu)
                    break;

                struct ip_common ic;

                if (ip_get_common(&ic, &gt.buf.data[size], r) || ic.size != r)
                    break;

                size += r;
            }

            int p = 0;

            while (p < size) {
                int tc = 0;
                int q = p;

                while (q < size) {
                    struct ip_common ic;

                    if ((ip_get_common(&ic, &gt.buf.data[q], size - q)) ||
                        (ic.size > size - q))
                        break;

                    if (q + ic.size > p + gt.mtu)
                        break;

                    q += ic.size;

                    if (tc < (ic.tc & 0xFC))
                        tc = ic.tc & 0xFC;
                }

                if (p >= q)
                    break;

                int r = mud_send(mud, &gt.buf.data[p], q - p, tc);

                if (r == -1 && errno == EMSGSIZE) {
                    gt_setup_mtu(mud, tun_name);
                } else {
                    if (r == -1 && errno != EAGAIN)
                        perror("mud_send");
                }

                p = q;
            }
        }

        if (FD_ISSET(mud_fd, &rfds)) {
            size_t size = 0;

            while (gt.buf.size - size >= gt.mtu) {
                const int r = mud_recv(mud, &gt.buf.data[size], gt.buf.size - size);

                if (r <= 0) {
                    if (r == -1 && errno != EAGAIN)
                        perror("mud_recv");
                    break;
                }

                size += r;
            }

            int p = 0;

            while (p < size) {
                struct ip_common ic;

                if ((ip_get_common(&ic, &gt.buf.data[p], size - p)) ||
                    (ic.size > size - p))
                    break;

                tun_write(tun_fd, &gt.buf.data[p], ic.size);

                p += ic.size;
            }
        }
    }

    if (gt.reload && tun_fd >= 0) {
        if (tun_set_persist(tun_fd, 1) == -1)
            perror("tun_set_persist");
    }

    return 0;
}
