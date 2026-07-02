#include "hdlc-bitstream.h"
#include <stdlib.h>

typedef struct hdlc_tx_state {
    uint64_t bitbuf;
    int      bitcnt;
    int      ones;
} hdlc_tx_state;

typedef struct hdlc_rx_state {
    uint64_t bitbuf;
    int      bitcnt;
    int      ones;
    int      in_frame;

    uint8_t  frame_buf[4096];
    int      frame_len;

    uint8_t  cur_byte;
    int      cur_bitpos;
    uint8_t  last8;
} hdlc_rx_state;

/* ================= HDLC RX (bitstream -> PPP bytes) ================= */

hdlc_rx_state* hdlc_rx_new()
{
    hdlc_rx_state* s = calloc(1,sizeof(struct hdlc_rx_state));
    return s;
}

void hdlc_rx_free(hdlc_rx_state* s)
{
    free(s);
}

void hdlc_rx_init(hdlc_rx_state *s)
{
    memset(s, 0, sizeof(*s));
}

void hdlc_rx_push_byte(hdlc_rx_state *s, uint8_t b,
                              hdlc_frame_cb cb, void *user)
{
    s->bitbuf = (s->bitbuf << 8) | b;
    s->bitcnt += 8;
    while (s->bitcnt > 0) {
        int bit = (s->bitbuf >> (s->bitcnt - 1)) & 1;
        s->bitcnt--;

        s->last8 = ((s->last8 << 1) | bit) & 0xFF;

        if (s->last8 == 0x7E) {
            s->ones = 0;
            s->cur_byte = 0;
            s->cur_bitpos = 0;

            if (cb && s->in_frame && s->frame_len > 0)
                cb(s->frame_buf, s->frame_len, user);

            s->in_frame = 1;
            s->frame_len = 0;

            continue;
        }

        if (!s->in_frame)
            continue;

        if (bit) {
            s->ones++;
        } else {
            if (s->ones == 5) {
                s->ones = 0;
                continue;
            }
            s->ones = 0;
        }

        s->cur_byte = (s->cur_byte >> 1) | (bit?0x80:0);
        s->cur_bitpos++;

        if (s->cur_bitpos == 8) {
            if (s->frame_len < (int)sizeof(s->frame_buf))
                s->frame_buf[s->frame_len++] = s->cur_byte;
            s->cur_bitpos = 0;
            s->cur_byte = 0;
        }
    }
}

/* ================= HDLC TX (PPP bytes -> bitstream) ================= */

hdlc_tx_state* hdlc_tx_new()
{
    hdlc_tx_state* s = calloc(1,sizeof(struct hdlc_tx_state));
    return s;
}

void hdlc_tx_free(hdlc_tx_state* s)
{
    free(s);
}


void hdlc_tx_init(hdlc_tx_state *s)
{
    memset(s, 0, sizeof(*s));
}

static void hdlc_tx_put_bit_raw(hdlc_tx_state *s, int bit,
                                uint8_t *out, int *out_len, int max)
{
    s->bitbuf = (s->bitbuf << 1) | (bit & 1);
    s->bitcnt++;

    if (s->bitcnt == 8) {
        if (*out_len < max)
            out[(*out_len)++] = (uint8_t)s->bitbuf;
        s->bitbuf = 0;
        s->bitcnt = 0;
    }
}

static void hdlc_tx_put_bit(hdlc_tx_state *s, int bit,
                            uint8_t *out, int *out_len, int max)
{

    hdlc_tx_put_bit_raw(s, bit, out, out_len, max);
    if (bit) {
        s->ones++;
        if (s->ones == 5) {
            hdlc_tx_put_bit_raw(s, 0, out, out_len, max);
            s->ones = 0;
        }
    } else {
        s->ones = 0;
    }
}

void hdlc_tx_put_byte(hdlc_tx_state *s, uint8_t b,
                             uint8_t *out, int *out_len, int max)
{
    for (int i = 7; i >= 0; --i)
        hdlc_tx_put_bit(s, ((b << i) & 0x80?1:0), out, out_len, max);
}

void hdlc_tx_put_flag(hdlc_tx_state *s,
                             uint8_t *out, int *out_len, int max)
{
    s->ones = 0;
    for (int i = 7; i >= 0; --i)
        hdlc_tx_put_bit_raw(s, (0x7E >> i) & 1, out, out_len, max);

}
