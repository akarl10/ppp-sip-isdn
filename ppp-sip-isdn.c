/* clearmode_ppp.c
 *
 * SIP CLEARMODE (only) <-> HDLC bitstream <-> PPPD
 * Tested API style for PJSIP 2.16
 */

#include <pjsua-lib/pjsua.h>

#include <pty.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <signal.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include <fcntl.h>
#include "clearmode_codec.h"

/* ================= CLI PARAMS ================= */

static char *cli_id   = NULL;
static char *cli_reg  = NULL;
static char *cli_user = NULL;
static char *cli_pass = NULL;
static char *cli_dial = NULL;
static char *cli_pppd = NULL;
static uint32_t cli_pppremoteipstart = 0;
static uint32_t cli_ppplocalip = 0;
static int cli_port = 0;
static int cli_loglevel = 0;
static int terminate = 0;
static int linecount = 1;
static int cli_ip6 = 0;

/* ================= PPPD VIA PTY ================= */

typedef struct {
    int   master_fd;
    pid_t pid;
} ppp_link;

static int start_pppd(ppp_link *link, const char *args, uint32_t remoteip)
{
    int master_fd, slave_fd;
    char slave_name[64];

    if (openpty(&master_fd, &slave_fd, slave_name, NULL, NULL) < 0) {
        perror("openpty");
        return -1;
    }

    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        close(master_fd);
        close(slave_fd);
        return -1;
    }

    if (pid == 0) {
        /* Child */
        close(master_fd);
        setsid();
        close(slave_fd);

        char *argv[64];
        int ac = 0;
        argv[ac++] = "pppd";
        argv[ac++] = slave_name;

        char *tmp = strdup(args);
        char *tok = strtok(tmp, " ");
        while (tok && ac < 63) {
            argv[ac++] = tok;
            tok = strtok(NULL, " ");
        }
        char localremote[128]; // 8*3(digits) + 3*2 (.) + 1 (:) + 1 (\0), 32 bytes, but compiler complains
        if(remoteip || cli_ppplocalip) {
            localremote[0]=0;
            char l[62];
            char r[62];
            if(cli_ppplocalip) {
                sprintf(l,"%u.%u.%u.%u",(cli_ppplocalip&0xff000000)>>24,(cli_ppplocalip&0xff0000)>>16,(cli_ppplocalip&0xff00)>>8,(cli_ppplocalip&0xff));
            }
            if(remoteip) {
                sprintf(r,"%u.%u.%u.%u",(remoteip&0xff000000)>>24,(remoteip&0xff0000)>>16,(remoteip&0xff00)>>8,(remoteip&0xff));
            }
            sprintf(localremote,"%s:%s",l,r);
            argv[ac++] = localremote;
        }
        argv[ac] = NULL;

        execvp("pppd", argv);
        _exit(1);
    }

    close(slave_fd);
    fcntl(master_fd,F_SETFL, (fcntl(master_fd, F_GETFL, 0)|O_NONBLOCK));
    link->master_fd = master_fd;
    link->pid       = pid;
    return 0;
}

static void stop_pppd(ppp_link *link)
{
    if (link->pid > 0) {
        kill(link->pid, SIGTERM);
        waitpid(link->pid, NULL, 0);
        link->pid = 0;
    }
    if (link->master_fd >= 0) {
        close(link->master_fd);
        link->master_fd = -1;
    }
}

/* ================= HDLC RX (bitstream -> PPP bytes) ================= */

