/* Unit tests for hdlc-bitstream (no PJSIP needed): make test */
#include "../hdlc-bitstream.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAXF 4096
#define MAXLEN 5000

static struct { uint8_t d[MAXLEN]; int n; } got[MAXF];
static int gotcnt;
static int failures, checks;

#define CHECK(cond, ...) do { checks++; if(!(cond)) { failures++; \
    printf("  FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while(0)

static void cb(const uint8_t *d, int n, void *u)
{
    (void)u;
    if (gotcnt < MAXF && n <= MAXLEN) {
        memcpy(got[gotcnt].d, d, n);
        got[gotcnt].n = n;
    }
    gotcnt++;
}

/* ---- helpers ---- */
typedef struct { hdlc_tx_state *tx; hdlc_rx_state *rx; uint8_t *out; int len; } ctx;

static void ctx_init(ctx *c)
{
    c->tx = hdlc_tx_new(); c->rx = hdlc_rx_new();
    hdlc_tx_init(c->tx); hdlc_rx_init(c->rx);
    c->out = malloc(1 << 20); c->len = 0; gotcnt = 0;
}
static void ctx_free(ctx *c) { hdlc_tx_free(c->tx); hdlc_rx_free(c->rx); free(c->out); }
static void tx_flag(ctx *c) { hdlc_tx_put_flag(c->tx, c->out, &c->len, 1 << 20); }
static void tx_frame(ctx *c, const uint8_t *d, int n)
{
    for (int i = 0; i < n; i++) hdlc_tx_put_byte(c->tx, d[i], c->out, &c->len, 1 << 20);
    tx_flag(c);
}
static void rx_all(ctx *c)
{
    for (int i = 0; i < c->len; i++) hdlc_rx_push_byte(c->rx, c->out[i], cb, NULL);
}
static void rx_bytes(const uint8_t *b, int n)
{
    hdlc_rx_state *rx = hdlc_rx_new(); hdlc_rx_init(rx); gotcnt = 0;
    for (int i = 0; i < n; i++) hdlc_rx_push_byte(rx, b[i], cb, NULL);
    hdlc_rx_free(rx);
}

/* ---- tests ---- */

static void test_roundtrip(void)
{
    printf("roundtrip, 1..200 byte frames incl. bit-stuffing patterns\n");
    ctx c; ctx_init(&c);
    static uint8_t fr[2000][256];
    tx_flag(&c);
    for (int f = 0; f < 2000; f++) {
        int n = 1 + f % 200;
        for (int i = 0; i < n; i++) fr[f][i] = (i % 3 == 0) ? 0xff : (f * 7 + i * 13) & 0xff;
        if (f % 5 == 0) fr[f][n - 1] = 0xff;      /* frame ends in ones before flag */
        if (f % 7 == 0) fr[f][0] = 0x7e;           /* payload looks like a flag */
        tx_frame(&c, fr[f], n);
        if (f % 4 == 0) tx_flag(&c);                /* extra idle flag */
    }
    for (int i = 0; i < 8; i++) tx_flag(&c);
    rx_all(&c);
    CHECK(gotcnt == 2000, "got %d frames, expected 2000", gotcnt);
    for (int f = 0; f < 2000 && f < gotcnt; f++)
        CHECK(got[f].n == 1 + f % 200 && !memcmp(got[f].d, fr[f], got[f].n), "frame %d differs", f);
    ctx_free(&c);
}

static void test_all_ones_and_zeros_payload(void)
{
    printf("payloads of 0xFF and 0x00 (worst-case stuffing)\n");
    ctx c; ctx_init(&c);
    uint8_t ones[64], zeros[64];
    memset(ones, 0xff, sizeof ones); memset(zeros, 0, sizeof zeros);
    tx_flag(&c); tx_frame(&c, ones, 64); tx_frame(&c, zeros, 64); tx_flag(&c); tx_flag(&c);
    rx_all(&c);
    CHECK(gotcnt == 2, "got %d frames", gotcnt);
    if (gotcnt == 2) {
        CHECK(got[0].n == 64 && !memcmp(got[0].d, ones, 64), "ones frame");
        CHECK(got[1].n == 64 && !memcmp(got[1].d, zeros, 64), "zeros frame");
    }
    ctx_free(&c);
}

static void test_shared_flag(void)
{
    printf("back-to-back frames sharing one flag\n");
    ctx c; ctx_init(&c);
    uint8_t a[] = {1, 2, 3}, b[] = {4, 5, 6, 7};
    tx_flag(&c); tx_frame(&c, a, 3); tx_frame(&c, b, 4); tx_flag(&c);
    rx_all(&c);
    CHECK(gotcnt == 2, "got %d frames", gotcnt);
    ctx_free(&c);
}

static void test_empty_frames(void)
{
    printf("consecutive flags deliver no frames\n");
    uint8_t b[] = {0x7e, 0x7e, 0x7e, 0x7e};
    rx_bytes(b, sizeof b);
    CHECK(gotcnt == 0, "got %d frames", gotcnt);
}

static void test_no_flag_no_frame(void)
{
    printf("junk without any flag delivers nothing\n");
    uint8_t b[100]; memset(b, 0xa5, sizeof b);
    rx_bytes(b, sizeof b);
    CHECK(gotcnt == 0, "got %d frames", gotcnt);
}

static void test_idle_ones_then_junk(void)
{
    printf("idle ones + junk between flags is dropped (abort)\n");
    uint8_t b[] = {0x7e, 0xff, 0xff, 0xff, 0xa5, 0x3c, 0x7e};
    rx_bytes(b, sizeof b);
    CHECK(gotcnt == 0, "got %d bogus frames", gotcnt);
}

static void test_misaligned(void)
{
    printf("frame that is not a whole number of octets is dropped\n");
    uint8_t b[] = {0x7e, 0x12, 0xbf, 0x7e, 0x00};
    rx_bytes(b, sizeof b);
    CHECK(gotcnt == 0, "got %d bogus frames", gotcnt);
}

static void test_valid_aligned(void)
{
    printf("hand-made valid 2-byte frame is delivered\n");
    uint8_t b[] = {0x7e, 0x12, 0x34, 0x7e};
    rx_bytes(b, sizeof b);
    CHECK(gotcnt == 1, "got %d frames", gotcnt);
}

static void test_recovery_after_abort(void)
{
    printf("valid frames after an abort/garbage are still received\n");
    ctx c; ctx_init(&c);
    uint8_t a[] = {0xde, 0xad, 0xbe, 0xef}, b[] = {9, 8, 7};
    tx_flag(&c); tx_frame(&c, a, 4);
    /* inject abort + junk straight into the receiver, then more frames */
    rx_all(&c);
    uint8_t junk[] = {0x00, 0xff, 0xff, 0xaa};
    for (int i = 0; i < 4; i++) hdlc_rx_push_byte(c.rx, junk[i], cb, NULL);
    c.len = 0;
    tx_flag(&c); tx_frame(&c, b, 3); tx_flag(&c); tx_flag(&c);
    rx_all(&c);
    int ok = 0;
    for (int i = 0; i < gotcnt && i < MAXF; i++)
        if (got[i].n == 3 && !memcmp(got[i].d, b, 3)) ok++;
    CHECK(ok == 1, "frame after abort delivered %d times", ok);
    ctx_free(&c);
}

static void test_oversize(void)
{
    printf("frame larger than the rx buffer is dropped, not truncated\n");
    ctx c; ctx_init(&c);
    static uint8_t big[5000], small[10];
    for (int i = 0; i < 5000; i++) big[i] = i & 0x7f;
    memset(small, 0x11, sizeof small);
    tx_flag(&c); tx_frame(&c, big, 5000); tx_frame(&c, small, 10); tx_flag(&c); tx_flag(&c);
    rx_all(&c);
    CHECK(gotcnt == 1, "got %d frames, expected only the small one", gotcnt);
    if (gotcnt == 1) CHECK(got[0].n == 10, "wrong frame length %d", got[0].n);
    ctx_free(&c);
}

int main(void)
{
    test_roundtrip();
    test_all_ones_and_zeros_payload();
    test_shared_flag();
    test_empty_frames();
    test_no_flag_no_frame();
    test_idle_ones_then_junk();
    test_misaligned();
    test_valid_aligned();
    test_recovery_after_abort();
    test_oversize();
    printf("\n%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
