/* Webes felület: HTTP + WebSocket, PIN-védelemmel */
#include "ft891ctl.h"
#include "net.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#ifdef HAVE_SSL
#include <openssl/err.h>
#include <fcntl.h>
#include <openssl/ssl.h>
#include <sys/stat.h>
#include <sys/wait.h>
static SSL_CTX *tls_ctx;
static __thread SSL *cur_ssl;
#endif

#ifdef _WIN32
#define PAGE_SECTION ".section .rdata,\"dr\"\n"
#else
#define PAGE_SECTION ".section .rodata\n"
#endif
__asm__(PAGE_SECTION ".global page_start\npage_start:\n.incbin \"web/index.html\"\n"
        ".global page_end\npage_end:\n.byte 0\n.text\n");
extern const char page_start[], page_end[];

#define MAX_WEB 8
#define TOKENS 16
#define SESSION_MS (30LL * 24 * 3600 * 1000)

static pthread_mutex_t A = PTHREAD_MUTEX_INITIALIZER;
static struct { char tok[33]; long long exp; } tokens[TOKENS];
static int fails;
static long long lock_until;
static int web_used[MAX_WEB];

typedef struct { int fd, tls; struct sockaddr_storage peer; } conn_t;
typedef struct {
    char method[8], path[200], host[128], origin[160], cookie[256], ws_key[64], upgrade[32];
    int  content_len;
    char body[32768];
} req_t;

/* A kapcsolat szálában fut, így a TLS-objektum is szálhoz kötött */
static ssize_t io_recv(int fd, void *b, size_t n) {
#ifdef HAVE_SSL
    if (cur_ssl) { int r = SSL_read(cur_ssl, b, n > 16384 ? 16384 : (int)n); return r > 0 ? r : -1; }
#endif
    return recv(fd, b, n, 0);
}

/* Van-e a TLS-rétegben már beolvasott adat, amit a poll nem lát */
static int io_ready(void) {
#ifdef HAVE_SSL
    return cur_ssl && SSL_pending(cur_ssl) > 0;
#else
    return 0;
#endif
}

static int send_all(int fd, const void *b, size_t n) {
    const char *p = b;
#ifdef HAVE_SSL
    if (cur_ssl) return n == 0 || SSL_write(cur_ssl, b, (int)n) > 0 ? 0 : -1;
#endif
    while (n) {
        ssize_t w = send(fd, p, n, MSG_NOSIGNAL);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) return -1;
        p += w;
        n -= (size_t)w;
    }
    return 0;
}

static const char SEC[] =
    "X-Frame-Options: DENY\r\nX-Content-Type-Options: nosniff\r\nReferrer-Policy: no-referrer\r\n"
    "Content-Security-Policy: default-src 'self'; style-src 'unsafe-inline'; script-src 'unsafe-inline'; "
    "connect-src 'self' ws: wss:; img-src 'self' data:; base-uri 'none'; form-action 'none'\r\n";

static void reply(int fd, const char *status, const char *type, const char *extra,
                  const void *body, size_t n) {
    char h[900];
    int l = snprintf(h, sizeof h,
                     "HTTP/1.1 %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\n"
                     "Cache-Control: no-store\r\nConnection: close\r\n%s%s\r\n",
                     status, type, n, SEC, extra ? extra : "");
    if (send_all(fd, h, (size_t)l) == 0 && n) send_all(fd, body, n);
}

static void reply_txt(int fd, const char *status, const char *body) {
    reply(fd, status, "text/plain; charset=utf-8", NULL, body, strlen(body));
}

/* --- hitelesítés --- */
static int host_is_loopback(const char *h) {
    return !strncmp(h, "127.0.0.1", 9) || !strncasecmp(h, "localhost", 9) || !strncmp(h, "[::1]", 5);
}

static int peer_is_loopback(const struct sockaddr_storage *p) {
    if (p->ss_family == AF_INET)
        return (ntohl(((const struct sockaddr_in *)p)->sin_addr.s_addr) >> 24) == 127;
    if (p->ss_family == AF_INET6) {
        const struct in6_addr *a = &((const struct sockaddr_in6 *)p)->sin6_addr;
        if (IN6_IS_ADDR_LOOPBACK(a)) return 1;
        return IN6_IS_ADDR_V4MAPPED(a) && a->s6_addr[12] == 127;
    }
    return 0;
}

static int ct_eq(const char *a, const char *b, size_t n) {
    unsigned char d = 0;
    for (size_t i = 0; i < n; i++) d |= (unsigned char)(a[i] ^ b[i]);
    return d == 0;
}

static int token_ok(const char *cookie) {
    const char *p = strstr(cookie, "ft891=");
    if (!p) return 0;
    p += 6;
    if (strspn(p, "0123456789abcdef") < 32) return 0;
    long long t = now_ms();
    int ok = 0;
    pthread_mutex_lock(&A);
    for (int i = 0; i < TOKENS; i++)
        if (tokens[i].exp > t && ct_eq(tokens[i].tok, p, 32)) ok = 1;
    pthread_mutex_unlock(&A);
    return ok;
}