typedef struct {
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

static void hdlc_rx_init(hdlc_rx_state *s)
{
    memset(s, 0, sizeof(*s));
}

typedef void (*hdlc_frame_cb)(const uint8_t*, int, void*);

static void hdlc_rx_push_byte(hdlc_rx_state *s, uint8_t b,
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

            if (s->in_frame && s->frame_len > 0)
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

typedef struct {
    uint64_t bitbuf;
    int      bitcnt;
    int      ones;
} hdlc_tx_state;

static void hdlc_tx_init(hdlc_tx_state *s)
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

static void hdlc_tx_put_byte(hdlc_tx_state *s, uint8_t b,
                             uint8_t *out, int *out_len, int max)
{
    for (int i = 7; i >= 0; --i)
        hdlc_tx_put_bit(s, ((b << i) & 0x80?1:0), out, out_len, max);
}

static void hdlc_tx_put_flag(hdlc_tx_state *s,
                             uint8_t *out, int *out_len, int max)
{
    s->ones = 0;
    for (int i = 7; i >= 0; --i)
        hdlc_tx_put_bit_raw(s, (0x7E >> i) & 1, out, out_len, max);

}

/* ================= PPP MEDIA PORT ================= */

typedef struct {
    pjmedia_port base;
    ppp_link     link;

    hdlc_rx_state rx;
    hdlc_tx_state tx;

    uint8_t tx_buf[4096];
    int     tx_len;
    int     tx_pos;
    pjsua_call_id call;
    uint8_t rbuf[1800];
    int rbuf_pos;
    uint64_t rxfrmcnt;
    uint64_t txfrmcnt;
    uint32_t remoteip;
    uint8_t active;

} ppp_media_port;

static ppp_media_port * lines;

size_t ppp_escape_pppd(const uint8_t *in, size_t in_len, uint8_t *out, size_t max_len) {
    size_t i = 0, j = 0;

    while (i < in_len) {
        if (in[i] == 0x7E) {            // FLAG byte
            out[j++] = 0x7D;
            out[j++] = 0x5E;
        } else if (in[i] == 0x7D) {     // ESCAPE byte
            out[j++] = 0x7D;
            out[j++] = 0x5D;
        } else if (in[i]<0x20) {
            out[j++] = 0x7D;
            out[j++] = in[i] ^ 0x20; // put this in clear for now
        } else {
            out[j++] = in[i];
        }
        i++;
        if(max_len<j) return j;
    }
    return j; // number of bytes written to out[]
}


static void deliver_ppp_frame(const uint8_t *data, int len, void *user)
{
    ppp_media_port *p = (ppp_media_port*)user;
    uint8_t buf[4192];
    if (len > 0 && len < 3128) {
        buf[0]=0x7e;
        size_t datalen = ppp_escape_pppd(data,len,buf+1, sizeof(buf)-2);
        buf[datalen+1]=0x7e;
        p->rxfrmcnt++;
        // we pre and append 0x7e, so the data to write is +2
        size_t wl = write(p->link.master_fd, buf, datalen+2);
        if(wl<(datalen+2))
            printf("short write\n");
    }
    //if peer responded and actually sent something like a ppp frame, check if additional lines should be added
    //do this only on the first packet. the first time there will never arrive 2 at the same time
    if(cli_dial && p->rxfrmcnt==1) {
        for(int i=0;i<linecount;i++) {
            if(!lines[i].active) {
                pj_str_t dialstr = pj_str(cli_dial);
                pjsua_call_info ci;
                pjsua_call_get_info(p->call, &ci);
                pjsua_call_id current_call = PJSUA_INVALID_ID;
                printf("\n\n----ADDING LINE %d----\n\n",i+1);
                pjsua_call_make_call(ci.acc_id, &dialstr, 0, NULL, NULL, &current_call);
                i = linecount;
            }
        }
    }
}

pj_status_t ppp_put_frame(pjmedia_port *port,
                                 pjmedia_frame *f)
{
    ppp_media_port *p = (ppp_media_port*)port;

    int needed = f->size;

    // 320 means pjsip insists on PCM data.
    if(f->size==320) { //the other end only wrote to the lower half if pjsip runs in PCM mode somewhere
        needed = 160;
    }
    if (f->type != PJMEDIA_FRAME_TYPE_AUDIO || f->size == 0)
        return PJ_SUCCESS;

    uint8_t *buf = (uint8_t*)f->buf;
    for (unsigned i = 0; i < needed; ++i) {
        hdlc_rx_push_byte(&p->rx, buf[i], deliver_ppp_frame, p);
    }

    return PJ_SUCCESS;
}

size_t ppp_unescape_pppd(const uint8_t *in, size_t in_len, uint8_t *out) {
    size_t i = 0, j = 0;

    while (i < in_len) {
        if (in[i] == 0x7D) {            // Escape byte
            if (i + 1 < in_len) {       // Ensure next byte exists
                out[j++] = in[i + 1] ^ 0x20;
                i += 2;
            } else {
                // Malformed frame: escape at end
                break;
            }
        } else {
            out[j++] = in[i++];
        }
    }

    return j; // number of bytes written to out[]
}


static void refill_tx(ppp_media_port *p, unsigned minimum)
{
    p->tx_len = 0;
    p->tx_pos = 0;

    uint8_t buf[1800];
    memset(buf,0,sizeof(buf));
    ssize_t rn = read(p->link.master_fd, p->rbuf+p->rbuf_pos, sizeof(p->rbuf)-p->rbuf_pos);

    if (rn > 0) {
        p->rbuf_pos+=rn;
        // printf("got %d bytes from pppd\n",rn);
        int start = 0;
        int delivered = 0;
        while (start<rn) {
            //search start of frame
            while (p->rbuf[start]!=0x7e && start < p->rbuf_pos) start++;
            int pos = start+1;
            //search end of frame
            while (p->rbuf[pos]!=0x7e && pos < p->rbuf_pos) pos++;
            if(pos-start>2 && p->rbuf[start]==0x7e && p->rbuf[pos]==0x7e)  { // if we have a frame
                int n = ppp_unescape_pppd(p->rbuf+start+1,pos-start-1,buf);
                for (ssize_t i = 0; i < n; i++)
                    hdlc_tx_put_byte(&p->tx, buf[i], p->tx_buf, &p->tx_len, sizeof(p->tx_buf));
                delivered++;
                p->txfrmcnt++;
                hdlc_tx_put_flag(&p->tx, p->tx_buf, &p->tx_len, sizeof(p->tx_buf));
            }
            start = pos;
        }
        // if last frame is not complete
        if(start<p->rbuf_pos) {
                if(p->rbuf[start]==0x7e) {
                    if(p->rbuf_pos-start>1)
                        memcpy(p->rbuf,p->rbuf+start,p->rbuf_pos-start);
                    else p->rbuf[0]=0x7e;
                    p->rbuf_pos=p->rbuf_pos-start;
                }
        }
        // if at least one frame got delivered and all frames are complete, put a start of frame
        // on the beginning so we have start and end the next time.
        // pppd sometimes uses 0x7e as stop and start, this is essentially a workaround
        else if(delivered>0){
                p->rbuf_pos=1;
                p->rbuf[0]=0x7e;
        }
    }
    // if we have no more data, fill with 0x7e
    if (p->tx_len == 0)
        for(unsigned i=0;i<minimum;i++)
            hdlc_tx_put_flag(&p->tx, p->tx_buf, &p->tx_len, sizeof(p->tx_buf));
}

static pj_status_t ppp_destroy(pjmedia_port *port);

static pj_status_t ppp_get_frame(pjmedia_port *port,
                                 pjmedia_frame *f)
{
    ppp_media_port *p = (ppp_media_port*)port;

    uint8_t *out = (uint8_t*)f->buf;
    unsigned need = f->size;
    unsigned w = 0;
    int ppp_status;
    if((waitpid(p->link.pid, &ppp_status, WNOHANG)) != 0) {
        pjsua_call_hangup(p->call,0,NULL,NULL);
        f->size=0;
        f->type = PJMEDIA_FRAME_TYPE_AUDIO;
        p->link.pid = 0;
        printf("PPP Line went down\n");
        ppp_destroy(port);
        int activecnt = 0;
        for(int i=0;i<linecount;i++) {
            activecnt += (lines[i].active?1:0);
        }
        if(cli_dial && activecnt == 0)
            terminate = 1;
        return PJ_SUCCESS;
    }
    if(need==320) { //just write the lower 160bytes. if we do the same on both sides it should work
        need=160;
        memset(out+160,0x0,160);
    }

    while (w < need) {
        if (p->tx_pos >= p->tx_len)
            refill_tx(p, need - w);

        unsigned avail = p->tx_len - p->tx_pos;
        unsigned chunk = (need - w < avail) ? (need - w) : avail;

        memcpy(out + w, p->tx_buf + p->tx_pos, chunk);
        p->tx_pos += chunk;
        w += chunk;
    }
    // f->size = w;
    f->type = PJMEDIA_FRAME_TYPE_AUDIO;
    return PJ_SUCCESS;
}

static pj_status_t ppp_destroy(pjmedia_port *port)
{
    ppp_media_port *p = (ppp_media_port*)port;
    stop_pppd(&p->link);
    for(int i=0;i<linecount;i++) {
        if(lines+i == p) {
            lines[i].active = 0;
        }
    }
    return PJ_SUCCESS;
}

static pj_status_t ppp_media_port_reset(const char *pppd_args,
                                         ppp_media_port *p)
{
    pj_str_t pname = pj_str("ppp-port");
    pjmedia_port_info_init(&p->base.info,
                           &pname,
                           PJMEDIA_SIG_CLASS_PORT('P','P','P'),
                           8000, 1, 8, 160);

    p->base.get_frame = ppp_get_frame;
    p->base.put_frame = ppp_put_frame;
    p->base.on_destroy = ppp_destroy;

    p->link.master_fd = -1;
    p->link.pid       = 0;
    p->rbuf_pos       = 0;
    p->txfrmcnt       = 0;
    p->rxfrmcnt       = 0;
    p->active         = 1;

    if (start_pppd(&p->link, pppd_args, p->remoteip) != 0)
        return PJ_EUNKNOWN;

    hdlc_rx_init(&p->rx);
    hdlc_tx_init(&p->tx);

    return PJ_SUCCESS;
}

/* ================= CLEARMODE ONLY SETUP ================= */

static void force_clearmode_only(void)
{
    unsigned count = 64;
    pjsua_codec_info ci[64];

    if (pjsua_enum_codecs(ci, &count) != PJ_SUCCESS)
        return;

    for (unsigned i = 0; i < count; ++i) {
        pj_str_t name = ci[i].codec_id;
        /* codec_id looks like "CLEARMODE/8000/1" */
        if (pj_strnicmp2(&name, "CLEARMODE", 9) == 0) {
            pjsua_codec_set_priority(&ci[i].codec_id, PJMEDIA_CODEC_PRIO_HIGHEST);
        } else {
            pjsua_codec_set_priority(&ci[i].codec_id, 0);
        }
    }
}

/* ================= PJSUA CALLBACKS ================= */


static void on_incoming_call(pjsua_acc_id acc, pjsua_call_id cid,
                             pjsip_rx_data *r)
{
    PJ_UNUSED_ARG(acc);
    PJ_UNUSED_ARG(r);
    pjsua_call_answer(cid, 200, NULL, NULL);
}

static void on_call_media_state(pjsua_call_id cid)
{
    pjsua_call_info ci;
    pjsua_call_get_info(cid, &ci);
    
    if (ci.media_status == PJSUA_CALL_MEDIA_ACTIVE) {
        
        ppp_media_port *ppp;
        ppp = (void*)pjsua_call_get_user_data(cid);
        if(ppp) {
            return;
        }
        
        for(int i=0;i<linecount;i++) {
            if(!lines[i].active) {
                ppp = &(lines[i]);
                i=linecount;
            }
        }
        if(!ppp) {
            printf("no free line found\n");
            pjsua_call_hangup(cid,0,NULL,NULL);
            return;
        }
        
        pjsua_conf_port_id call_conf = pjsua_call_get_conf_port(cid);

        pj_pool_t *pool = pjsua_pool_create("ppp", 1024, 1024);

        if (ppp_media_port_reset(cli_pppd, ppp) != PJ_SUCCESS) {
            printf("PPP port failed\n");
            pjsua_call_hangup(cid,0,NULL,NULL);
            return;
        }
        ppp->call = cid;

        pjsua_conf_port_id ppp_conf;
        pjsua_conf_add_port(pool, &ppp->base, &ppp_conf);

        pjsua_conf_connect(call_conf, ppp_conf);
        pjsua_conf_connect(ppp_conf, call_conf);
        pjsua_call_set_user_data(cid, ppp);
        printf("PPP bridge active (CLEARMODE only)\n");
    }
}

static void on_call_state(pjsua_call_id cid, pjsip_event *e)
{
    PJ_UNUSED_ARG(e);
    pjsua_call_info ci;
    pjsua_call_get_info(cid, &ci);
    printf("Call state %d: %.*s\n",cid,
           (int)ci.state_text.slen, ci.state_text.ptr);
    if(ci.state == PJSIP_INV_STATE_DISCONNECTED) {
        ppp_media_port *ppp;
        ppp = (void*)pjsua_call_get_user_data(cid);
        int activecnt = 0;
        if(ppp) {
            if(ppp->active) {
                ppp_destroy((pjmedia_port *)ppp);
            }
        }
        for(int i=0;i<linecount;i++) {
            activecnt += (lines[i].active?1:0);
        }
        if(cli_dial && activecnt == 0)
            terminate = 1;
    }
}

/* ================= CLI PARSER ================= */

static uint32_t parseip(const char* ipparam)
{
    uint32_t ip = 0;
    int p = 0;
    int l=0;
    char nb[4];
    nb[0]=0;
    while(ipparam[p]!='\0') {
        if(l>3) {// wrong ip format
            return 0;
        }
        if(ipparam[p]!='.') {
            nb[l]=ipparam[p];
            nb[l+1]='\0';
            l++;
        }
        if (ipparam[p+1]=='\0' || ipparam[p]=='.') {
            ip = (ip << 8) | atoi(nb);
            l=0;
            nb[0]='\0';
        }
        p++;
    }
    return ip;
}

static void parse_cli(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--id"))   cli_id   = argv[++i];
        else if (!strcmp(argv[i], "--reg"))  cli_reg  = argv[++i];
        else if (!strcmp(argv[i], "--user")) cli_user = argv[++i];
        else if (!strcmp(argv[i], "--pass")) {
            cli_pass = strdup(argv[++i]);
            memset(argv[i],'X',strlen(cli_pass));
        }
        else if (!strcmp(argv[i], "--dial")) cli_dial = argv[++i];
        else if (!strcmp(argv[i], "--pppd")) cli_pppd = argv[++i];
        else if (!strcmp(argv[i], "--bindport")) cli_port = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--loglevel")) cli_loglevel = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--linecount")) linecount = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--pppremoteipstart")) cli_pppremoteipstart = parseip(argv[++i]);
        else if (!strcmp(argv[i], "--ppplocalip")) cli_ppplocalip = parseip(argv[++i]);
        else if (!strcmp(argv[i], "--ipv6")) cli_ip6=1;
    }

    if (!cli_id || (cli_reg && (!cli_user || !cli_pass))) {
        fprintf(stderr,
            "Required: --id, --user and --pass if --reg\n");
        exit(1);
    }

    if (!cli_pppd)
        cli_pppd = "noauth debug local";
    if (cli_port == 0) { // Random high port (49152–65535)
        srand(time(NULL));
        cli_port = 49152 + (rand() % (65535 - 49152));
    }
}

