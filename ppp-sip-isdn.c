/* ppp-sip-isdn.c
 *
 * SIP CLEARMODE (only) <-> HDLC bitstream <-> PPPD
 * Tested API style for PJSIP 2.16
 */

#include <pjsua-lib/pjsua.h>

#ifndef SECURE_PPP_HELPER
#define SECURE_PPP_HELPER "/usr/local/sbin/ppp-helper"
#endif

#ifndef PPPD_PATH
#define PPPD_PATH "/usr/sbin/pppd"
#endif

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
#include "hdlc-bitstream.h"
#include <wordexp.h>
#include <spawn.h>
#include <dirent.h>

#define PPPD_MAX_ARGS 64
#define T_UDP 0
#define T_TCP 1
#define T_TLS 2

#include <pjmedia/alaw_ulaw.h>

typedef enum {
    LINE_FREE = 0,
    LINE_ACTIVE = 1,
    LINE_STOPPING = 2,
    LINE_RESERVED_DIALOUT = 3
} line_state_t;
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
static volatile sig_atomic_t terminate = 0;
static int linecount = 1;
static int cli_ip6 = 0;
static int cli_srtp = 0;
static int secure_ppp = 0;
static int cli_voice_bearer = 0;

/* ================= PPPD VIA PTY ================= */

typedef struct {
    int   master_fd;
    pid_t pid;
} ppp_link;

static void fmt_ip(char *out, size_t len, uint32_t ip)
{
    if (!ip) { out[0] = 0; return; }
    snprintf(out, len, "%u.%u.%u.%u", (ip&0xff000000)>>24, (ip&0xff0000)>>16, (ip&0xff00)>>8, (ip&0xff));
}

/* Spawns pppd (or the setuid helper) on the slave side of a new PTY.
 * posix_spawn instead of fork: fork() of a multithreaded process takes the mm
 * lock and sets up copy-on-write, which can stall the media threads. */
static int start_pppd(ppp_link *link, const char *args, uint32_t remoteip)
{
    int master_fd, slave_fd;
    char slave_name[64];
    char l[62], r[62];
    char *argv[PPPD_MAX_ARGS+1];
    char *envp_empty[] = { NULL };
    char **envp = envp_empty;
    const char *path;
    int ac = 0;
    wordexp_t we;
    int have_we = 0;
    pid_t pid;
    int rc = -1;

    fmt_ip(l, sizeof(l), cli_ppplocalip);
    fmt_ip(r, sizeof(r), remoteip);

    if (secure_ppp) {
        /* The setuid root helper takes the pts number and validates it itself:
         * "/dev/pts/<n>" is the only form accepted. */
        const char *prefix = "/dev/pts/";
        char *end = NULL;
        if (openpty(&master_fd, &slave_fd, slave_name, NULL, NULL) < 0) {
            perror("openpty");
            return -1;
        }
        if (strncmp(slave_name, prefix, strlen(prefix)) != 0) {
            printf("unexpected pty name %s\n", slave_name);
            goto fail_close;
        }
        strtol(slave_name + strlen(prefix), &end, 10);
        if (!end || *end != '\0') {
            printf("unexpected pty name %s\n", slave_name);
            goto fail_close;
        }
        path = SECURE_PPP_HELPER;
        argv[ac++] = "sip-isdn-pppd";
        argv[ac++] = slave_name + strlen(prefix);
        argv[ac++] = l;
        argv[ac++] = r;
        argv[ac] = NULL;
    } else {
        char localremote[128];
        if (openpty(&master_fd, &slave_fd, slave_name, NULL, NULL) < 0) {
            perror("openpty");
            return -1;
        }
        extern char **environ;
        envp = environ;
        path = PPPD_PATH;
        argv[ac++] = "sip-isdn-pppd"; //let userspace see this one is special
        argv[ac++] = slave_name;
        argv[ac++] = "nodetach";
        if (remoteip || cli_ppplocalip) {
            snprintf(localremote, sizeof(localremote), "%s:%s", l, r);
            argv[ac++] = localremote;
        }
        if (args && wordexp(args, &we, WRDE_NOCMD) == 0) {
            have_we = 1;
            for (size_t i = 0; i < we.we_wordc && ac < PPPD_MAX_ARGS; i++)
                argv[ac++] = we.we_wordv[i];
        }
        argv[ac] = NULL;
    }

    posix_spawn_file_actions_t fa;
    posix_spawnattr_t attr;
    posix_spawn_file_actions_init(&fa);
    posix_spawnattr_init(&attr);
#ifdef POSIX_SPAWN_SETSID
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSID);
#endif
    /* don't leak SIP sockets or other lines' PTYs into pppd */