static int authed(const conn_t *c, const req_t *r) {
    if (peer_is_loopback(&c->peer) && host_is_loopback(r->host)) return 1;
    return token_ok(r->cookie);
}

/* A böngészőből érkező kérés csak saját magától jöhet (idegen oldal nem adhat PTT-t) */
static int origin_ok(const req_t *r) {
    if (!r->origin[0]) return 1;
    const char *o = strstr(r->origin, "://");
    if (!o) return 0;
    return !strcasecmp(o + 3, r->host);
}

static void do_login(const conn_t *c, const req_t *r) {
    long long t = now_ms();
    char body[160];
    pthread_mutex_lock(&A);
    long long wait = lock_until - t;
    pthread_mutex_unlock(&A);
    if (wait > 0) {
        int l = snprintf(body, sizeof body, "{\"ok\":false,\"wait\":%lld}", wait / 1000 + 1);
        reply(c->fd, "429 Too Many Requests", "application/json", NULL, body, (size_t)l);
        return;
    }
    const char *p = strstr(r->body, "pin=");
    const char *given = p ? p + 4 : "";
    size_t need = strlen(g_cfg.pin);
    int good = strlen(given) == need && ct_eq(given, g_cfg.pin, need);
    if (!good) {
        pthread_mutex_lock(&A);
        fails++;
        if (fails >= 5) {
            int k = fails - 5;
            lock_until = t + (30000LL << (k > 6 ? 6 : k));
        }
        pthread_mutex_unlock(&A);
        log_msg(TR("Hibás PIN-kód érkezett", "Wrong PIN received"));
        reply(c->fd, "403 Forbidden", "application/json", NULL, "{\"ok\":false}", 12);
        return;
    }
    uint8_t rnd[16];
    char tok[33], hdr[140];
    if (random_bytes(rnd, sizeof rnd)) { reply_txt(c->fd, "500 Error", "random"); return; }
    for (int i = 0; i < 16; i++) snprintf(tok + i * 2, 3, "%02x", rnd[i]);
    pthread_mutex_lock(&A);
    fails = 0;
    int idx = 0;
    for (int i = 1; i < TOKENS; i++) if (tokens[i].exp < tokens[idx].exp) idx = i;
    memcpy(tokens[idx].tok, tok, 33);
    tokens[idx].exp = t + SESSION_MS;
    pthread_mutex_unlock(&A);
    snprintf(hdr, sizeof hdr, "Set-Cookie: ft891=%s; Path=/; Max-Age=2592000; HttpOnly; SameSite=Strict\r\n", tok);
    reply(c->fd, "200 OK", "application/json", hdr, "{\"ok\":true}", 11);
}

/* --- JSON állapot --- */
static size_t jstr(char *o, size_t cap, const char *k, const char *v) {
    size_t n = (size_t)snprintf(o, cap, "\"%s\":\"", k);
    for (; *v && n + 8 < cap; v++) {
        unsigned char ch = (unsigned char)*v;
        if (ch == '"' || ch == '\\') { o[n++] = '\\'; o[n++] = (char)ch; }
        else if (ch >= 0x20) o[n++] = (char)ch;
    }
    n += (size_t)snprintf(o + n, cap - n, "\",");
    return n;
}

