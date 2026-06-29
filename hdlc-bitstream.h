#ifndef HDLC_BITSREAM_H
#define HDLC_BITSREAM_H

#include <stdint.h>
#include <string.h>

typedef struct hdlc_tx_state hdlc_tx_state;

typedef struct hdlc_rx_state hdlc_rx_state;

typedef void (*hdlc_frame_cb)(const uint8_t*, int, void*);

hdlc_rx_state * hdlc_rx_new();
void hdlc_rx_init(hdlc_rx_state *s);
void hdlc_rx_free(hdlc_rx_state *s);
void hdlc_rx_push_byte(hdlc_rx_state *s, uint8_t b,
                              hdlc_frame_cb cb, void *user);

hdlc_tx_state* hdlc_tx_new();
void hdlc_tx_init(hdlc_tx_state *s);
void hdlc_tx_free(hdlc_tx_state *s);
void hdlc_tx_put_byte(hdlc_tx_state *s, uint8_t b,
                             uint8_t *out, int *out_len, int max);

void hdlc_tx_put_flag(hdlc_tx_state *s,
                             uint8_t *out, int *out_len, int max);
#endif