static void wait_for_registration(pjsua_acc_id acc_id)
{
    for (int i = 0; i < 30; ++i) { // ~3 seconds
        pjsua_acc_info ai;
        pjsua_acc_get_info(acc_id, &ai);
        printf("Registraton state: %d\n",ai.status);
        if (ai.status == 200) {
            return;
        }

        pj_thread_sleep(100);
    }

    printf("Registration did not complete in time\n");
}


/* ================= MAIN ================= */

int main(int argc, char **argv)
{
    parse_cli(argc, argv);

    lines = calloc(sizeof(ppp_media_port),linecount);
    if(!lines)
        exit(1);

    for(int i=0;i<linecount;i++) {
        lines[i].active=0;
        if(cli_pppremoteipstart) lines[i].remoteip = cli_pppremoteipstart++;
    }
    pjsua_create();

    pjsua_config cfg;
    pjsua_logging_config log;
    pjsua_media_config media;

    pjsua_config_default(&cfg);
    cfg.cb.on_incoming_call   = on_incoming_call;
    cfg.cb.on_call_media_state= on_call_media_state;
    cfg.cb.on_call_state      = on_call_state;

    pjsua_logging_config_default(&log);
    log.console_level = cli_loglevel;

    pjsua_media_config_default(&media);

    media.clock_rate = 8000;
    media.snd_clock_rate = 8000;
    pjsua_init(&cfg, &log, &media);

    /* Register all built-in audio codecs (includes CLEARMODE) */
    pjmedia_endpt *endpt = pjsua_get_pjmedia_endpt();
    pjmedia_codec_register_audio_codecs(endpt, NULL);
    pjmedia_codec_clearmode_init(endpt);

    /* Force CLEARMODE as the only offered codec */
    force_clearmode_only();

    pjsua_transport_config tcfg;
    pjsua_transport_config_default(&tcfg);
    tcfg.port = cli_port;
    pj_status_t status;
    if(cli_ip6)
        status = pjsua_transport_create(PJSIP_TRANSPORT_UDP6, &tcfg, NULL);
    else
        status = pjsua_transport_create(PJSIP_TRANSPORT_UDP, &tcfg, NULL);

    if (status != PJ_SUCCESS) {
        char errmsg[PJ_ERR_MSG_SIZE];
        pj_strerror(status, errmsg, sizeof(errmsg));
        printf("Transport creation failed on port %d: %s\n", cli_port, errmsg);
        return 1;
    }

    printf("SIP UDP transport active on port %d\n", cli_port);

    pjsua_start();

    pjsua_set_null_snd_dev();


    pjsua_acc_config acc;
    pjsua_acc_config_default(&acc);
    acc.id = pj_str(cli_id);
    if(cli_ip6)
        acc.ipv6_sip_use=PJSUA_IPV6_ENABLED_USE_IPV6_ONLY;
    if(cli_reg) {
        acc.reg_uri = pj_str(cli_reg);
        acc.cred_count = 1;
        acc.cred_info[0].realm = pj_str("*");
        acc.cred_info[0].scheme = pj_str("digest");
        acc.cred_info[0].username = pj_str(cli_user);
        acc.cred_info[0].data_type = PJSIP_CRED_DATA_PLAIN_PASSWD;
        acc.cred_info[0].data = pj_str(cli_pass);
        acc.proxy_cnt = 1;
        acc.proxy[0] = pj_str(cli_reg); // e.g. "sip:pbx.example.com;lr"
    }

    pjsua_acc_id acc_id;
    status = pjsua_acc_add(&acc, PJ_TRUE, &acc_id);
    if (status != PJ_SUCCESS) {
        printf("acc_add failed\n");
        return 1;
    }
    if(cli_reg)
        wait_for_registration(acc_id);

    if (cli_dial) {
        printf("Dialing (CLEARMODE only) %s\n", cli_dial);
        pj_str_t dialstr = pj_str(cli_dial);
        pjsua_call_id current_call = PJSUA_INVALID_ID;
        pjsua_call_make_call(acc_id, &dialstr, 0, NULL, NULL, &current_call);
    } else {
        printf("Waiting for incoming CLEARMODE data call...\n");
    }

    while(!terminate)
        pj_thread_sleep(1000);
}