static int state_json(char *o, size_t cap) {
    rig_state_t s;
    rig_get(&s);
    size_t n = (size_t)snprintf(o, cap, "{\"t\":\"s\",");
#define I(k, v) n += (size_t)snprintf(o + n, cap - n, "\"%s\":%lld,", k, (long long)(v))
#define T(k, v) n += jstr(o + n, cap - n, k, v)
    I("connected", s.connected); T("status", s.status); T("port", s.port); T("ptt_port", s.ptt_port);
    I("baud", s.baud); I("rts_on", s.rts_on); I("menu_ok", s.menu_ok);
    T("fw_main", s.fw_main); T("fw_dsp", s.fw_dsp); T("fw_lcd", s.fw_lcd);
    I("fa", s.freq_a); I("fb", s.freq_b);
    char md[2] = {s.md, 0};
    T("md", md); T("mode", rig_mode_name(&s)); I("bfo_ssb", s.bfo_ssb); I("bfo_cw", s.bfo_cw);
    I("mem_mode", s.mem_mode); T("channel", s.channel);
    I("split", s.split); I("clar", s.clar); I("clar_ofs", s.clar_ofs);
    I("tx", s.tx); I("keyed", s.keyed); I("sm", s.smeter); I("po", s.po); I("swr", s.swr); I("alc", s.alc);
    I("af", s.lvl[L_AF]); I("rf", s.lvl[L_RF]); I("sql", s.lvl[L_SQL]); I("pwr", s.lvl[L_PWR]);
    I("mic", s.lvl[L_MIC]); I("nrl", s.lvl[L_NRL]); I("nbl", s.lvl[L_NBL]); I("procl", s.lvl[L_PROC]);
    I("nr", s.fn[FN_NR]); I("nb", s.fn[FN_NB]); I("notch", s.fn[FN_NOTCH]); I("amp", s.fn[FN_AMP]);
    I("att", s.fn[FN_ATT]); I("proc", s.fn[FN_PROC]); I("lock", s.fn[FN_LOCK]);
    I("eq", s.fn[FN_EQ]); I("lower", rig_lower_side(&s)); I("pitch", s.pitch);
    I("ifs_on", s.ft[FT_IFS_ON]); I("ifs", s.ft[FT_IFS]); I("notch_on", s.ft[FT_NOTCH_ON]); I("notch_hz", s.ft[FT_NOTCH]);
    I("cont_on", s.ft[FT_CONT_ON]); I("cont", s.ft[FT_CONT]); I("apf_on", s.ft[FT_APF_ON]); I("apf", s.ft[FT_APF]);
    { char wf[160]; audio_status(wf, sizeof wf); T("wf_status", wf); }
    T("lang", g_cfg.lang_en ? "en" : "hu"); I("tls_port", g_cfg.tls_port); I("agc", s.agc); I("width_idx", s.width_idx); I("narrow", s.narrow); I("width_hz", rig_width_hz(&s));
    { int pm, ml, tm; char dev[128];
      rig_cfg_get(&pm, &ml, &tm, dev, sizeof dev);
      I("ptt_method", pm); I("mode_lock", ml); I("tx_max", tm); T("wf_dev", dev); }
    I("drops", s.ptt_drops); I("timeouts", s.timeouts); I("frames", s.poll_cnt);
    T("job", s.job); I("job_i", s.job_i); I("job_n", s.job_n); I("data_seq", s.data_seq);
    I("notice_seq", s.notice_seq); I("notice_bad", s.notice_bad); T("notice", s.notice);
    { sb_t b = {0}; sstv_state(&b); if (b.p) { n += (size_t)snprintf(o + n, cap - n, "%s", b.p); free(b.p); } }
    n += (size_t)snprintf(o + n, cap - n, "\"menu\":{");
    for (int i = 0; i < s.menu_n; i++)
        n += (size_t)snprintf(o + n, cap - n, "%s\"%s\":\"%s\"", i ? "," : "", s.menu_id[i], s.menu_val[i]);
    n += (size_t)snprintf(o + n, cap - n, "}}");
    return (int)n;
}

/* --- WebSocket --- */
static int ws_send(int fd, const char *txt, size_t n) {
    uint8_t h[4];
    size_t hl;
    h[0] = 0x81;
    if (n < 126) { h[1] = (uint8_t)n; hl = 2; }
    else { h[1] = 126; h[2] = (uint8_t)(n >> 8); h[3] = (uint8_t)n; hl = 4; }
    if (send_all(fd, h, hl)) return -1;
    return send_all(fd, txt, n);
}

/* Az első bájt a típus: 1 vízesés, 2 vételi hang, 3 SSTV-sor */
static int ws_send_bin(int fd, int type, const uint8_t *d, size_t n) {
    uint8_t f[2500];
    if (n + 5 > sizeof f) return 0;
    size_t t = n + 1;
    f[0] = 0x82; f[1] = 126; f[2] = (uint8_t)(t >> 8); f[3] = (uint8_t)t; f[4] = (uint8_t)type;
    memcpy(f + 5, d, n);
    return send_all(fd, f, n + 5);
}