#if defined(__GLIBC__) && __GLIBC_PREREQ(2, 34)
    posix_spawn_file_actions_addclosefrom_np(&fa, 3);
#else
    {
        /* close every other open descriptor, but not the one opendir() itself holds */
        DIR *d = opendir("/proc/self/fd");
        if (d) {
            int dfd = dirfd(d);
            struct dirent *de;
            while ((de = readdir(d)) != NULL) {
                int fd = atoi(de->d_name);
                if (de->d_name[0] != '.' && fd > 2 && fd != dfd)
                    posix_spawn_file_actions_addclose(&fa, fd);
            }
            closedir(d);
        }
    }
#endif
    if (posix_spawn(&pid, path, &fa, &attr, argv, envp) != 0) {
        printf("spawning %s failed\n", path);
    } else {
        rc = 0;
    }
    posix_spawn_file_actions_destroy(&fa);
    posix_spawnattr_destroy(&attr);
    if (have_we)
        wordfree(&we);
    if (rc != 0)
        goto fail_close;

    close(slave_fd);
    fcntl(master_fd,F_SETFL, (fcntl(master_fd, F_GETFL, 0)|O_NONBLOCK));
    link->master_fd = master_fd;
    link->pid       = pid;
    return 0;

fail_close:
    close(master_fd);
    close(slave_fd);
    return -1;
}

static void stop_pppd(ppp_link *link)
{
    if (link->pid > 0) {
        if(secure_ppp) {
            /* posix_spawn instead of fork: fork() of a multithreaded process
             * takes the mm lock and sets up copy-on-write, which can stall
             * the media threads for milliseconds (see line teardown) */
            char term_pid [32];
            char* args [4];
            pid_t pid;
            snprintf(term_pid,sizeof(term_pid),"%d",link->pid);
            args[0]=SECURE_PPP_HELPER;
            args[1]="stop";
            args[2]=term_pid;
            args[3]=NULL;
            char *envp[] = {
                NULL
            };
            if (posix_spawn(&pid, SECURE_PPP_HELPER, NULL, NULL, args, envp) != 0) {
                printf("calling secure pppd terminate failed\n");
                return;
            }
            waitpid(pid,NULL,0);
        }
        else {
            kill(link->pid, SIGHUP);
        }
    }
    else if (link->master_fd >= 0) {
        close(link->master_fd);
        link->master_fd = -1;
    }
}


/* ================= PPP MEDIA PORT ================= */

typedef struct {
    pjmedia_port base;
    ppp_link     link;

    hdlc_rx_state* rx;
    hdlc_tx_state* tx;
    pj_pool_t *pool;

    uint8_t tx_buf[4096];
    int     tx_len;
    int     tx_pos;
    pjsua_call_id call;
    uint8_t rbuf[1800];
    int rbuf_pos;
    uint64_t rxfrmcnt;
    uint64_t txfrmcnt;
    uint32_t remoteip;
    line_state_t state;
    pjsua_conf_port_id ppp_conf_id;
    volatile int pppd_exited; /* set by media thread, handled by main loop */
    volatile int call_ended;  /* set by pjsip callback, handled by main loop */

} ppp_media_port;

static ppp_media_port * lines;

