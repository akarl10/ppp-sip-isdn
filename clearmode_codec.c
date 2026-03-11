#include "clearmode_codec.h"

#include <pjmedia/codec.h>
#include <pjmedia/endpoint.h>
#include <pjmedia/errno.h>
#include <pj/pool.h>
#include <pj/string.h>
#include <pj/os.h>

#define THIS_CODEC_NAME   "CLEARMODE"
#define THIS_CLOCK_RATE   8000
#define THIS_CHANNELS     1

typedef struct clearmode_factory
{
    pjmedia_codec_factory base;
    pjmedia_endpt        *endpt;
    pj_pool_t            *pool;
} clearmode_factory;

typedef struct clearmode_codec
{
    pjmedia_codec        base;
    pj_pool_t           *pool;
    pjmedia_codec_param  param;
} clearmode_codec;

/* ---- factory ops ---- */

static pj_status_t clearmode_factory_test_alloc(pjmedia_codec_factory *f,
                                      const pjmedia_codec_info *info);
static pj_status_t clearmode_factory_default_attr(pjmedia_codec_factory *f,
                                        const pjmedia_codec_info *info,
                                        pjmedia_codec_param *attr);
static pj_status_t clearmode_factory_enum_info(pjmedia_codec_factory *f,
                                     unsigned *count,
                                     pjmedia_codec_info info[]);
static pj_status_t clearmode_factory_alloc_codec(pjmedia_codec_factory *f,
                                       const pjmedia_codec_info *info,
                                       pjmedia_codec **p_codec);
static pj_status_t clearmode_factory_dealloc_codec(pjmedia_codec_factory *f,
                                         pjmedia_codec *codec);
static pj_status_t clearmode_factory_destroy();

/* ---- codec ops ---- */

static pj_status_t clearmode_codec_init(pjmedia_codec *codec,
                              pj_pool_t *pool);
static pj_status_t clearmode_codec_open(pjmedia_codec *codec,
                              pjmedia_codec_param *attr);
static pj_status_t clearmode_codec_close(pjmedia_codec *codec);
static pj_status_t clearmode_codec_modify(pjmedia_codec *codec,
                                const pjmedia_codec_param *attr);
static pj_status_t clearmode_codec_parse(pjmedia_codec *codec,
                               void *pkt,
                               pj_size_t pkt_size,
                               const pj_timestamp *ts,
                               unsigned *frame_cnt,
                               pjmedia_frame frames[]);
static pj_status_t clearmode_codec_encode(pjmedia_codec *codec,
                                const pjmedia_frame *input,
                                unsigned output_buf_len,
                                pjmedia_frame *output);
static pj_status_t clearmode_codec_decode(pjmedia_codec *codec,
                                const pjmedia_frame *input,
                                unsigned output_buf_len,
                                pjmedia_frame *output);
static pj_status_t clearmode_codec_recover(pjmedia_codec *codec,
                                 unsigned output_buf_len,
                                 pjmedia_frame *output);

/* ---- ops tables ---- */

static pjmedia_codec_factory_op factory_op =
{
    &clearmode_factory_test_alloc,
    &clearmode_factory_default_attr,
    &clearmode_factory_enum_info,
    &clearmode_factory_alloc_codec,
    &clearmode_factory_dealloc_codec,
    &clearmode_factory_destroy
};

static pjmedia_codec_op codec_op =
{
    &clearmode_codec_init,
    &clearmode_codec_open,
    &clearmode_codec_close,
    &clearmode_codec_modify,
    &clearmode_codec_parse,
    &clearmode_codec_encode,
    &clearmode_codec_decode,
    &clearmode_codec_recover
};

static clearmode_factory *g_factory = NULL;

/* ===== factory implementation ===== */

static pj_status_t clearmode_factory_test_alloc(pjmedia_codec_factory *f,
                                      const pjmedia_codec_info *info)
{
    PJ_UNUSED_ARG(f);

    if (info->type != PJMEDIA_TYPE_AUDIO)
        return PJMEDIA_CODEC_EUNSUP;

    if (info->clock_rate != THIS_CLOCK_RATE)
        return PJMEDIA_CODEC_EUNSUP;

    if (info->channel_cnt != THIS_CHANNELS)
        return PJMEDIA_CODEC_EUNSUP;

    if (pj_stricmp2(&info->encoding_name, THIS_CODEC_NAME) != 0)
        return PJMEDIA_CODEC_EUNSUP;

    return PJ_SUCCESS;
}