static void ws_cmd(char *line, int slot, int *wf, int *rx) {
    char v[16] = "";
    long long a = 0, b = 0;
    int k = sscanf(line, "%15s %lld %lld", v, &a, &b);
    if (k < 1) return;
    if (!strcmp(v, "wf") && k >= 2) {
        if ((a != 0) != *wf) { *wf = a != 0; audio_want(*wf ? 1 : -1); }
    }
    else if (!strcmp(v, "rx") && k >= 2) {
        int on = a != 0;
        if (on != *rx) {
            *rx = on;
            audio_want(on ? 1 : -1);
        }
    }
    else if (!strcmp(v, "txa") && k >= 2) {
        if (a) audio_tx_begin(slot); else audio_tx_end(slot);
    }
    else if (!strcmp(v, "sstv") && k >= 2) sstv_cmd("on", (int)a);
    else if (!strcmp(v, "sstvgo") && k >= 2) sstv_cmd("go", (int)a);
    else if (!strcmp(v, "sstvstop")) sstv_cmd("stop", 0);
    else if (!strcmp(v, "lang") && k >= 2) set_lang(a != 0);
    else if (!strcmp(v, "power") && k >= 2) rig_post(OP_POWER, a != 0, 0);
    else if (!strcmp(v, "wfdev")) {
        char *d = line + 6;
        d[strcspn(d, "\r\n")] = 0;
        if (audio_set_dev(d)) rig_notice(1, TR("Nincs ilyen hangeszköz.", "No such sound device."));
    }
    else if (!strcmp(v, "filt") && k >= 3) rig_post(OP_FILT, a, b);
    else if (!strcmp(v, "ptt")) rig_hold(SRC_WEB(slot), a != 0, a ? 1200 : 0);
    else if (!strcmp(v, "fa") && k >= 2) rig_post(OP_FREQ_A, a, 0);
    else if (!strcmp(v, "fb") && k >= 2) rig_post(OP_FREQ_B, a, 0);
    else if (!strcmp(v, "mode") && k >= 2) rig_post(OP_MODE, a, 0);
    else if (!strcmp(v, "band") && k >= 2) rig_post(OP_BAND, a, 0);
    else if (!strcmp(v, "lvl") && k >= 3) rig_post(OP_LEVEL, a, b);
    else if (!strcmp(v, "fn") && k >= 3) rig_post(OP_FUNC, a, b);
    else if (!strcmp(v, "agc") && k >= 2) rig_post(OP_AGC, a, 0);
    else if (!strcmp(v, "width") && k >= 2) rig_post(OP_WIDTH, a, 0);
    else if (!strcmp(v, "swap")) rig_post(OP_SWAP, 0, 0);
    else if (!strcmp(v, "a2b")) rig_post(OP_A2B, 0, 0);
    else if (!strcmp(v, "b2a")) rig_post(OP_B2A, 0, 0);
    else if (!strcmp(v, "split") && k >= 2) rig_post(OP_SPLIT, a, 0);
    else if (!strcmp(v, "clar") && k >= 2) rig_post(OP_CLAR, a, 0);
    else if (!strcmp(v, "clarclr")) rig_post(OP_CLAR_CLEAR, 0, 0);
    else if (!strcmp(v, "clarstep") && k >= 2) rig_post(OP_CLAR_STEP, a, 0);
    else if (!strcmp(v, "pttm") && k >= 2) rig_post(OP_PTT_METHOD, a, 0);
    else if (!strcmp(v, "ptttest")) rig_post(OP_PTT_TEST, 0, 0);
    else if (!strcmp(v, "menuread")) rig_post(OP_MENU_READ, 0, 0);
    else if (!strcmp(v, "menusweep")) rig_post(OP_MENU_SWEEP, 0, 0);
    else if (!strcmp(v, "memread")) rig_post(OP_MEM_READ, 0, 0);
    else if (!strcmp(v, "memrecall") && k >= 2) rig_post(OP_MEM_RECALL, a, 0);
    else if (!strcmp(v, "modelock") && k >= 2) rig_set_mode_lock((int)a);
    else if (!strcmp(v, "txmax") && k >= 2) rig_set_tx_max((int)a);
}

static void ws_loop(int fd, int slot) {
    static __thread char out[7500];
    uint8_t in[1024];
    size_t have = 0;
    unsigned sent_seq = 0, sent_log = 0, wf_seq = 0, rx_cur = 0;
    int wf = 0, rx = 0, sid = 0, sln = 0, sview = 0;
    uint8_t wbuf[WF_BINS], rbuf[RX_CHUNK], sbuf[2420];
    int first = 1;
    long long next = 0;
    rig_client_count(1);
    while (!g_stop) {
        long long t = now_ms();
        if (t >= next) {
            next = t + 200;
            unsigned q = rig_seq();
            if (first || q != sent_seq) {
                sent_seq = q;
                int n = state_json(out, sizeof out);
                if (ws_send(fd, out, (size_t)n)) break;
            }
            unsigned l = log_seq();
            if (first || l != sent_log) {
                int n = log_copy_json(out, sizeof out, &sent_log);
                if (ws_send(fd, out, (size_t)n)) break;
            }
            first = 0;
        }
        if (wf) {
            unsigned ws = audio_frame(wbuf, wf_seq);
            if (ws != wf_seq) { wf_seq = ws; if (ws_send_bin(fd, 1, wbuf, WF_BINS)) break; }
        }
        if (sview) {
            int fail = 0;
            for (int i = 0; i < 16; i++) {
                int n = sstv_line(&sid, &sln, sbuf, sizeof sbuf);
                if (!n) break;
                if (ws_send_bin(fd, 3, sbuf, (size_t)n)) { fail = 1; break; }
            }
            if (fail) break;
        }
        if (rx) {
            /* Adás alatt nincs vételi hang, így nem gerjed vissza a mikrofonba */
            int fail = 0;
            while (audio_rx(rbuf, &rx_cur))
                if (!rig_keyed() && ws_send_bin(fd, 2, rbuf, RX_CHUNK)) { fail = 1; break; }
            if (fail) break;
        }
        struct pollfd p = {fd, POLLIN, 0};
        int r = io_ready() ? 1 : sock_poll(&p, 1, wf || rx ? 40 : 100);
        if (r < 0 && errno != EINTR) break;
        if (r <= 0) continue;
        ssize_t got = io_recv(fd, in + have, sizeof in - have);
        if (got <= 0) break;
        have += (size_t)got;
        for (;;) {
            if (have < 2) break;
            int op = in[0] & 15, masked = in[1] >> 7;
            size_t len = in[1] & 127, hl = 2;
            if (len == 126) { if (have < 4) break; len = (size_t)in[2] << 8 | in[3]; hl = 4; }
            else if (len == 127) goto done;
            if (!masked || len > 512) goto done;
            if (have < hl + 4 + len) break;
            uint8_t *mask = in + hl, *d = mask + 4;
            for (size_t i = 0; i < len; i++) d[i] ^= mask[i % 4];
            if (op == 8) goto done;
            if (op == 1) {
                char line[520];
                memcpy(line, d, len);
                line[len] = 0;
                if (!strncmp(line, "sview ", 6)) { sview = line[6] == '1'; sln = 0; sid = -1; }
                else ws_cmd(line, slot, &wf, &rx);
            } else if (op == 2) {
                if (rig_keyed()) audio_tx_write(slot, d, len);
            } else if (op == 9) {
                uint8_t pong[2] = {0x8A, 0};
                send_all(fd, pong, 2);
            }
            size_t used = hl + 4 + len;
            have -= used;
            memmove(in, in + used, have);
        }
    }
done:
    rig_hold(SRC_WEB(slot), 0, 0);     /* megszakadt kapcsolat: nincs tovább adás */
    rig_client_count(-1);
    if (wf) audio_want(-1);
    if (rx) audio_want(-1);
    audio_tx_end(slot);
}