size_t ppp_escape_pppd(const uint8_t *in, size_t in_len, uint8_t *out, size_t max_len) {
    size_t i = 0, j = 0;

    while (i < in_len) {
        if (in[i] == 0x7E) {            // FLAG byte
            if(max_len<j+2) return j;
            out[j++] = 0x7D;
            out[j++] = 0x5E;
        } else if (in[i] == 0x7D) {     // ESCAPE byte
            if(max_len<j+2) return j;
            out[j++] = 0x7D;
            out[j++] = 0x5D;
        } else if (in[i]<0x20) {
            if(max_len<j+2) return j;
            out[j++] = 0x7D;
            out[j++] = in[i] ^ 0x20; // put this in clear for now
        } else {
            if(max_len<j+1) return j;
            out[j++] = in[i];
        }
        i++;
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
            if(lines[i].state==LINE_FREE) {
                pj_str_t dialstr = pj_str(cli_dial);
                pjsua_call_info ci;
                pjsua_call_get_info(p->call, &ci);
                pjsua_call_id current_call = PJSUA_INVALID_ID;
                pjsua_call_setting call_settings;
                pjsua_call_setting_default(&call_settings);
                call_settings.aud_cnt = 1;
                call_settings.vid_cnt = 0;
                call_settings.txt_cnt = 0;
                printf("\n\n----ADDING LINE %d----\n\n",i+1);
                lines[i].state = LINE_RESERVED_DIALOUT; /* claim it so it isn't picked twice */
                if (pjsua_call_make_call(ci.acc_id, &dialstr, &call_settings, NULL, NULL, &current_call) != PJ_SUCCESS)
                    lines[i].state = LINE_FREE;
                break;
            }
        }
    }
}

pj_status_t ppp_put_frame(pjmedia_port *port,
                                 pjmedia_frame *f)
{
    ppp_media_port *p = (ppp_media_port*)port;

    unsigned needed = f->size/2;
    if((p->state!=LINE_ACTIVE && p->state!=LINE_STOPPING) || p->rx==0)
        return PJ_SUCCESS;

    if (f->type != PJMEDIA_FRAME_TYPE_AUDIO || f->size == 0)
        return PJ_SUCCESS;

    uint16_t *buf = (uint16_t*)f->buf;
    for (unsigned i = 0; i < needed; ++i) {
        if(!cli_voice_bearer)
            hdlc_rx_push_byte(p->rx, buf[i] & 0xff, deliver_ppp_frame, p);
        else
            hdlc_rx_push_byte(p->rx, pjmedia_linear2alaw(buf[i]), deliver_ppp_frame, p);
    }

    return PJ_SUCCESS;
}

