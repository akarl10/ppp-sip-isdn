#ifndef CLEARMODE_CODEC_H
#define CLEARMODE_CODEC_H

#include <pjmedia/endpoint.h>
#include <pjmedia/codec.h>
#include <pjlib.h>

PJ_BEGIN_DECL

pj_status_t pjmedia_codec_clearmode_init(pjmedia_endpt *endpt);
pj_status_t pjmedia_codec_clearmode_deinit(pjmedia_endpt *endpt);

PJ_END_DECL

#endif /* CLEARMODE_CODEC_H */