static void ws_accept(conn_t *c, const req_t *r) {
    char src[128], acc[64], h[300];
    uint8_t dig[20];
    int slot = -1;
    pthread_mutex_lock(&A);
    for (int i = 0; i < MAX_WEB; i++) if (!web_used[i]) { web_used[i] = 1; slot = i; break; }
    pthread_mutex_unlock(&A);
    if (slot < 0) { reply_txt(c->fd, "503 Busy", TR("Túl sok megnyitott felület", "Too many open interfaces")); return; }
    int n = snprintf(src, sizeof src, "%s258EAFA5-E914-47DA-95CA-C5AB0DC85B11", r->ws_key);
    sha1((uint8_t *)src, (size_t)n, dig);
    base64(dig, 20, acc);
    int l = snprintf(h, sizeof h, "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n"
                                  "Connection: Upgrade\r\nSec-WebSocket-Accept: %s\r\n\r\n", acc);
    if (!send_all(c->fd, h, (size_t)l)) ws_loop(c->fd, slot);
    pthread_mutex_lock(&A);
    web_used[slot] = 0;
    pthread_mutex_unlock(&A);
}

/* --- adat-API --- */
static void url_arg(const char *url, const char *key, char *out, size_t cap) {
    const char *p = strchr(url, '?');
    size_t n = 0, kl = strlen(key);
    out[0] = 0;
    for (p = p ? p + 1 : NULL; p && *p; ) {
        if (!strncmp(p, key, kl) && p[kl] == '=') {
            for (p += kl + 1; *p && *p != '&' && n + 1 < cap; p++) {
                unsigned h;
                if (*p == '%' && sscanf(p + 1, "%2x", &h) == 1) { out[n++] = (char)h; p += 2; }
                else out[n++] = *p == '+' ? ' ' : *p;
            }
            out[n] = 0;
            return;
        }
        p = strchr(p, '&');
        if (p) p++;
    }
}

static void reply_ok(int fd, int ok, const char *msg) {
    sb_t b = {0};
    sb_add(&b, "{\"ok\":%s,\"msg\":\"%s\"}", ok ? "true" : "false", msg);
    reply(fd, ok ? "200 OK" : "409 Conflict", "application/json", NULL, b.p, b.n);
    free(b.p);
}

static void reply_sb(int fd, sb_t *b) {
    reply(fd, "200 OK", "application/json", NULL, b->p, b->n);
    free(b->p);
}