static pj_status_t clearmode_factory_default_attr(pjmedia_codec_factory *f,
                                        const pjmedia_codec_info *info,
                                        pjmedia_codec_param *attr)
{
    PJ_UNUSED_ARG(f);

    pj_bzero(attr, sizeof(*attr));

    attr->info.clock_rate  = THIS_CLOCK_RATE;//info->clock_rate;
    attr->info.channel_cnt = info->channel_cnt;
    attr->info.avg_bps     = 64000;
    attr->info.max_bps     = 64000;
    attr->info.frm_ptime   = 20;  /* 20 ms */
    attr->info.pt          = info->pt; /* dynamic, assigned by core */

    attr->setting.frm_per_pkt = 1;
    attr->setting.vad         = 0;
    attr->setting.plc         = 0;
    attr->setting.cng         = 0;

    return PJ_SUCCESS;
}

static pj_status_t clearmode_factory_enum_info(pjmedia_codec_factory *f,
                                     unsigned *count,
                                     pjmedia_codec_info info[])
{
    PJ_UNUSED_ARG(f);

    if (*count == 0)
        return PJ_SUCCESS;

    pj_bzero(&info[0], sizeof(pjmedia_codec_info));
    info[0].type = PJMEDIA_TYPE_AUDIO;
    info[0].pt   = PJMEDIA_RTP_PT_DYNAMIC; /* => let core assign dynamic PT */
    pj_cstr(&info[0].encoding_name, THIS_CODEC_NAME);
    info[0].clock_rate  = THIS_CLOCK_RATE;
    info[0].channel_cnt = THIS_CHANNELS;

    *count = 1;
    return PJ_SUCCESS;
}

static pj_status_t clearmode_factory_alloc_codec(pjmedia_codec_factory *f,
                                       const pjmedia_codec_info *info,
                                       pjmedia_codec **p_codec)
{
    clearmode_factory *cf = (clearmode_factory*)f;
    pj_pool_t *pool;
    clearmode_codec *cc;

    PJ_UNUSED_ARG(info);

    pool = pj_pool_create(cf->pool->factory, "clearmode_codec", 512, 512, NULL);
    if (!pool)
        return PJ_ENOMEM;

    cc = PJ_POOL_ZALLOC_T(pool, clearmode_codec);
    cc->pool = pool;
    cc->base.op = &codec_op;
    cc->base.codec_data = cc; 
    cc->base.factory = f;

    *p_codec = &cc->base;
//    printf("allocc pjmedia_codec %x for factory %x\n",*p_codec, f);
    return PJ_SUCCESS;
}

static pj_status_t clearmode_factory_dealloc_codec(pjmedia_codec_factory *f,
                                         pjmedia_codec *codec)
{
    clearmode_factory *cf = (clearmode_factory*)f;
    clearmode_codec *cc = (clearmode_codec*)codec->codec_data;

    PJ_UNUSED_ARG(cf);

    if (cc->pool) {
        pj_pool_release(cc->pool);
        cc->pool = NULL;
    }

    return PJ_SUCCESS;
}

static pj_status_t clearmode_factory_destroy()
{
    g_factory = NULL;
    return PJ_SUCCESS;
}

/* ===== codec implementation ===== */

static pj_status_t clearmode_codec_init(pjmedia_codec *codec,
                              pj_pool_t *pool)
{
    PJ_UNUSED_ARG(codec);
    PJ_UNUSED_ARG(pool);
    return PJ_SUCCESS;
}

static pj_status_t clearmode_codec_open(pjmedia_codec *codec,
                              pjmedia_codec_param *attr)
{
    clearmode_codec *cc = (clearmode_codec*)codec;
    cc->param = *attr;
    return PJ_SUCCESS;
}

static pj_status_t clearmode_codec_close(pjmedia_codec *codec)
{
    PJ_UNUSED_ARG(codec);
    return PJ_SUCCESS;
}

static pj_status_t clearmode_codec_modify(pjmedia_codec *codec,
                                const pjmedia_codec_param *attr)
{
    clearmode_codec *cc = (clearmode_codec*)codec;
    cc->param = *attr;
    return PJ_SUCCESS;
}

