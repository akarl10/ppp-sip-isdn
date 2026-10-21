/* fake_pppd: stand-in for pppd / ppp-helper in the end-to-end test.
 *
 * Invoked by ppp-sip-isdn (secure mode, SECURE_PPP_HELPER pointing here) as
 *     fake_pppd <pts#|/dev/pts/N> <local-ip|''> <remote-ip|''>
 * or     fake_pppd stop <pid>
 * Role: "server" if a local ip is given, else "client".
 *
 * It speaks async-HDLC (RFC 1662) on the PTY: sends FRAMES PPP-looking frames
 * with a valid FCS-16 and deterministic content, and verifies the frames it
 * receives from the peer (FCS, header, sequence, payload).
 * Result goes to stderr as one line (plus "FAKEUP role=<r>" at startup):
 *     FAKEPPPD role=<r> sent=<n> recv=<n> bad=<n>
 * The client exits when done (so ppp-sip-isdn hangs up); the server waits for
 * SIGHUP, which is what ppp-sip-isdn sends when the call ends.
 */
#define _GNU_SOURCE
#include <ctype.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define DEFAULT_FRAMES 250  /* overridable: FAKE_PPPD_FRAMES in ppp-sip-isdn's environment, max 65535 */
#define DEFAULT_INTERVAL_MS 20   /* one codec frame; overridable: FAKE_PPPD_INTERVAL_MS */
#define START_DELAY_MS 300
#define IDLE_DONE_MS 1500   /* after sending: peer silent this long => assume the rest was lost */
#define LINGER_MS 500       /* client: let tx drain before exiting (the call ends with it) */
#define TIMEOUT_S 40

static int FRAMES = DEFAULT_FRAMES;
static int INTERVAL_MS = DEFAULT_INTERVAL_MS;
static volatile sig_atomic_t got_hup, got_alrm;

/* ppp-sip-isdn starts us with an empty environment, but its own environment
 * is readable via /proc, which lets the test script pass settings */
static void read_parent_config(void)
{
    char path[64], buf[8192];
    snprintf(path, sizeof path, "/proc/%d/environ", (int)getppid());
    int fd = open(path, O_RDONLY);
    if (fd < 0) return;
    ssize_t n = read(fd, buf, sizeof buf - 1);
    close(fd);
    for (ssize_t i = 0; i < n; ) {
        const char *e = buf + i;
        if (!strncmp(e, "FAKE_PPPD_FRAMES=", 17)) {
            int v = atoi(e + 17);
            if (v > 0 && v <= 65535) FRAMES = v;
        }
        if (!strncmp(e, "FAKE_PPPD_INTERVAL_MS=", 22)) {
            int v = atoi(e + 22);
            if (v > 0) INTERVAL_MS = v;
        }
        i += strlen(e) + 1;
    }
}
static void on_hup(int s) { (void)s; got_hup = 1; }
static void on_alrm(int s) { (void)s; got_alrm = 1; }

static uint16_t fcs16(const uint8_t *p, int n)
{
    uint16_t fcs = 0xffff;
    while (n--) {
        fcs ^= *p++;
        for (int i = 0; i < 8; i++)
            fcs = (fcs & 1) ? (fcs >> 1) ^ 0x8408 : fcs >> 1;
    }
    return fcs;
}

/* frame body (without FCS): FF 03 00 21 role idx payload... */
static int make_body(uint8_t *b, int role, int idx)
{
    idx &= 0xff;
    int plen = 20 + (idx * 37) % 200;
    int n = 0;
    b[n++] = 0xff; b[n++] = 0x03; b[n++] = 0x00; b[n++] = 0x21;
    b[n++] = role; b[n++] = idx;
    for (int i = 0; i < plen; i++) {
        uint8_t v = (uint8_t)(role * 31 + idx * 7 + i * 13);
        if (i % 17 == 0) v = 0x7e;
        else if (i % 23 == 0) v = 0x7d;
        else if (i % 31 == 0) v = 0xff;
        else if (i % 19 == 0) v = 0x03;
        b[n++] = v;
    }
    return n;
}

static int send_frame(int fd, int role, int idx)
{
    uint8_t body[512], out[1200];
    int n = make_body(body, role, idx);
    uint16_t fcs = ~fcs16(body, n);
    body[n++] = fcs & 0xff;
    body[n++] = fcs >> 8;
    int o = 0;
    out[o++] = 0x7e;
    for (int i = 0; i < n; i++) {
        if (body[i] < 0x20 || body[i] == 0x7d || body[i] == 0x7e) {
            out[o++] = 0x7d;
            out[o++] = body[i] ^ 0x20;
        } else out[o++] = body[i];
    }
    out[o++] = 0x7e;
    return write(fd, out, o) == o ? 0 : -1;
}

/* Frame problems are timestamped (CLOCK_MONOTONIC, ms) and printed with the result of
 * lines that ran to completion, so the test script can relate them to events
 * such as other lines being torn down. */