/* 1, ha ez az útvonal adat-API volt */
static int data_api(conn_t *c, req_t *r, const char *path) {
    char name[64], err[100] = "";
    int post = !strcmp(r->method, "POST");
    if (strncmp(path, "/api/", 5)) return 0;
    const char *a = path + 5;
    int known = !strcmp(a, "menus") || !strcmp(a, "profiles") || !strcmp(a, "backups") || !strcmp(a, "mem") ||
                !strcmp(a, "write") || !strcmp(a, "apply") || !strcmp(a, "profile") || !strcmp(a, "profile-del") ||
                !strcmp(a, "mem-write") || !strcmp(a, "audio") || !strcmp(a, "rig") || !strcmp(a, "sstv") || !strcmp(a, "sstv-img");
    if (!known) return 0;
    if (!authed(c, r)) { reply_txt(c->fd, "401 Unauthorized", TR("PIN szükséges", "PIN required")); return 1; }
    if (!origin_ok(r)) { reply_txt(c->fd, "403 Forbidden", "Idegen eredet"); return 1; }
    url_arg(r->path, "name", name, sizeof name);
    if (!post) {
        sb_t b = {0};
        if (!strcmp(a, "menus")) menu_json(&b);
        else if (!strcmp(a, "profiles")) sets_json(&b, 0);
        else if (!strcmp(a, "backups")) sets_json(&b, 1);
        else if (!strcmp(a, "mem")) mem_json(&b);
        else if (!strcmp(a, "audio")) audio_list(&b);
        else if (!strcmp(a, "sstv")) sstv_list(&b);
        else if (!strcmp(a, "sstv-img")) {
            size_t len = 0;
            char *d = sstv_read(name, &len);
            if (!d) reply_txt(c->fd, "404 Not Found", TR("Nincs ilyen kép", "No such picture"));
            else {
                reply(c->fd, "200 OK", "image/png", NULL, d, len);
                free(d);
            }
            return 1;
        }
        else if (!strcmp(a, "rig")) {
            rig_state_t s;
            rig_get(&s);
            sb_add(&b, "{\"connected\":%d,\"freq\":%lld,\"freq_b\":%lld,\"mode\":\"%s\",\"adif_mode\":\"%s\",\"tx\":%d,\"split\":%d}",
                   s.connected, s.freq_a, s.freq_b, rig_hamlib_mode(&s), rig_adif_mode(&s), s.keyed || s.tx, s.split);
        }
        else { reply_txt(c->fd, "405 Method Not Allowed", "POST kell"); return 1; }
        reply_sb(c->fd, &b);
        return 1;
    }
    if (!strcmp(a, "write")) {
        int q = menu_queue(r->body, TR("Menüírás", "Menu write"));
        reply_ok(c->fd, !q, q ? TR("Már fut egy művelet.", "Another operation is running.") : "");
    } else if (!strcmp(a, "apply")) {
        char type[16];
        url_arg(r->path, "type", type, sizeof type);
        int q = menu_queue_set(!strcmp(type, "backup"), name);
        reply_ok(c->fd, !q, q == -2 ? TR("Nincs ilyen profil.", "No such profile.") : q ? TR("Már fut egy művelet.", "Another operation is running.") : "");
    } else if (!strcmp(a, "profile")) {
        int q = profile_save(name, r->body);
        reply_ok(c->fd, !q, q ? TR("Ezzel a névvel nem menthető.", "Cannot save with this name.") : "");
    } else if (!strcmp(a, "profile-del")) {
        int q = profile_delete(name);
        reply_ok(c->fd, !q, q ? TR("Nincs ilyen profil.", "No such profile.") : "");
    } else if (!strcmp(a, "mem-write")) {
        int q = mem_queue(r->body, err, sizeof err);
        reply_ok(c->fd, !q, err);
    } else reply_txt(c->fd, "405 Method Not Allowed", "GET kell");
    return 1;
}

/* --- HTTP --- */
static void hdr_copy(char *dst, size_t cap, const char *line) {
    const char *v = strchr(line, ':');
    if (!v) return;
    v++;
    while (*v == ' ') v++;
    snprintf(dst, cap, "%s", v);
    dst[strcspn(dst, "\r\n")] = 0;
}

static int read_req(int fd, req_t *r) {
    char b[4096];
    size_t n = 0;
    char *end = NULL;
    long long dl = now_ms() + 5000;
    while (!end) {
        if (n >= sizeof b - 1) return -1;
        struct pollfd p = {fd, POLLIN, 0};
        int left = (int)(dl - now_ms());
        if (left <= 0 || (!io_ready() && sock_poll(&p, 1, left) <= 0)) return -1;
        ssize_t got = io_recv(fd, b + n, sizeof b - 1 - n);
        if (got <= 0) return -1;
        n += (size_t)got;
        b[n] = 0;
        end = strstr(b, "\r\n\r\n");
    }
    *end = 0;
    char *body = end + 4;
    size_t have = n - (size_t)(body - b);
    char *save = NULL, *line = strtok_r(b, "\n", &save);
    if (!line || sscanf(line, "%7s %199s", r->method, r->path) != 2) return -1;
    while ((line = strtok_r(NULL, "\n", &save))) {
        if (!strncasecmp(line, "Host:", 5)) hdr_copy(r->host, sizeof r->host, line);
        else if (!strncasecmp(line, "Origin:", 7)) hdr_copy(r->origin, sizeof r->origin, line);
        else if (!strncasecmp(line, "Cookie:", 7)) hdr_copy(r->cookie, sizeof r->cookie, line);
        else if (!strncasecmp(line, "Sec-WebSocket-Key:", 18)) hdr_copy(r->ws_key, sizeof r->ws_key, line);
        else if (!strncasecmp(line, "Upgrade:", 8)) hdr_copy(r->upgrade, sizeof r->upgrade, line);
        else if (!strncasecmp(line, "Content-Length:", 15)) r->content_len = atoi(line + 15);
    }
    if (r->content_len < 0 || r->content_len >= (int)sizeof r->body) r->content_len = 0;
    size_t want = (size_t)r->content_len;
    size_t c = have < want ? have : want;
    memcpy(r->body, body, c);
    while (c < want) {
        struct pollfd p = {fd, POLLIN, 0};
        if (!io_ready() && sock_poll(&p, 1, 2000) <= 0) break;
        ssize_t got = io_recv(fd, r->body + c, want - c);
        if (got <= 0) break;
        c += (size_t)got;
    }
    r->body[c] = 0;
    return 0;
}