static pj_status_t clearmode_codec_parse(pjmedia_codec *codec,
                               void *pkt,
                               pj_size_t pkt_size,
                               const pj_timestamp *ts,
                               unsigned *frame_cnt,
                               pjmedia_frame frames[])
{
    PJ_UNUSED_ARG(codec);

    if (pkt_size == 0) {
        *frame_cnt = 0;
        return PJ_SUCCESS;
    }

    *frame_cnt = 1;

    frames[0].buf       = pkt;
    frames[0].size      = pkt_size;
    frames[0].timestamp = *ts;
    frames[0].type      = PJMEDIA_FRAME_TYPE_AUDIO;

    return PJ_SUCCESS;
}

static pj_status_t clearmode_codec_encode(pjmedia_codec *codec,
                                const pjmedia_frame *input,
                                unsigned output_buf_len,
                                pjmedia_frame *output)
{
    PJ_UNUSED_ARG(codec);


    if (input->size > output_buf_len)
        return PJMEDIA_CODEC_EFRMTOOSHORT;

    if (input->type != PJMEDIA_FRAME_TYPE_AUDIO) {
        output->type = PJMEDIA_FRAME_TYPE_NONE;
        output->size = 0;
        return PJ_SUCCESS;
    }

    pj_memcpy(output->buf, input->buf, input->size/2); // we write to the lower part
    output->size      = input->size/2;
    output->type      = PJMEDIA_FRAME_TYPE_AUDIO;
    output->timestamp = input->timestamp;

    return PJ_SUCCESS;
}

static pj_status_t clearmode_codec_decode(pjmedia_codec *codec,
                                const pjmedia_frame *input,
                                unsigned output_buf_len,
                                pjmedia_frame *output)
{
    PJ_UNUSED_ARG(codec);

    if (input->size > output_buf_len)
        return PJMEDIA_CODEC_EFRMTOOSHORT;

    if (input->type != PJMEDIA_FRAME_TYPE_AUDIO) {
        output->type = PJMEDIA_FRAME_TYPE_NONE;
        output->size = 0;
        return PJ_SUCCESS;
    }

    pj_memcpy(output->buf, input->buf, input->size); // we write to the lower part
    // printf("junk: %s\n",output->buf);
    memset(output->buf+input->size,0,input->size);
    output->size      = input->size*2;
    output->type      = PJMEDIA_FRAME_TYPE_AUDIO;
    output->timestamp = input->timestamp;

    return PJ_SUCCESS;
}

static pj_status_t clearmode_codec_recover(pjmedia_codec *codec,
                                 unsigned output_buf_len,
                                 pjmedia_frame *output)
{
    PJ_UNUSED_ARG(codec);
    PJ_UNUSED_ARG(output_buf_len);

    output->type = PJMEDIA_FRAME_TYPE_NONE;
    output->size = 0;
    return PJ_SUCCESS;
}

/* ===== public init/deinit ===== */

pj_status_t pjmedia_codec_clearmode_init(pjmedia_endpt *endpt)
{
    pj_pool_t *pool;
    clearmode_factory *cf;
    pj_status_t status;

    if (g_factory)
        return PJ_SUCCESS;

    pool = pjmedia_endpt_create_pool(endpt, "clearmode_factory", 512, 512);
    if (!pool)
        return PJ_ENOMEM;

    cf = PJ_POOL_ZALLOC_T(pool, clearmode_factory);
    cf->base.op = &factory_op;
    cf->base.factory_data = NULL;
    cf->endpt = endpt;
    cf->pool  = pool;

    // printf("codec reg %x base op %x, factory %x \n",pjmedia_endpt_get_codec_mgr(endpt),cf->base.op, &cf->base);

    status = pjmedia_codec_mgr_register_factory(
                 pjmedia_endpt_get_codec_mgr(endpt),
                 &cf->base);
    if (status != PJ_SUCCESS) {
        pj_pool_release(pool);
        return status;
    }

    g_factory = cf;
    return PJ_SUCCESS;
}

pj_status_t pjmedia_codec_clearmode_deinit(pjmedia_endpt *endpt)
{
    PJ_UNUSED_ARG(endpt);

    if (!g_factory)
        return PJ_SUCCESS;

    pjmedia_codec_mgr_unregister_factory(
        pjmedia_endpt_get_codec_mgr(g_factory->endpt),
        &g_factory->base);

    clearmode_factory_dealloc_codec(&g_factory->base, NULL);
    /* pool is released in clearmode_factory_dealloc_codec via codec, so just clear */
    g_factory = NULL;

    return PJ_SUCCESS;
}