static long long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}
static long long evt[64];
static int nevt;
static void note_evt(void)
{
    if (nevt < 64) evt[nevt++] = now_ms();
}
static void print_evts(void)
{
    for (int i = 0; i < nevt; i++) fprintf(stderr, "EVT t=%lld\n", evt[i]);
}

static int next_idx;   /* next expected incoming frame */
static int recv_ok, recv_bad;

static void check_frame(const uint8_t *f, int n, int peer_role)
{
    uint8_t exp[512];
    if (n < 4 + 2 + 2) { note_evt(); recv_bad++; fprintf(stderr, "short frame %d\n", n); return; }
    if (fcs16(f, n) != 0xf0b8) { note_evt(); recv_bad++; fprintf(stderr, "bad FCS (len %d)\n", n); return; }
    int idx = f[5];
    int en = make_body(exp, peer_role, idx);
    if (idx != next_idx || n - 2 != en || memcmp(f, exp, en)) {
        note_evt();
        recv_bad++;
        fprintf(stderr, "unexpected frame idx=%d (expected %d) len=%d\n", idx, next_idx, n - 2);
        next_idx = (idx + 1) & 0xff;
        return;
    }
    next_idx = (next_idx + 1) & 0xff;
    recv_ok++;
}

static uint8_t rbuf[8192];
static int rlen;

static void feed(const uint8_t *d, int n, int peer_role)
{
    for (int i = 0; i < n; i++) {
        if (d[i] == 0x7e) {
            if (rlen > 0) {
                uint8_t un[8192]; int u = 0;
                for (int k = 0; k < rlen; k++) {
                    if (rbuf[k] == 0x7d && k + 1 < rlen) un[u++] = rbuf[++k] ^ 0x20;
                    else un[u++] = rbuf[k];
                }
                check_frame(un, u, peer_role);
            }
            rlen = 0;
        } else if (rlen < (int)sizeof rbuf) rbuf[rlen++] = d[i];
    }
}

int main(int argc, char **argv)
{
    if (argc == 3 && !strcmp(argv[1], "stop")) {   /* ppp-helper "stop <pid>" */
        kill(atoi(argv[2]), SIGHUP);
        return 0;
    }
    if (argc < 4) { fprintf(stderr, "usage: fake_pppd <pts> <local> <remote>\n"); return 1; }

    char path[64];
    if (!strncmp(argv[1], "/dev/", 5)) snprintf(path, sizeof path, "%s", argv[1]);
    else snprintf(path, sizeof path, "/dev/pts/%s", argv[1]);
    int role = argv[2][0] ? 1 : 2;                 /* 1 = server, 2 = client */
    const char *rname = role == 1 ? "server" : "client";
    int peer = role == 1 ? 2 : 1;

    read_parent_config();
    signal(SIGHUP, on_hup);
    signal(SIGALRM, on_alrm);
    alarm(TIMEOUT_S);

    int fd = open(path, O_RDWR | O_NOCTTY);
    if (fd < 0) { perror("fake_pppd: open pty"); return 1; }
    struct termios t;
    tcgetattr(fd, &t); cfmakeraw(&t); tcsetattr(fd, TCSANOW, &t);

    fprintf(stderr, "FAKEUP role=%s\n", rname);   /* lets the test script see the line is up */
    /* real elapsed time: poll() returns early when data arrives, so counting
     * loop iterations would run the send schedule and timeouts too fast */
    long long t0 = now_ms(), next_send = START_DELAY_MS;
    long long ms = 0, done_ms = -1, last_rx_ms = 0;
    int sent = 0, reported = 0;
    while (!got_hup && !got_alrm) {
        struct pollfd p = { fd, POLLIN, 0 };
        int r = poll(&p, 1, 5);
        ms = now_ms() - t0;
        if (r > 0 && (p.revents & POLLIN)) {
            uint8_t buf[2048];
            int n = read(fd, buf, sizeof buf);
            if (n > 0) { feed(buf, n, peer); last_rx_ms = ms; }
        }
        if (sent < FRAMES && ms >= next_send) {
            if (send_frame(fd, role, sent) == 0) sent++;
            next_send += INTERVAL_MS;
        }
        if (sent == FRAMES && done_ms < 0 &&
            (recv_ok + recv_bad >= FRAMES || ms - last_rx_ms >= IDLE_DONE_MS)) {
            done_ms = ms;
            print_evts();
            fprintf(stderr, "FAKEPPPD role=%s sent=%d recv=%d bad=%d\n",
                    rname, sent, recv_ok, recv_bad);
            reported = 1;
        }
        if (role == 2 && done_ms >= 0 && ms - done_ms >= LINGER_MS)
            return recv_bad ? 1 : 0;
    }
    if (!reported && sent == FRAMES) print_evts();
    if (!reported)
        fprintf(stderr, "FAKEPPPD role=%s sent=%d recv=%d bad=%d%s\n", rname,
                sent, recv_ok, recv_bad, got_alrm ? " TIMEOUT" : "");
    return 0;
}