static void *conn_main(void *p) {
    conn_t c = *(conn_t *)p;
    free(p);
#ifdef HAVE_SSL
    if (c.tls) {
        sock_rcvtimeo(c.fd, 5000);
        cur_ssl = SSL_new(tls_ctx);
        if (!cur_ssl || !SSL_set_fd(cur_ssl, c.fd) || SSL_accept(cur_ssl) <= 0) {
            if (cur_ssl) SSL_free(cur_ssl);
            cur_ssl = NULL;
            sock_close(c.fd);
            return NULL;
        }
    }
#endif
    req_t *r = calloc(1, sizeof *r);
    if (r && !read_req(c.fd, r)) {
        char path[200];
        snprintf(path, sizeof path, "%s", r->path);
        path[strcspn(path, "?")] = 0;
        if (!strcmp(path, "/") && !strcmp(r->method, "GET")) {
            /* a lap nyelvét a <html lang="hu"> attribútum adja meg */
            size_t n = (size_t)(page_end - page_start);
            char *pg = malloc(n);
            if (pg) {
                memcpy(pg, page_start, n);
                char *l = memmem(pg, n < 200 ? n : 200, "lang=\"hu\"", 9);
                if (l && g_cfg.lang_en) memcpy(l + 6, "en", 2);
                reply(c.fd, "200 OK", "text/html; charset=utf-8", NULL, pg, n);
                free(pg);
            } else
                reply_txt(c.fd, "500 Internal Server Error", "out of memory");
        }
        else if (!strcmp(path, "/api/whoami")) {
            char b[64];
            int l = snprintf(b, sizeof b, "{\"auth\":%s,\"lang\":\"%s\"}", authed(&c, r) ? "true" : "false", g_cfg.lang_en ? "en" : "hu");
            reply(c.fd, "200 OK", "application/json", NULL, b, (size_t)l);
        }
        else if (!strcmp(path, "/api/login") && !strcmp(r->method, "POST")) {
            if (!origin_ok(r)) reply_txt(c.fd, "403 Forbidden", "Idegen eredet");
            else do_login(&c, r);
        }
        else if (!strcmp(path, "/api/state")) {
            if (!authed(&c, r)) reply_txt(c.fd, "401 Unauthorized", TR("PIN szükséges", "PIN required"));
            else {
                static __thread char o[7500];
                int l = state_json(o, sizeof o);
                reply(c.fd, "200 OK", "application/json", NULL, o, (size_t)l);
            }
        }
        else if (data_api(&c, r, path)) { /* kész */ }
        else if (!strcmp(path, "/ws")) {
            if (strcasecmp(r->upgrade, "websocket") || !r->ws_key[0]) reply_txt(c.fd, "400 Bad Request", "WebSocket kell");
            else if (!origin_ok(r)) reply_txt(c.fd, "403 Forbidden", "Idegen eredet");
            else if (!authed(&c, r)) reply_txt(c.fd, "401 Unauthorized", TR("PIN szükséges", "PIN required"));
            else ws_accept(&c, r);
        }
        else reply_txt(c.fd, "404 Not Found", TR("Nincs ilyen oldal", "No such page"));
    }
    free(r);
#ifdef HAVE_SSL
    if (cur_ssl) { SSL_shutdown(cur_ssl); SSL_free(cur_ssl); cur_ssl = NULL; }
#endif
    sock_close(c.fd);
    return NULL;
}

typedef struct { int ls, tls; } lst_t;

static void *accept_main(void *p) {
    lst_t l = *(lst_t *)p;
    int ls = l.ls;
    free(p);
    while (!g_stop) {
        conn_t *c = calloc(1, sizeof *c);
        socklen_t sl = sizeof c->peer;
        c->fd = sock_accept(ls, (struct sockaddr *)&c->peer, &sl);
        c->tls = l.tls;
        if (c->fd < 0) {
            free(c);
            if (errno == EINTR) continue;
            break;
        }
        sock_nodelay(c->fd);
        pthread_t t;
        if (pthread_create(&t, NULL, conn_main, c)) { sock_close(c->fd); free(c); continue; }
        pthread_detach(t);
    }
    return NULL;
}