size_t ppp_unescape_pppd(const uint8_t *in, size_t in_len, uint8_t *out, size_t max_out_len) {
    size_t i = 0, j = 0;

    while (i < in_len && j < max_out_len) {
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
    if(sizeof(p->rbuf)-p->rbuf_pos==0) {
        printf("emergency flush, should never happen if mtu is < 1800\n");
        p->rbuf_pos=0;
        memset(p->rbuf,0,sizeof(p->rbuf));
    }
    ssize_t rn = read(p->link.master_fd, p->rbuf+p->rbuf_pos, sizeof(p->rbuf)-p->rbuf_pos);

    if (rn > 0) {
        p->rbuf_pos+=rn;
        // printf("got %d bytes from pppd\n",rn);
        int start = 0;
        int delivered = 0;
        int lastFrameEnd = 0;
        while (start<p->rbuf_pos) {
            //search start of frame
            while (start < p->rbuf_pos && p->rbuf[start]!=0x7e) start++;
            int pos = start+1;
            //search end of frame
            while (pos < p->rbuf_pos && p->rbuf[pos]!=0x7e) pos++;
            if(pos < p->rbuf_pos &&
                pos-start>2 && p->rbuf[start]==0x7e && 
                p->rbuf[pos]==0x7e)  { // if we have a frame
                int n = ppp_unescape_pppd(p->rbuf+start+1,pos-start-1,buf,sizeof(buf));
                for (ssize_t i = 0; i < n; i++)
                    hdlc_tx_put_byte(p->tx, buf[i], p->tx_buf, &p->tx_len, sizeof(p->tx_buf));
                delivered++;
                p->txfrmcnt++;
                hdlc_tx_put_flag(p->tx, p->tx_buf, &p->tx_len, sizeof(p->tx_buf));
            }
            if(pos < p->rbuf_pos && p->rbuf[pos]==0x7e)
                lastFrameEnd = pos;
            start = pos;
        }
        // if last frame is not complete, move last frame to the beginning of the buffer
        if(lastFrameEnd<p->rbuf_pos && p->rbuf[lastFrameEnd] == 0x7e) {
            if(p->rbuf_pos-lastFrameEnd > 1) {
                memmove(p->rbuf,p->rbuf+lastFrameEnd,p->rbuf_pos-lastFrameEnd);
                p->rbuf_pos = p->rbuf_pos-lastFrameEnd;
            }
            else {
                p->rbuf[0] = 0x7e;
                p->rbuf_pos = 1;
            }
        }
        // if at least one frame got delivered and all frames are complete, put a start of frame
        // on the beginning so we have start and end the next time.
        // pppd sometimes uses 0x7e as stop and start, this is essentially a workaround
        else if(delivered>0){
            p->rbuf[0] = 0x7e;
            p->rbuf_pos = 1;
        }
    }
    // if we have no more data, fill with 0x7e
    if (p->tx_len == 0)
        for(unsigned i=0;i<minimum;i++)
            hdlc_tx_put_flag(p->tx, p->tx_buf, &p->tx_len, sizeof(p->tx_buf));
}

static pj_status_t ppp_destroy(pjmedia_port *port);

static pj_status_t ppp_get_frame(pjmedia_port *port,
                                 pjmedia_frame *f)
{
    ppp_media_port *p = (ppp_media_port*)port;
    if((p->state!=LINE_ACTIVE && p->state!=LINE_STOPPING)|| p->tx==0) {
        memset(f->buf,0x7e,f->size);
        return PJ_SUCCESS;
    }
    uint16_t *out = (uint16_t*)f->buf;
    unsigned need = f->size/2;
    unsigned w = 0;
    int ppp_status;
    if(p->pppd_exited ||
       (p->link.pid > 0 && waitpid(p->link.pid, &ppp_status, WNOHANG) != 0)) {
        /* Don't tear down from the media thread: the main loop hangs up
         * the call and removes the port (see handle_pppd_exit). */
        p->pppd_exited = 1;
        memset(f->buf,0x7e,f->size);
        f->type = PJMEDIA_FRAME_TYPE_AUDIO;
        return PJ_SUCCESS;
    }


    while (w < need) {
        if (p->tx_pos >= p->tx_len)
            refill_tx(p, need - w);

        unsigned avail = p->tx_len - p->tx_pos;
        unsigned chunk = (need - w < avail) ? (need - w) : avail;
        for(int i = 0;i<chunk;i++) {
            if(!cli_voice_bearer)
                out[w + i]=(p->tx_buf[p->tx_pos + i] & 0xff);
            else
                out[w + i]=pjmedia_alaw2linear(p->tx_buf[p->tx_pos + i]);
        }
        p->tx_pos += chunk;
        w += chunk;
    }

    f->type = PJMEDIA_FRAME_TYPE_AUDIO;
    return PJ_SUCCESS;
}

static pj_status_t ppp_destroy(pjmedia_port *port)
{
    ppp_media_port *p = (ppp_media_port*)port;
    stop_pppd(&p->link);
    for(int i=0;i<linecount;i++) {
        if(lines+i == p && lines[i].state == LINE_ACTIVE) {
            pjsua_conf_remove_port(lines[i].ppp_conf_id);
            lines[i].state = (p->link.pid>0?LINE_STOPPING:LINE_FREE);
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
    p->state         = LINE_ACTIVE;
    p->pppd_exited   = 0;
    p->call_ended    = 0;

    if (start_pppd(&p->link, pppd_args, p->remoteip) != 0)
        return PJ_EUNKNOWN;
    if(!p->rx)
        return PJ_ENOMEM;
    if(!p->tx)
        return PJ_ENOMEM;

    hdlc_rx_init(p->rx); //reset
    hdlc_tx_init(p->tx); //reset

    return PJ_SUCCESS;
}

/* Called from the main loop when the media thread noticed pppd exiting */
static void handle_pppd_exit(ppp_media_port *p)
{
    printf("PPP Line went down\n");
    p->link.pid = 0; // already down, no need to send HUP to pppd
    p->pppd_exited = 0;
    pjsua_call_hangup(p->call,0,NULL,NULL);
    if (p->state == LINE_ACTIVE)
        ppp_destroy((pjmedia_port *)p);
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

static void force_pcma_only(void)
{
    unsigned count = 64;
    pjsua_codec_info ci[64];

    if (pjsua_enum_codecs(ci, &count) != PJ_SUCCESS)
        return;

    for (unsigned i = 0; i < count; ++i) {
        pj_str_t name = ci[i].codec_id;
        if (pj_strnicmp2(&name, "PCMA", 4) == 0) {
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
    if(!cli_dial)
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
        
        /* dial-out calls own a reserved line; prefer it over any free one */
        if(cli_dial) {
            for(int i=0;i<linecount && !ppp;i++)
                if(lines[i].state==LINE_RESERVED_DIALOUT)
                    ppp = &(lines[i]);
        }
        for(int i=0;i<linecount && !ppp;i++) {
            if(lines[i].state==LINE_FREE)
                ppp = &(lines[i]);
        }
        if(!ppp) {
            printf("no free line found\n");
            pjsua_call_hangup(cid,0,NULL,NULL);
            return;
        }
        
        if(!ppp->pool) {
            return;
        }
        pjsua_conf_port_id call_conf = pjsua_call_get_conf_port(cid);

        if (ppp_media_port_reset(cli_pppd, ppp) != PJ_SUCCESS) {
            printf("PPP port failed\n");
            pjsua_call_hangup(cid,0,NULL,NULL);
            return;
        }
        ppp->call = cid;

        pjsua_conf_add_port(ppp->pool, &ppp->base, &ppp->ppp_conf_id);

        pjsua_conf_connect(call_conf, ppp->ppp_conf_id);
        pjsua_conf_connect(ppp->ppp_conf_id, call_conf);
        pjsua_call_set_user_data(cid, ppp);
        printf("PPP bridge active (CLEARMODE only)\n");
    }
}

/* with --loglevel > 0, print RTP / jitter buffer statistics when a stream ends:
 * dropped or discarded packets there show up as lost HDLC data downstream */
static void on_stream_destroyed(pjsua_call_id cid, pjmedia_stream *strm,
                                unsigned stream_idx)
{
    pjmedia_rtcp_stat st;
    pjmedia_jb_state jb;
    PJ_UNUSED_ARG(stream_idx);
    if (cli_loglevel <= 0)
        return;
    if (pjmedia_stream_get_stat(strm, &st) == PJ_SUCCESS &&
        pjmedia_stream_get_stat_jbuf(strm, &jb) == PJ_SUCCESS)
        printf("STREAMSTAT call=%d rx_pkt=%u rx_loss=%u rx_discard=%u rx_reorder=%u "
               "jb_lost=%u jb_discard=%u jb_empty=%u jb_delay_avg=%ums max=%ums\n",
               cid, (unsigned)st.rx.pkt, (unsigned)st.rx.loss, (unsigned)st.rx.discard,
               (unsigned)st.rx.reorder, jb.lost, jb.discard, jb.empty,
               jb.avg_delay, jb.max_delay);
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
            /* This runs on pjsip's worker thread, which also receives RTP for
             * all other lines: don't fork/wait/lock here, the main loop does
             * the teardown (a few ms of blocking is enough to starve the
             * jitter buffers of the other lines). */
            if(ppp->state == LINE_ACTIVE)
                ppp->call_ended = 1;
        }
        else if(cli_dial) {
            /* call failed before media came up: release its reserved line */
            for(int i=0;i<linecount;i++) {
                if(lines[i].state==LINE_RESERVED_DIALOUT) {
                    lines[i].state = LINE_FREE;
                    break;
                }
            }
        }
        for(int i=0;i<linecount;i++) {
            activecnt += (lines[i].state!=LINE_FREE&&lines[i].state!=LINE_RESERVED_DIALOUT?1:0);
        }
        if(cli_dial && activecnt == 0)
            terminate = 1;
    }

}

/* ================= CLI PARSER ================= */

static uint32_t parseip(const char* ipparam)
{
    struct in_addr addr;
    if (inet_pton(AF_INET, ipparam, &addr) != 1)
        return 0;
    return ntohl(addr.s_addr);
}

static void parse_cli(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--id"))   cli_id   = argv[++i];
        else if (!strcmp(argv[i], "--reg"))  cli_reg  = argv[++i];
        else if (!strcmp(argv[i], "--user")) {
            cli_user = argv[++i];
            if(getenv("SIP_PASS")!=0)
                cli_pass = strdup(getenv("SIP_PASS"));
        }
        else if (!strcmp(argv[i], "--pass")) {
            if(cli_pass)
                free(cli_pass);
            cli_pass = strdup(argv[++i]);
            memset(argv[i],'X',strlen(cli_pass));
        }
        else if (!strcmp(argv[i], "--dial")) cli_dial = argv[++i];
        else if (!strcmp(argv[i], "--pppd")) {
            cli_pppd = argv[++i];
            secure_ppp = 0;
        }
        else if (!strcmp(argv[i], "--bindport")) cli_port = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--loglevel")) cli_loglevel = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--linecount")) linecount = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--pppremoteipstart")) cli_pppremoteipstart = parseip(argv[++i]);
        else if (!strcmp(argv[i], "--ppplocalip")) cli_ppplocalip = parseip(argv[++i]);
        else if (!strcmp(argv[i], "--ipv6")) cli_ip6=1;
        else if (!strcmp(argv[i], "--srtp")) cli_srtp=1;
        else if (!strcmp(argv[i], "--pcma")) cli_voice_bearer=1;
    }

    if (!cli_id || (cli_reg && (!cli_user || !cli_pass))) {
        fprintf(stderr,
            "Required: --id, --user and --pass if --reg. --pass might be also set by the environment variable SIP_PASS\n");
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

static void configure_dns_resolver(pjsua_config* cfg,pj_pool_t* pjsua_pool) {
    FILE *resolv_conf;
    char line[256];
    cfg->nameserver_count = 0;

    resolv_conf = fopen("/etc/resolv.conf", "r");
    if (!resolv_conf) {
        return;
    }

    while ( fgets(line, sizeof(line), resolv_conf) && cfg->nameserver_count < 4) {
        // Skip lines that don't start with "nameserver"
        if (strncmp(line, "nameserver", 10) != 0) {
            continue;
        }
        int l = strlen(line);
        if(line[l-1]=='\n') line[l-1]='\0';
            if(strncmp(line,"nameserver",10)==0) {
                int ipstart = 11;
                while(line[ipstart]==' ' || line[ipstart]=='\t') ipstart++;
                pj_str_t resolver = pj_str(line+ipstart);
                pj_strdup(pjsua_pool,&cfg->nameserver[cfg->nameserver_count],&resolver);
                cfg->nameserver_count++;
            }
    }
    fclose(resolv_conf);
}

/* ================= MAIN ================= */

static void on_signal(int sig)
{
    (void)sig;
    terminate = 1;
}

int main(int argc, char **argv)
{
    secure_ppp = 1;
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    parse_cli(argc, argv);

    lines = calloc(sizeof(ppp_media_port),linecount);
    if(!lines)
        exit(1);

    pjsua_create();
    for(int i=0;i<linecount;i++) {
        lines[i].state=LINE_FREE;
        lines[i].rx=hdlc_rx_new();
        lines[i].tx=hdlc_tx_new();
        lines[i].link.master_fd=-1;
        lines[i].link.pid=0;
        char poolname[PJ_MAX_OBJ_NAME];
        memset(poolname,0,PJ_MAX_OBJ_NAME);
        snprintf(poolname,PJ_MAX_OBJ_NAME-1,"mp-%lx",(long unsigned int)i);
        lines[i].pool = pjsua_pool_create(poolname, 512, 512);
        if(cli_pppremoteipstart) {
            lines[i].remoteip = cli_pppremoteipstart++;
            // don't use .0 and .255. Should be valid ip addresses but might make problems
            if ((lines[i].remoteip%256)==255) lines[i].remoteip = cli_pppremoteipstart++;
            if ((lines[i].remoteip%256)==0) lines[i].remoteip = cli_pppremoteipstart++;
        }
    }

    pjsua_config cfg;
    pjsua_logging_config log;
    pjsua_media_config media;

    pjsua_config_default(&cfg);
    cfg.cb.on_incoming_call   = on_incoming_call;
    cfg.cb.on_call_media_state= on_call_media_state;
    cfg.cb.on_call_state      = on_call_state;
    cfg.cb.on_stream_destroyed = on_stream_destroyed;

    pj_pool_t* pjsua_pool = pjsua_pool_create("pjsua", 128, 128);

    configure_dns_resolver(&cfg,pjsua_pool);

    pjsua_logging_config_default(&log);
    log.console_level = cli_loglevel;

    pjsua_media_config_default(&media);

    media.clock_rate = 8000;
    media.snd_clock_rate = 8000;
    media.no_vad = 1;
    
    int transport = T_UDP;
    if(cli_reg && strncmp(cli_reg,"sips:",5)==0)
        transport = T_TLS;
    else if(cli_reg && strcasestr(cli_reg,"transport=tcp")!=NULL)
        transport = T_TCP;
    else if(cli_reg && strcasestr(cli_reg,"transport=tls")!=NULL)
        transport = T_TLS;
    else if(cli_dial && strncmp(cli_dial,"sips:",5)==0)
        transport = T_TLS;
    else if(cli_dial && strcasestr(cli_dial,"transport=tcp")!=NULL)
        transport = T_TCP;
    else if(cli_dial && strcasestr(cli_dial,"transport=tls")!=NULL)
        transport = T_TLS;

    if(cli_srtp) {
        cfg.use_srtp = (transport==T_TLS?PJMEDIA_SRTP_MANDATORY:PJMEDIA_SRTP_OPTIONAL);
        cfg.srtp_secure_signaling = 0;
        pjsua_srtp_opt_default(&cfg.srtp_opt);
        cfg.srtp_opt.keying_count=2;
        cfg.srtp_opt.keying[0] = PJMEDIA_SRTP_KEYING_SDES;
        cfg.srtp_opt.keying[1] = PJMEDIA_SRTP_KEYING_DTLS_SRTP;
    }
    
    pjsua_init(&cfg, &log, &media);

    /* Register all built-in audio codecs (includes CLEARMODE) */
    pjmedia_endpt *endpt = pjsua_get_pjmedia_endpt();
    pjmedia_codec_register_audio_codecs(endpt, NULL);
    pjmedia_codec_clearmode_init(endpt);

    /* Force CLEARMODE as the only offered codec */
    if(!cli_voice_bearer)
        force_clearmode_only();
    else
        force_pcma_only();

    pjsua_transport_config tcfg;
    pjsua_transport_config_default(&tcfg);
    tcfg.port = cli_port;
    pj_status_t status;

    status = pjsua_transport_create(
        (transport==T_UDP?PJSIP_TRANSPORT_UDP:
            (transport==T_TLS?PJSIP_TRANSPORT_TLS:PJSIP_TRANSPORT_TCP)
        ) + (cli_ip6?PJSIP_TRANSPORT_IPV6:0), &tcfg, NULL);

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
    if(cli_ip6) {
        acc.ipv6_sip_use=PJSUA_IPV6_ENABLED_USE_IPV6_ONLY;
        acc.ipv6_media_use=PJSUA_IPV6_ENABLED_USE_IPV6_ONLY;
    }

    if(cli_srtp) {
        acc.use_srtp = (transport<2?PJMEDIA_SRTP_OPTIONAL:PJMEDIA_SRTP_MANDATORY);
        acc.srtp_secure_signaling = 0;
        pjsua_srtp_opt_default(&acc.srtp_opt);
        acc.srtp_opt.keying_count=2;
        acc.srtp_opt.keying[0] = PJMEDIA_SRTP_KEYING_SDES;
        acc.srtp_opt.keying[1] = PJMEDIA_SRTP_KEYING_DTLS_SRTP;
    }
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
        lines[0].state=LINE_RESERVED_DIALOUT;
        printf("Dialing %s\n", cli_dial);
        pj_str_t dialstr = pj_str(cli_dial);
        pjsua_call_id current_call = PJSUA_INVALID_ID;
        pjsua_call_setting call_settings;
        pjsua_call_setting_default(&call_settings);
        call_settings.aud_cnt = 1;
        call_settings.vid_cnt = 0;
        call_settings.txt_cnt = 0; // Ensure this is zero to prevent m=text
        pjsua_call_make_call(acc_id, &dialstr, &call_settings, NULL, NULL, &current_call);
    } else {
        printf("Waiting for incoming data call...\n");
    }

    while(!terminate) {
        pj_thread_sleep(250);
        int activecnt = 0;
        for(int i=0;i<linecount;i++) {
            if(lines[i].state==LINE_ACTIVE && lines[i].pppd_exited)
                handle_pppd_exit(&lines[i]);
            else if(lines[i].state==LINE_ACTIVE && lines[i].call_ended)
                ppp_destroy((pjmedia_port *)&lines[i]);
            if(lines[i].state==LINE_STOPPING) {
                int ppp_status;
                if((waitpid(lines[i].link.pid, &ppp_status, WNOHANG)) != 0) {
                    printf("pppd terminated on line %d after disconnect\n", i);
                    lines[i].link.pid=0;
                    lines[i].state=LINE_FREE;
                    if(lines[i].link.master_fd>=0) {
                        close(lines[i].link.master_fd);
                        lines[i].link.master_fd=-1;
                    }
                }
            }
            if(lines[i].state!=LINE_FREE)
                activecnt++;
        }
        if(cli_dial && activecnt == 0)
            terminate = 1;
    }

    /* clean shutdown: drop calls, stop pppd instances, wait briefly for them */
    pjsua_call_hangup_all();
    for(int i=0;i<linecount;i++) {
        if(lines[i].state==LINE_ACTIVE)
            ppp_destroy((pjmedia_port *)&lines[i]);
    }
    for(int t=0;t<20;t++) {
        int pending = 0;
        for(int i=0;i<linecount;i++) {
            if(lines[i].state==LINE_STOPPING) {
                if(lines[i].link.pid>0 && waitpid(lines[i].link.pid, NULL, WNOHANG) == 0)
                    pending++;
                else {
                    lines[i].link.pid = 0;
                    lines[i].state = LINE_FREE;
                }
            }
        }
        if(!pending) break;
        pj_thread_sleep(100);
    }
    for(int i=0;i<linecount;i++) {
        if(lines[i].link.master_fd>=0) {
            close(lines[i].link.master_fd);
            lines[i].link.master_fd = -1;
        }
        hdlc_tx_free(lines[i].tx);
        hdlc_rx_free(lines[i].rx);
        pj_pool_release(lines[i].pool);
    }
    pj_pool_release(pjsua_pool);
    pjsua_destroy();
    return 0;
}