static int open_listener(int port, int tls) {
    int ls = (int)socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    struct sockaddr_in sa = {0};
    sa.sin_family = AF_INET;
    sa.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, g_cfg.web_bind, &sa.sin_addr) != 1) sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sock_reuse(ls);
    if (bind(ls, (struct sockaddr *)&sa, sizeof sa) || listen(ls, 16)) {
        log_msg(TR("A %d-es port nem nyitható meg: %s", "Cannot open port %d: %s"), port, strerror(errno));
        sock_close(ls);
        return -1;
    }
    lst_t *a = malloc(sizeof *a);
    a->ls = ls;
    a->tls = tls;
    pthread_t t;
    pthread_create(&t, NULL, accept_main, a);
    pthread_detach(t);
    return 0;
}

#ifdef HAVE_SSL
/* Saját aláírású tanúsítvány a gép címeire; a böngésző egyszer rákérdez */
static int make_cert(const char *crt, const char *key) {
    char san[900] = "subjectAltName=DNS:localhost,IP:127.0.0.1";
    char ip[16][64];
    int nip = sys_local_ipv4(ip, 16);
    for (int i = 0; i < nip; i++)
        if (strlen(san) + 32 < sizeof san) { strcat(san, ",IP:"); strcat(san, ip[i]); }
    pid_t p = fork();
    if (p == 0) {
        int n = open("/dev/null", 1);
        dup2(n, 1); dup2(n, 2);
        execlp("openssl", "openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "3650",
               "-keyout", key, "-out", crt, "-subj", "/CN=FT-891 vezerlo", "-addext", san, (char *)NULL);
        _exit(127);
    }
    int st = 1;
    if (p > 0) waitpid(p, &st, 0);
    chmod(key, 0600);
    return WIFEXITED(st) && WEXITSTATUS(st) == 0 ? 0 : -1;
}

static void tls_start(void) {
    char crt[320], key[320];
    snprintf(crt, sizeof crt, "%s/tls-cert.pem", g_cfg.cfg_dir);
    snprintf(key, sizeof key, "%s/tls-key.pem", g_cfg.cfg_dir);
    if (access(crt, R_OK) || access(key, R_OK)) {
        if (make_cert(crt, key)) { log_msg(TR("A HTTPS-tanúsítvány nem készült el, a mikrofon csak ezen a gépen működik", "HTTPS certificate could not be made, the microphone works on this computer only")); g_cfg.tls_port = 0; return; }
    }
    tls_ctx = SSL_CTX_new(TLS_server_method());
    if (!tls_ctx) { g_cfg.tls_port = 0; return; }
    SSL_CTX_set_min_proto_version(tls_ctx, TLS1_2_VERSION);
    SSL_CTX_set_mode(tls_ctx, SSL_MODE_AUTO_RETRY);
    if (SSL_CTX_use_certificate_file(tls_ctx, crt, SSL_FILETYPE_PEM) != 1 ||
        SSL_CTX_use_PrivateKey_file(tls_ctx, key, SSL_FILETYPE_PEM) != 1) {
        log_msg(TR("A HTTPS-tanúsítvány hibás. Töröld a tls-*.pem fájlokat, és indítsd újra.", "The HTTPS certificate is bad. Delete the tls-*.pem files and restart."));
        g_cfg.tls_port = 0;
        return;
    }
    if (open_listener(g_cfg.tls_port, 1)) g_cfg.tls_port = 0;
}
#endif

void web_start(void) {
    if (g_cfg.tls_port < 0) g_cfg.tls_port = g_cfg.web_port + 1;
#ifdef HAVE_SSL
    if (g_cfg.tls_port) tls_start();
#elif defined(_WIN32)
    g_cfg.tls_port = 0;          /* a Windows-változatban nincs HTTPS */
#else
    if (g_cfg.tls_port) log_msg(TR("A program HTTPS nélkül készült, a telefon mikrofonja nem elérhető. Telepítsd a libssl-dev csomagot, és fordítsd újra.", "The program was built without HTTPS, the phone microphone is not available. Install libssl-dev and rebuild."));
    g_cfg.tls_port = 0;
#endif
    /* A sima port utoljára nyílik: ekkorra a beállítás már végleges */
    if (open_listener(g_cfg.web_port, 0)) exit(1);
}

void web_print_urls(void) {
    con_printf(TR("\nFT891 Next " APP_VERSION "\n  Ezen a gépen:  http://127.0.0.1:%d\n",
              "\nFT891 Next " APP_VERSION "\n  This computer:  http://127.0.0.1:%d\n"), g_cfg.web_port);
    if (strcmp(g_cfg.web_bind, "127.0.0.1")) {
        char ip[16][64];
        int nip = sys_local_ipv4(ip, 16);
        for (int i = 0; i < nip; i++) {
            con_printf(TR("  Telefonról:    http://%s:%d\n", "  From a phone:   http://%s:%d\n"), ip[i], g_cfg.web_port);
            if (g_cfg.tls_port) con_printf(TR("  Mikrofonnal:   https://%s:%d\n", "  With mic:       https://%s:%d\n"), ip[i], g_cfg.tls_port);
        }
        con_printf(TR("  PIN-kód:       %s\n", "  PIN:            %s\n"), g_cfg.pin);
    }
    con_printf("\n");
    fflush(stdout);
}
