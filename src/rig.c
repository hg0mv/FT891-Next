/* FT-891 CAT-mag: egyetlen szál kezeli a soros portot, minden kérés sorba áll */
#include "ft891ctl.h"
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

#define POST_WRITE_MS 50     /* a Hamlib FT-891-es értéke */
#define REPLY_MS      400
#define MAX_TIMEOUTS  3

typedef struct { op_t op; long long a, b; int *done, *rc; } job_t;

static pthread_mutex_t M = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  C_done = PTHREAD_COND_INITIALIZER;
static rig_state_t S;
static job_t Q[64];
static int   qn;
static int   wake_p[2] = {-1, -1};
static pthread_t cat_th;
static int   clients;

static int  fd = -1, ptt_fd = -1;
static char rxbuf[1024];
static int  rxn;
static char last_reply[96];
static int  nto;              /* egymás utáni időtúllépések */

/* PTT */
static unsigned hold_mask;
static long long lease[32];
static long long key_t0, verify_t;
static int guard, rekeys, test_active, test_seen, test_drop;

#define LOCK()   pthread_mutex_lock(&M)
#define UNLOCK() pthread_mutex_unlock(&M)
#define SET(f, v) do { LOCK(); if (S.f != (v)) { S.f = (v); S.seq++; } UNLOCK(); } while (0)

static void status(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void status(const char *fmt, ...) {
    char b[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    utf8_trim(b);
    LOCK();
    if (strcmp(b, S.status)) {
        snprintf(S.status, sizeof S.status, "%s", b);
        S.seq++;
        UNLOCK();
        log_msg("%s", b);
    } else UNLOCK();
}

void rig_notice(int bad, const char *fmt, ...) {
    char b[200];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    utf8_trim(b);
    LOCK();
    snprintf(S.notice, sizeof S.notice, "%s", b);
    S.notice_bad = bad;
    S.notice_seq++;
    S.seq++;
    UNLOCK();
    log_msg("%s", b);
}

/* --- módok és szélességek --- */
int rig_ssb_eff(int bfo, long long freq) {
    if (bfo == 0) return 0;
    if (bfo == 1) return 1;
    return freq < 10000000LL;   /* AUTO: 10 MHz alatt LSB */
}

static int family(char md) {   /* 1 SSB, 2 CW, 3 RTTY, 4 DATA, 5 AM, 6 FM */
    switch (md) {
    case '1': case '2': return 1;
    case '3': case '7': return 2;
    case '6': case '9': return 3;
    case '8': case 'C': return 4;
    case '5': case 'D': return 5;
    case '4': case 'B': return 6;
    }
    return 0;
}

/* Az MD kód önmagában nem mondja meg az oldalsávot: a BFO-menü dönt */
int rig_lower_side(const rig_state_t *s) {
    int bfo;
    switch (family(s->md)) {
    case 1: bfo = s->bfo_ssb;  if (bfo < 0) return s->md == '1'; break;
    case 2: bfo = s->bfo_cw;   if (bfo < 0) return s->md == '7'; break;
    case 3: bfo = s->bfo_rtty; if (bfo < 0) return s->md == '6'; break;
    case 4: bfo = s->bfo_data; if (bfo < 0) return s->md == '8'; break;
    default: return 0;
    }
    return rig_ssb_eff(bfo, s->freq_a);
}

const char *rig_mode_name(const rig_state_t *s) {
    int l = rig_lower_side(s);
    switch (s->md) {
    case '1': case '2': return l ? "LSB" : "USB";
    case '3': case '7': return l ? "CW-L" : "CW-U";
    case '6': case '9': return l ? "RTTY-L" : "RTTY-U";
    case '8': case 'C': return l ? "DATA-L" : "DATA-U";
    case '4': return "FM";
    case 'B': return "FM-N";
    case '5': return "AM";
    case 'D': return "AM-N";
    }
    return "?";
}

const char *rig_hamlib_mode(const rig_state_t *s) {
    int l = rig_lower_side(s);
    switch (s->md) {
    case '1': case '2': return l ? "LSB" : "USB";
    case '3': case '7': return l ? "CWR" : "CW";
    case '6': case '9': return l ? "RTTY" : "RTTYR";
    case '8': case 'C': return l ? "PKTLSB" : "PKTUSB";
    case '4': return "FM";
    case 'B': return "FMN";
    case '5': return "AM";
    case 'D': return "AMN";
    }
    return "USB";
}

static const short W_SSB[] = {0, 200, 400, 600, 850, 1100, 1350, 1500, 1650, 1800, 1950, 2100,
                              2200, 2300, 2400, 2500, 2600, 2700, 2800, 2900, 3000, 3200};
static const short W_CW[]  = {0, 50, 100, 150, 200, 250, 300, 350, 400, 450, 500, 800, 1200,
                              1400, 1700, 2000, 2400, 3000};

const char *rig_adif_mode(const rig_state_t *s) {
    switch (family(s->md)) {
    case 1: return "SSB";
    case 2: return "CW";
    case 3: return "RTTY";
    case 4: return "DATA";
    case 5: return "AM";
    case 6: return "FM";
    }
    return "";
}

int rig_width_hz(const rig_state_t *s) {
    int f = family(s->md), i = s->width_idx;
    if (f == 1) {
        if (i > 0 && i <= 21) return W_SSB[i];
        return s->narrow ? 1500 : 2400;
    }
    if (f == 2 || f == 3 || f == 4) {
        if (i > 0 && i <= 17) return W_CW[i];
        if (f == 2) return s->narrow ? 500 : 2400;
        return s->narrow ? 300 : 500;
    }
    return f == 6 ? 12000 : 6000;
}

/* --- soros port --- */
static speed_t baud_const(int b) {
    switch (b) {
    case 4800: return B4800;
    case 9600: return B9600;
    case 19200: return B19200;
    default: return B38400;
    }
}

static int open_port(const char *path, int baud, int rts) {
    int f = open(path, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (f < 0) return -errno;
    /* Nyitáskor a Linux felemelheti az RTS/DTR vonalat: azonnal levesszük */
    int bits = TIOCM_RTS | TIOCM_DTR;
    ioctl(f, TIOCMBIC, &bits);
    if (ioctl(f, TIOCEXCL) < 0 && errno == EBUSY) { close(f); return -EBUSY; }
    struct termios t;
    if (tcgetattr(f, &t) == 0) {
        cfmakeraw(&t);
        t.c_cflag &= ~(unsigned)(CSIZE | PARENB | CRTSCTS | HUPCL);
        t.c_cflag |= CS8 | CSTOPB | CLOCAL | CREAD;   /* 8N2 */
        cfsetispeed(&t, baud_const(baud));
        cfsetospeed(&t, baud_const(baud));
        t.c_cc[VMIN] = 0;
        t.c_cc[VTIME] = 0;
        tcsetattr(f, TCSANOW, &t);
    }
    if (rts) { bits = TIOCM_RTS; ioctl(f, TIOCMBIS, &bits); }
    return f;
}

static void hw_ptt(int on) {
    if (ptt_fd < 0) return;
    int bits = g_cfg.ptt_method == 2 ? TIOCM_DTR : TIOCM_RTS;
    if (ioctl(ptt_fd, on ? TIOCMBIS : TIOCMBIC, &bits) < 0)
        log_msg("A PTT-vonal nem állítható: %s", strerror(errno));
}

static void ptt_port_close(void) {
    if (ptt_fd >= 0) {
        int bits = TIOCM_RTS | TIOCM_DTR;
        ioctl(ptt_fd, TIOCMBIC, &bits);
        close(ptt_fd);
        ptt_fd = -1;
    }
    LOCK(); S.ptt_port[0] = 0; S.seq++; UNLOCK();
}

static int find_ptt_path(char *out, size_t cap) {
    if (g_cfg.ptt_port[0]) { snprintf(out, cap, "%s", g_cfg.ptt_port); return 0; }
    const char *p = strstr(S.port, "-if00");
    if (p) {                      /* by-id: a Standard port az if01 */
        snprintf(out, cap, "%.*s-if01%s", (int)(p - S.port), S.port, p + 5);
        return 0;
    }
    const char *d = S.port + strlen(S.port);
    while (d > S.port && isdigit((unsigned char)d[-1])) d--;
    if (*d) {
        snprintf(out, cap, "%.*s%d", (int)(d - S.port), S.port, atoi(d) + 1);
        return 0;
    }
    return -1;
}

static int ptt_port_open(void) {
    char path[256];
    ptt_port_close();
    if (g_cfg.ptt_method == 0) return 0;
    if (find_ptt_path(path, sizeof path)) return -1;
    int f = open_port(path, 9600, 0);
    if (f < 0) {
        rig_notice(1, "A PTT-port nem nyitható meg (%s): %s", path, strerror(-f));
        return -1;
    }
    ptt_fd = f;
    LOCK(); snprintf(S.ptt_port, sizeof S.ptt_port, "%s", path); S.seq++; UNLOCK();
    log_msg("PTT-port: %s", path);
    return 0;
}

static void clear_on_disconnect(void) {
    LOCK();
    hold_mask = 0;
    S.connected = 0;
    S.keyed = 0;
    S.tx = 0;
    S.smeter = S.po = S.swr = S.alc = 0;
    S.seq++;
    UNLOCK();
    guard = 0;
    test_active = 0;
}

static int power_off_req;

static void drop_link(const char *why) {
    if (fd >= 0) {
        close(fd);
        fd = -1;
    }
    hw_ptt(0);
    ptt_port_close();
    rxn = 0;
    clear_on_disconnect();
    if (power_off_req) { power_off_req = 0; status("A rádió kikapcsolva."); log_msg("A rádió kikapcsolása után várakozás"); }
    else status("Megszakadt a kapcsolat: %s", why);
}

/* --- keretkezelés --- */
static void handle_frame(const char *f);

static int cat_write(const char *cmd) {
    if (fd < 0) return -1;
    size_t n = strlen(cmd), o = 0;
    log_trace(">", cmd);
    long long end = now_ms() + 300;
    while (o < n) {
        ssize_t w = write(fd, cmd + o, n - o);
        if (w > 0) { o += (size_t)w; continue; }
        if (w < 0 && errno != EAGAIN && errno != EINTR) { drop_link(strerror(errno)); return -1; }
        struct pollfd p = {fd, POLLOUT, 0};
        poll(&p, 1, 20);
        if (now_ms() > end) { drop_link("az írás nem fejeződött be"); return -1; }
    }
    return 0;
}

/* Beolvassa a következő, pontosvesszővel zárt keretet. 1 kész, 0 idő, -1 hiba */
static int read_frame(char *out, size_t cap, int timeout_ms, int use_wake) {
    long long end = now_ms() + timeout_ms;
    for (;;) {
        char *e = memchr(rxbuf, ';', (size_t)rxn);
        if (e) {
            size_t len = (size_t)(e - rxbuf) + 1;
            size_t c = len < cap ? len : cap - 1;
            memcpy(out, rxbuf, c);
            out[c] = 0;
            rxn -= (int)len;
            memmove(rxbuf, rxbuf + len, (size_t)rxn);
            log_trace("<", out);
            return 1;
        }
        if (fd < 0) return -1;
        long long left = end - now_ms();
        if (left <= 0) return 0;
        struct pollfd p[2] = {{fd, POLLIN, 0}, {wake_p[0], POLLIN, 0}};
        int r = poll(p, use_wake ? 2 : 1, (int)left);
        if (r < 0) { if (errno == EINTR) continue; return -1; }
        if (use_wake && (p[1].revents & POLLIN)) return 0;
        if (p[0].revents & (POLLERR | POLLNVAL)) { drop_link("a port eltűnt"); return -1; }
        if (p[0].revents & (POLLIN | POLLHUP)) {
            if (rxn >= (int)sizeof rxbuf - 1) rxn = 0;    /* szemét, eldobjuk */
            ssize_t n = read(fd, rxbuf + rxn, sizeof rxbuf - 1 - (size_t)rxn);
            if (n > 0) rxn += (int)n;
            else if (n == 0 || (errno != EAGAIN && errno != EINTR)) {
                drop_link("a port eltűnt");
                return -1;
            }
        }
    }
}

/* Rövid várakozás, közben a rádió önálló jelzéseit feldolgozza */
static void cat_pause(int ms) {
    char f[96];
    long long end = now_ms() + ms;
    while (fd >= 0) {
        long long left = end - now_ms();
        if (left <= 0) break;
        if (read_frame(f, sizeof f, (int)left, 0) == 1) handle_frame(f);
    }
}

static int cat_set(const char *cmd) {
    if (cat_write(cmd)) return -1;
    cat_pause(POST_WRITE_MS);
    return fd >= 0 ? 0 : -1;
}

/* 0 kész, 1 a rádió elutasította (?;), -1 nincs válasz */
static int query(const char *cmd) {
    size_t pl = strlen(cmd) - 1;
    char f[96];
    if (cat_write(cmd)) return -1;
    long long end = now_ms() + REPLY_MS;
    while (fd >= 0) {
        long long left = end - now_ms();
        if (left <= 0) break;
        int r = read_frame(f, sizeof f, (int)left, 0);
        if (r < 0) return -1;
        if (r == 0) break;
        nto = 0;
        LOCK(); S.poll_cnt++; UNLOCK();
        if (!strcmp(f, "?;")) return 1;
        handle_frame(f);
        if (!strncmp(f, cmd, pl)) {
            snprintf(last_reply, sizeof last_reply, "%s", f);
            return 0;
        }
    }
    if (fd < 0) return -1;
    LOCK(); S.timeouts++; S.seq++; UNLOCK();
    if (++nto >= MAX_TIMEOUTS) drop_link("a rádió nem válaszol (kikapcsolták?)");
    return -1;
}

static void key_off(void);

static void on_tx_reply(int tx) {
    SET(tx, tx);
    if (test_active && tx == 1) test_seen = 1;
    long long t = now_ms();
    LOCK();
    int keyed = S.keyed;
    UNLOCK();
    if (!keyed || g_cfg.ptt_method != 0 || tx != 0 || t - verify_t < 300) return;
    /* CAT PTT mellett a rádió kiesett az adásból */
    LOCK(); S.ptt_drops++; S.seq++; UNLOCK();
    if (test_active) test_drop = 1;
    if (rekeys < 2) {
        rekeys++;
        verify_t = t;
        log_msg("A rádió elengedte a CAT PTT-t, újra adásba teszem (%d. próba)", rekeys);
        cat_write("TX1;");
    } else {
        rig_notice(1, "A rádió többször elengedte a CAT PTT-t. Próbáld ki az RTS módot a Beállításban.");
        guard = 1;
    }
}

static void handle_frame(const char *f) {
    size_t n = strlen(f);
    if (n < 3) return;
    const char *p = f + 2;
#define IS(s) (f[0] == s[0] && f[1] == s[1])
#define LVL(i, v) do { LOCK(); if (S.lvl[i] != (v)) { S.lvl[i] = (v); S.seq++; } UNLOCK(); } while (0)
#define FT(i, v)  do { LOCK(); if (S.ft[i] != (v))  { S.ft[i]  = (v); S.seq++; } UNLOCK(); } while (0)
#define FN(i, v)  do { LOCK(); if (S.fn[i] != (v))  { S.fn[i]  = (v); S.seq++; } UNLOCK(); } while (0)
    if (IS("FA") && n >= 12) { long long v = atoll(p); SET(freq_a, v); }
    else if (IS("FB") && n >= 12) { long long v = atoll(p); SET(freq_b, v); }
    else if (IS("IF") && n >= 28) {
        char fq[10], ofs[6];
        memcpy(fq, f + 5, 9); fq[9] = 0;
        memcpy(ofs, f + 14, 5); ofs[5] = 0;
        long long v = atoll(fq);
        int o = atoi(ofs), c = f[19] == '1', m = f[22] - '0';
        char md = f[21];
        LOCK();
        if (S.freq_a != v || S.clar_ofs != o || S.clar != c || S.mem_mode != m || S.md != md ||
            memcmp(S.channel, f + 2, 3)) S.seq++;
        S.freq_a = v; S.clar_ofs = o; S.clar = c; S.mem_mode = m; S.md = md;
        memcpy(S.channel, f + 2, 3); S.channel[3] = 0;
        UNLOCK();
    }
    else if (IS("MD") && n >= 5) { char md = (char)toupper((unsigned char)f[3]); SET(md, md); }
    else if (IS("SM") && n >= 7) { int v = atoi(f + 3); SET(smeter, v); }
    else if (IS("RM") && n >= 7) {
        int v = atoi(f + 3);
        if (f[2] == '5') SET(po, v);
        else if (f[2] == '6') SET(swr, v);
        else if (f[2] == '4') SET(alc, v);
    }
    else if (IS("TX") && n >= 4) on_tx_reply(f[2] - '0');
    else if (IS("AG") && n >= 7) LVL(L_AF, atoi(f + 3));
    else if (IS("RG") && n >= 7) LVL(L_RF, atoi(f + 3));
    else if (IS("SQ") && n >= 7) LVL(L_SQL, atoi(f + 3));
    else if (IS("PC") && n >= 6) LVL(L_PWR, atoi(p));
    else if (IS("MG") && n >= 6) LVL(L_MIC, atoi(p));
    else if (IS("PL") && n >= 6) LVL(L_PROC, atoi(p));
    else if (IS("RL") && n >= 6) LVL(L_NRL, atoi(f + 3));
    else if (IS("NL") && n >= 7) LVL(L_NBL, atoi(f + 3));
    else if (IS("NR") && n >= 5) FN(FN_NR, f[3] == '1');
    else if (IS("NB") && n >= 5) FN(FN_NB, f[3] == '1');
    else if (IS("BC") && n >= 5) FN(FN_NOTCH, f[3] == '1');
    else if (IS("PA") && n >= 5) FN(FN_AMP, f[3] - '0');
    else if (IS("RA") && n >= 5) FN(FN_ATT, f[3] == '1');
    else if (IS("LK") && n >= 4) FN(FN_LOCK, f[2] == '1');
    else if (IS("PR") && n >= 5) { if (f[2] == '0') FN(FN_PROC, f[3] == '1'); else if (f[2] == '1') FN(FN_EQ, f[3] == '1'); }
    else if (IS("KP") && n >= 5) { int v = 300 + atoi(p) * 10; SET(pitch, v); }
    else if (IS("IS") && n >= 10) {
        int v = atoi(f + 5);
        if (f[4] == '-') v = -v;
        FT(FT_IFS_ON, f[3] == '1');
        FT(FT_IFS, v);
    }
    else if (IS("BP") && n >= 8) {
        int v = atoi(f + 4);
        if (f[3] == '0') FT(FT_NOTCH_ON, v != 0); else FT(FT_NOTCH, v * 10);
    }
    else if (IS("CO") && n >= 9) {
        int v = atoi(f + 4);
        switch (f[3]) {
        case '0': FT(FT_CONT_ON, v != 0); break;
        case '1': FT(FT_CONT, v); break;
        case '2': FT(FT_APF_ON, v != 0); break;
        case '3': FT(FT_APF, v * 10 - 250); break;
        }
    }
    else if (IS("GT") && n >= 5) { int v = f[3] - '0'; SET(agc, v); }
    else if (IS("NA") && n >= 5) { int v = f[3] == '1'; SET(narrow, v); }
    else if (IS("SH") && n >= 7) { int v = atoi(f + 4); SET(width_idx, v); }
    else if (IS("ST") && n >= 4) { int v = f[2] != '0'; SET(split, v); }
    else if (IS("EX") && n >= 8) {
        int v = atoi(f + 6);
        if (!strncmp(f, "EX1107", 6)) SET(bfo_ssb, v);
        else if (!strncmp(f, "EX0707", 6)) SET(bfo_cw, v);
        else if (!strncmp(f, "EX0812", 6)) SET(bfo_data, v);
        else if (!strncmp(f, "EX1011", 6)) SET(bfo_rtty, v);
    }
}

/* --- csatlakozás --- */
static int probe(const char *path, int baud, int rts) {
    int f = open_port(path, baud, rts);
    if (f < 0) return f;
    tcflush(f, TCIOFLUSH);
    fd = f;
    rxn = 0;
    if (write(f, "ID;", 3) != 3) { close(f); fd = -1; return -EIO; }
    long long end = now_ms() + 300;
    char fr[96];
    while (fd >= 0 && now_ms() < end) {
        int r = read_frame(fr, sizeof fr, (int)(end - now_ms()), 0);
        if (r < 0) break;
        if (r == 1 && !strcmp(fr, "ID0650;")) return f;
    }
    if (fd >= 0) close(fd);
    fd = -1;
    return 0;
}

typedef struct { char path[256]; int trusted; } cand_t;

static int scan_candidates(cand_t *c, int max) {
    int n = 0;
    if (g_cfg.cat_port[0]) {
        snprintf(c[0].path, sizeof c[0].path, "%.255s", g_cfg.cat_port);
        c[0].trusted = 1;
        return 1;
    }
    DIR *d = opendir("/dev/serial/by-id");
    if (d) {
        struct dirent *e;
        while ((e = readdir(d)) && n < max) {
            if (!strstr(e->d_name, "CP2105") || !strstr(e->d_name, "-if00")) continue;
            snprintf(c[n].path, sizeof c[n].path, "/dev/serial/by-id/%.200s", e->d_name);
            c[n++].trusted = 1;
        }
        closedir(d);
    }
    if (n) return n;     /* a Standard portot így sosem nyitjuk meg véletlenül */
    d = opendir("/dev");
    if (d) {
        struct dirent *e;
        while ((e = readdir(d)) && n < max) {
            if (strncmp(e->d_name, "ttyUSB", 6) && strncmp(e->d_name, "ttyACM", 6)) continue;
            snprintf(c[n].path, sizeof c[n].path, "/dev/%.200s", e->d_name);
            c[n++].trusted = 0;
        }
        closedir(d);
        /* névsorrend: az Enhanced port az alacsonyabb sorszámú */
        for (int i = 0; i < n; i++)
            for (int j = i + 1; j < n; j++)
                if (strcmp(c[i].path, c[j].path) > 0) { cand_t t = c[i]; c[i] = c[j]; c[j] = t; }
    }
    return n;
}

static int try_connect(void) {
    static const int rates[] = {38400, 19200, 9600, 4800};
    cand_t c[8];
    int n = scan_candidates(c, 8), denied = 0, busy = 0;
    if (!n) { status("Nem találom a rádió USB-portját. Dugd be az USB-kábelt."); return 0; }
    for (int i = 0; i < n; i++) {
        int order[5], k = 0;
        if (g_cfg.last_baud) order[k++] = g_cfg.last_baud;
        for (int b = 0; b < 4; b++) if (rates[b] != g_cfg.last_baud) order[k++] = rates[b];
        for (int b = 0; b < k && !g_stop; b++) {
            int r = probe(c[i].path, order[b], c[i].trusted);
            if (r == -EACCES || r == -EPERM) { denied = 1; break; }
            if (r == -EBUSY) { busy = 1; break; }
            if (r > 0) {
                LOCK();
                snprintf(S.port, sizeof S.port, "%.255s", c[i].path);
                S.baud = order[b];
                S.rts_on = c[i].trusted;
                S.seq++;
                UNLOCK();
                LOCK();
                if (g_cfg.last_baud != order[b]) { g_cfg.last_baud = order[b]; config_save(); }
                UNLOCK();
                status("Kapcsolódva: %s, %d baud", c[i].path, order[b]);
                nto = 0;
                return 1;
            }
            if (r < 0) break;
        }
    }
    if (denied) status("Nincs jogosultság a porthoz. Futtasd: sudo usermod -a -G dialout $USER, majd lépj be újra.");
    else if (busy) status("A portot másik program használja. Állítsd le a rigctld-t vagy az flrig-et.");
    else status("A rádió nem válaszol. Kapcsold be, és nézd meg a 05-08 CAT RTS menüt.");
    return 0;
}

static const char *const SLOW[] = {
    "MD0;", "AG0;", "RG0;", "SQ0;", "PC;", "MG;", "SH0;", "NA0;", "NR0;", "RL0;", "NB0;",
    "NL0;", "BC0;", "PA0;", "RA0;", "GT0;", "PR0;", "PR1;", "PL;", "ST;", "LK;",
    "IS0;", "BP00;", "BP01;", "CO00;", "CO01;", "CO02;", "CO03;", "KP;",
    "EX1107;", "EX0707;", "EX0812;", "EX1011;"};
#define N_SLOW (int)(sizeof SLOW / sizeof SLOW[0])

static void read_fw(const char *cmd, char *out) {
    if (query(cmd) == 0 && strlen(last_reply) >= 11) {
        LOCK();
        snprintf(out, 8, "%.2s.%.2s", last_reply + 6, last_reply + 8);
        S.seq++;
        UNLOCK();
    }
}

static const char *const MENU_IDS[] = {"0506", "0507", "0508", "0707", "0801", "0809",
                                       "0810", "0812", "1105", "1107", "1108"};

static void do_menu_read(void) {
    char cmd[12];
    int n = 0, total = (int)(sizeof MENU_IDS / sizeof MENU_IDS[0]);
    for (int i = 0; i < total && fd >= 0; i++) {
        snprintf(cmd, sizeof cmd, "EX%s;", MENU_IDS[i]);
        int r = query(cmd);
        LOCK();
        snprintf(S.menu_id[n], sizeof S.menu_id[n], "%s", MENU_IDS[i]);
        if (r == 0) snprintf(S.menu_val[n], sizeof S.menu_val[n], "%.*s",
                             (int)strcspn(last_reply + 6, ";"), last_reply + 6);
        else snprintf(S.menu_val[n], sizeof S.menu_val[n], "?");
        n++;
        S.menu_n = n;
        S.seq++;
        UNLOCK();
    }
}

static void after_connect(void) {
    menu_on_connect();
    cat_set(g_cfg.use_ai ? "AI1;" : "AI0;");
    /* Menütérkép: a 11-07 csak akkor SSB BFO, ha van 11-09 (SSB TX BPF).
       Korábbi kiadásban a 11-07 az SSB PTT SELECT volt, oda tilos írni. */
    int ok = query("EX1109;") == 0 && strlen(last_reply) == 8 &&
             isdigit((unsigned char)last_reply[6]);
    SET(menu_ok, ok);
    if (ok) {
        read_fw("EX1801;", S.fw_main);
        read_fw("EX1802;", S.fw_dsp);
        read_fw("EX1803;", S.fw_lcd);
    } else if (fd >= 0) {
        rig_notice(1, "Eltérő menütáblát látok, ezért az oldalsáv-menüt nem írom. "
                      "Az USB/LSB ilyenkor csak MD paranccsal működik.");
    }
    query("FA;"); query("FB;"); query("IF;"); query("TX;"); query("SM0;");
    /* A felület és a külső programok csak beolvasott adatot lássanak */
    if (fd >= 0) SET(connected, 1);
    for (int i = 0; i < N_SLOW && fd >= 0; i++) {
        if (!ok && !strncmp(SLOW[i], "EX", 2)) continue;
        query(SLOW[i]);
    }
    if (ok && fd >= 0) do_menu_read();
    if (fd >= 0) ptt_port_open();
    if (fd >= 0) status("Kapcsolódva: %s, %d baud", S.port, S.baud);
}

/* --- PTT --- */
static void key_on(void) {
    long long t = now_ms();
    if (g_cfg.ptt_method == 0) { if (cat_write("TX1;")) return; }
    else hw_ptt(1);
    key_t0 = verify_t = t;
    rekeys = 0;
    SET(keyed, 1);
    log_msg("Adás be (%s)", g_cfg.ptt_method == 0 ? "CAT" : g_cfg.ptt_method == 1 ? "RTS" : "DTR");
}

static void key_off(void) {
    SET(keyed, 0);
    if (g_cfg.ptt_method == 0) {
        for (int i = 0; i < 3 && fd >= 0; i++) {
            if (cat_write("TX0;")) break;
            cat_pause(20);
            if (query("TX;") != 0 || last_reply[2] != '1') break;
        }
    } else {
        hw_ptt(0);
        if (fd >= 0) query("TX;");
    }
    log_msg("Adás ki");
    if (test_active) {
        test_active = 0;
        if (g_cfg.ptt_method != 0) rig_notice(0, "PTT-teszt lefutott. Figyeld a rádió TX-jelzését: a program az RTS/DTR módot nem tudja visszaolvasni.");
        else if (test_seen && !test_drop) rig_notice(0, "A PTT-teszt sikerült: a rádió adásba kapcsolt és ott is maradt.");
        else if (test_seen) rig_notice(1, "PTT-teszt: a rádió adás közben elengedte a PTT-t.");
        else rig_notice(1, "PTT-teszt: a rádió nem kapcsolt adásba.");
    }
}

static void service_ptt(void) {
    if (fd < 0) return;
    long long t = now_ms();
    LOCK();
    for (int i = 0; i < 32; i++)
        if ((hold_mask >> i & 1) && lease[i] && t > lease[i]) {
            hold_mask &= ~(1u << i);
            if (i != SRC_TEST) log_msg("A webes PTT jele elmaradt, elengedem az adást");
        }
    if (!hold_mask) guard = 0;
    int want = hold_mask != 0 && !guard;
    int keyed = S.keyed;
    int max_s = g_cfg.tx_max_s;
    UNLOCK();
    if (want && !keyed) key_on();
    else if (!want && keyed) key_off();
    else if (keyed && max_s > 0 && t - key_t0 > max_s * 1000LL) {
        guard = 1;
        key_off();
        rig_notice(1, "Az adásidő-korlát lejárt (%d mp), kikapcsoltam az adást. Engedd fel a PTT-t.", max_s);
    }
}

/* --- kérések --- */
static int verify_query(const char *cmd) {
    int r = query(cmd);
    return r == 0 ? 0 : -1;
}

static int set_bfo(const char *menu, int v) {
    char c[16];
    snprintf(c, sizeof c, "EX%s%d;", menu, v);
    if (cat_set(c)) return -1;
    snprintf(c, sizeof c, "EX%s;", menu);
    if (query(c)) return -1;
    return atoi(last_reply + 6) == v ? 0 : -1;
}

static int do_mode(int m, int external) {
    rig_state_t s;
    rig_get(&s);
    LOCK();
    int locked = g_cfg.mode_lock;
    UNLOCK();
    if (external && locked) {
        log_msg("Módzár: külső módváltás figyelmen kívül hagyva");
        return 0;
    }
    if (s.keyed || s.tx) { rig_notice(1, "Adás közben nem váltok módot."); return -1; }
    const char *md = NULL, *menu = NULL;
    int want = -1;
    switch (m) {
    case M_LSB: md = "MD01;"; menu = "1107"; want = 1; break;
    case M_USB: md = "MD02;"; menu = "1107"; want = 0; break;
    case M_SB_AUTO:
        if (!s.menu_ok) { rig_notice(1, "Az automatikus oldalsáv ezen a rádión nem állítható innen."); return -1; }
        if (set_bfo("1107", 2)) { rig_notice(1, "A rádió nem fogadta el az automatikus oldalsávot."); return -1; }
        md = rig_ssb_eff(2, s.freq_a) ? "MD01;" : "MD02;";
        break;
    case M_CW:  md = "MD03;"; menu = "0707"; want = 0; break;
    case M_CWR: md = "MD07;"; menu = "0707"; want = 1; break;
    case M_AM:  md = "MD05;"; break;
    case M_AMN: md = "MD0D;"; break;
    case M_FM:  md = "MD04;"; break;
    case M_FMN: md = "MD0B;"; break;
    case M_RTTY:   md = "MD06;"; menu = "1011"; want = 1; break;
    case M_RTTYR:  md = "MD09;"; menu = "1011"; want = 0; break;
    case M_PKTUSB: md = "MD0C;"; menu = "0812"; want = 0; break;
    case M_PKTLSB: md = "MD08;"; menu = "0812"; want = 1; break;
    default: return -1;
    }
    if (menu && s.menu_ok) {
        int cur = !strcmp(menu, "1107") ? s.bfo_ssb : !strcmp(menu, "0707") ? s.bfo_cw :
                  !strcmp(menu, "0812") ? s.bfo_data : s.bfo_rtty;
        /* Ha már úgyis ez az oldalsáv, a menühöz nem nyúlunk: így marad meg az AUTO */
        if (cur < 0 || rig_ssb_eff(cur, s.freq_a) != want) {
            if (set_bfo(menu, want)) { rig_notice(1, "A rádió nem fogadta el az oldalsáv-beállítást."); return -1; }
        }
    }
    if (cat_set(md)) return -1;
    if (query("MD0;")) return -1;
    query("IF;");
    return 0;
}

static int do_level(int l, int v) {
    static const struct { int lo, hi; const char *fmt, *rd; } T[L__COUNT] = {
        [L_AF] = {0, 255, "AG0%03d;", "AG0;"}, [L_RF] = {0, 30, "RG0%03d;", "RG0;"},
        [L_SQL] = {0, 100, "SQ0%03d;", "SQ0;"}, [L_PWR] = {5, 100, "PC%03d;", "PC;"},
        [L_MIC] = {0, 100, "MG%03d;", "MG;"},  [L_NRL] = {1, 15, "RL0%02d;", "RL0;"},
        [L_NBL] = {0, 10, "NL0%03d;", "NL0;"}, [L_PROC] = {0, 100, "PL%03d;", "PL;"}};
    char c[16];
    if (l < 0 || l >= L__COUNT) return -1;
    if (v < T[l].lo) v = T[l].lo;
    if (v > T[l].hi) v = T[l].hi;
    snprintf(c, sizeof c, T[l].fmt, v);
    if (cat_set(c)) return -1;
    return verify_query(T[l].rd);
}

static int do_func(int f, int v) {
    static const struct { const char *fmt, *rd; } T[FN_COUNT] = {
        [FN_NR] = {"NR0%d;", "NR0;"}, [FN_NB] = {"NB0%d;", "NB0;"}, [FN_NOTCH] = {"BC0%d;", "BC0;"},
        [FN_AMP] = {"PA0%d;", "PA0;"}, [FN_ATT] = {"RA0%d;", "RA0;"}, [FN_PROC] = {"PR0%d;", "PR0;"},
        [FN_LOCK] = {"LK%d;", "LK;"}, [FN_EQ] = {"PR1%d;", "PR1;"}};
    char c[16];
    if (f < 0 || f >= FN_COUNT) return -1;
    snprintf(c, sizeof c, T[f].fmt, v ? 1 : 0);
    if (cat_set(c)) return -1;
    return verify_query(T[f].rd);
}

static int clamp(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

static int do_filt(int k, int v) {
    char c[20];
    const char *rd;
    switch (k) {
    case FT_IFS_ON: case FT_IFS: {
        int on = k == FT_IFS_ON ? v != 0 : S.ft[FT_IFS_ON];
        int hz = k == FT_IFS ? clamp(v, -1200, 1200) / 20 * 20 : S.ft[FT_IFS];
        snprintf(c, sizeof c, "IS0%d%c%04d;", on, hz < 0 ? '-' : '+', abs(hz));
        rd = "IS0;";
        break; }
    case FT_NOTCH_ON: snprintf(c, sizeof c, "BP00%03d;", v != 0); rd = "BP00;"; break;
    case FT_NOTCH:    snprintf(c, sizeof c, "BP01%03d;", clamp(v, 10, 3200) / 10); rd = "BP01;"; break;
    case FT_CONT_ON:  snprintf(c, sizeof c, "CO00%04d;", v != 0); rd = "CO00;"; break;
    case FT_CONT:     snprintf(c, sizeof c, "CO01%04d;", clamp(v, 10, 3200)); rd = "CO01;"; break;
    case FT_APF_ON:   snprintf(c, sizeof c, "CO02%04d;", v != 0); rd = "CO02;"; break;
    case FT_APF:      snprintf(c, sizeof c, "CO03%04d;", (clamp(v, -250, 250) + 250) / 10); rd = "CO03;"; break;
    default: return -1;
    }
    if (cat_set(c)) return -1;
    return verify_query(rd);
}

static int do_width(int idx) {
    rig_state_t s;
    rig_get(&s);
    int f = family(s.md), lim = f == 1 ? 21 : 17, split = f == 1 ? 9 : 10;
    char c[16];
    if (f < 1 || f > 4) { rig_notice(1, "Ebben a módban nem állítható a szűrőszélesség."); return -1; }
    if (idx < 1) idx = 1;
    if (idx > lim) idx = lim;
    if (cat_set(idx < split ? "NA01;" : "NA00;")) return -1;
    snprintf(c, sizeof c, "SH01%02d;", idx);
    if (cat_set(c)) return -1;
    query("NA0;");
    return verify_query("SH0;");
}

/* Bekapcsolás: előbb egy tetszőleges adat, 1-2 mp múlva a tényleges parancs */
static void power_on(void) {
    cand_t c[8];
    char path[256];
    int n = scan_candidates(c, 8), rts = 1;
    LOCK();
    snprintf(path, sizeof path, "%s", S.port);
    int baud = g_cfg.last_baud ? g_cfg.last_baud : 38400;
    UNLOCK();
    if (n) { snprintf(path, sizeof path, "%.255s", c[0].path); rts = c[0].trusted; }
    if (!path[0]) { rig_notice(1, "Nem látom a rádió portját. Lehet, hogy kikapcsolva eltűnik, ilyenkor távolról nem kapcsolható be."); return; }
    int f = open_port(path, baud, rts);
    if (f < 0) { rig_notice(1, "A port nem nyitható meg: %s", strerror(-f)); return; }
    int ok = write(f, "PS1;", 4) == 4;
    usleep(1200000);
    ok = write(f, "PS1;", 4) == 4 && ok;
    tcdrain(f);
    close(f);
    if (ok) rig_notice(0, "Bekapcsolási parancs elküldve. Pár másodperc múlva kapcsolódnia kell.");
    else rig_notice(1, "A bekapcsolási parancs nem ment ki.");
}

static int run_job(const job_t *j) {
    char c[24];
    long long a = j->a;
    int r = 0;
    if (fd < 0) return -1;
    switch (j->op) {
    case OP_FREQ_A: case OP_FREQ_B:
        if (a < 30000 || a > 56000000) { rig_notice(1, "Ez a frekvencia a rádió tartományán kívül esik."); return -1; }
        snprintf(c, sizeof c, "%s%09lld;", j->op == OP_FREQ_A ? "FA" : "FB", a);
        if (cat_set(c)) return -1;
        if (j->op == OP_FREQ_A) SET(freq_a, a); else SET(freq_b, a);
        return 0;
    case OP_MODE: return do_mode((int)a, (int)j->b);
    case OP_BAND:
        if (S.keyed || S.tx) { rig_notice(1, "Adás közben nem váltok sávot."); return -1; }
        snprintf(c, sizeof c, "BS%02d;", (int)a);
        if (cat_set(c)) return -1;
        cat_pause(100);
        query("FA;"); query("MD0;"); query("IF;");
        return 0;
    case OP_LEVEL: return do_level((int)a, (int)j->b);
    case OP_FUNC:  return do_func((int)a, (int)j->b);
    case OP_FILT:  return do_filt((int)a, (int)j->b);
    case OP_AGC:
        snprintf(c, sizeof c, "GT0%d;", (int)a);
        if (cat_set(c)) return -1;
        return verify_query("GT0;");
    case OP_WIDTH: return do_width((int)a);
    case OP_SWAP: if (cat_set("SV;")) return -1; goto vfo;
    case OP_A2B:  if (cat_set("AB;")) return -1; goto vfo;
    case OP_B2A:  if (cat_set("BA;")) return -1;
    vfo:
        query("FA;"); query("FB;"); query("IF;");
        return 0;
    case OP_SPLIT:
        snprintf(c, sizeof c, "ST%d;", a ? 1 : 0);
        if (cat_set(c)) return -1;
        return verify_query("ST;");
    case OP_CLAR:
        snprintf(c, sizeof c, "CF0%d0;", a ? 1 : 0);
        if (cat_set(c)) return -1;
        return verify_query("IF;");
    case OP_CLAR_CLEAR:
        if (cat_set("RC;")) return -1;
        return verify_query("IF;");
    case OP_CLAR_STEP:
        snprintf(c, sizeof c, "%s%04d;", a < 0 ? "RD" : "RU", (int)llabs(a));
        if (cat_set(c)) return -1;
        return verify_query("IF;");
    case OP_PTT_METHOD:
        if (S.keyed) { rig_notice(1, "Adás közben nem váltok PTT-módot."); return -1; }
        if (a < 0 || a > 2) return -1;
        LOCK(); g_cfg.ptt_method = (int)a; config_save(); UNLOCK();
        r = ptt_port_open();
        LOCK(); S.seq++; UNLOCK();
        if (!r) rig_notice(0, "PTT-mód: %s", a == 0 ? "CAT" : a == 1 ? "RTS" : "DTR");
        return r;
    case OP_PTT_TEST:
        if (S.keyed || S.tx) { rig_notice(1, "Adás közben nem indítható a PTT-teszt."); return -1; }
        test_active = 1; test_seen = 0; test_drop = 0;
        rig_hold(SRC_TEST, 1, 1000);
        return 0;
    case OP_MENU_READ:
        if (S.menu_ok) do_menu_read();
        return 0;
    case OP_MENU_SWEEP: return menu_sweep();
    case OP_MENU_APPLY: return menu_apply();
    case OP_MEM_READ:   return mem_read();
    case OP_MEM_WRITE:  return mem_write();
    case OP_MEM_RECALL:
        if (S.keyed || S.tx) { rig_notice(1, "Adás közben nem hívok elő csatornát."); return -1; }
        r = mem_recall((int)a);
        query("FA;"); query("MD0;"); query("IF;");
        return r;
    case OP_POWER:
        if (!a) {
            if (S.keyed || S.tx) { rig_notice(1, "Adás közben nem kapcsolom ki a rádiót."); return -1; }
            power_off_req = 1;
            cat_write("PS0;");
        }
        return 0;
    case OP_PTT_SYNC:
        service_ptt();
        if (fd >= 0) query("TX;");
        return 0;
    default: return -1;
    }
}

static void drain_wake(void) {
    char b[32];
    while (read(wake_p[0], b, sizeof b) > 0) {}
}

static void run_jobs(int fail_all) {
    for (;;) {
        LOCK();
        if (!qn) { UNLOCK(); return; }
        job_t j = Q[0];
        memmove(Q, Q + 1, sizeof(job_t) * (size_t)--qn);
        UNLOCK();
        int r;
        if (fail_all && j.op == OP_POWER && j.a) { power_on(); r = 0; }
        else r = fail_all ? -1 : run_job(&j);
        if (j.op == OP_MENU_APPLY || j.op == OP_MEM_WRITE) menu_cancel();
        LOCK();
        if (j.done) { *j.rc = r; *j.done = 1; pthread_cond_broadcast(&C_done); }
        UNLOCK();
        if (!fail_all) service_ptt();
    }
}

static void poll_step(long long *next) {
    static int tick, slow_i, rm_i;
    static const char *const RM[] = {"RM5;", "RM6;", "RM4;"};
    long long t = now_ms();
    if (t < *next) return;
    LOCK();
    int tx = S.keyed || S.tx, have = clients > 0, ok = S.menu_ok;
    UNLOCK();
    if (tx) {
        *next = t + 150;
        query("TX;");
        if (fd >= 0) query(RM[rm_i++ % 3]);
        return;
    }
    *next = t + (have ? g_cfg.poll_ms : 1000);
    tick++;
    query("SM0;");
    if (fd >= 0 && tick % 2 == 0) { query("IF;"); if (fd >= 0) query("FB;"); if (fd >= 0) query("TX;"); }
    if (fd >= 0) {
        const char *c = SLOW[slow_i++ % N_SLOW];
        if (ok || strncmp(c, "EX", 2)) query(c);
    }
}

static void *cat_main(void *arg) {
    (void)arg;
    long long next = 0;
    char f[96];
    while (!g_stop) {
        if (fd < 0) {
            drain_wake();
            run_jobs(1);
            LOCK(); hold_mask = 0; UNLOCK();
            if (try_connect()) { after_connect(); next = 0; continue; }
            for (int i = 0; i < 20 && !g_stop; i++) {
                struct pollfd p = {wake_p[0], POLLIN, 0};
                poll(&p, 1, 100);
                drain_wake();
                run_jobs(1);
            }
            continue;
        }
        drain_wake();
        service_ptt();
        run_jobs(0);
        if (fd < 0) continue;
        poll_step(&next);
        if (fd < 0) continue;
        long long w = next - now_ms();
        if (w > 60) w = 60;   /* a PTT-határidők miatt sűrűn nézünk rá */
        if (w < 1) w = 1;
        if (read_frame(f, sizeof f, (int)w, 1) == 1) handle_frame(f);
    }
    if (fd >= 0) {
        LOCK(); hold_mask = 0; UNLOCK();
        if (S.keyed) key_off();
        if (g_cfg.use_ai) cat_write("AI0;");
        close(fd);
        fd = -1;
    }
    run_jobs(1);
    ptt_port_close();
    return NULL;
}

/* --- a menu.c segédei --- */
int cat_q(const char *cmd, char *reply, size_t cap) {
    int r = query(cmd);
    if (r == 0 && reply) snprintf(reply, cap, "%s", last_reply);
    return r;
}
int cat_s(const char *cmd) { return cat_set(cmd); }
/* Hosszú művelet közben is figyeljük a PTT-t; adás alatt nem írunk és nem olvasunk */
int cat_step(void) {
    service_ptt();
    LOCK();
    int free_to_go = fd >= 0 && !S.keyed && !S.tx && !hold_mask;
    UNLOCK();
    return free_to_go;
}
int rig_menu_ok(void) { return S.menu_ok; }
int rig_keyed(void) { LOCK(); int k = S.keyed; UNLOCK(); return k; }
int rig_set_mode_now(int m) { return do_mode(m, 0); }
void rig_menu_refresh(void) { if (S.menu_ok) do_menu_read(); }
void rig_bump_only(void) { LOCK(); S.seq++; UNLOCK(); }
void rig_bump(void) { LOCK(); S.seq++; S.data_seq++; UNLOCK(); }
void rig_job(const char *name, int i, int n) {
    LOCK();
    snprintf(S.job, sizeof S.job, "%s", name);
    S.job_i = i; S.job_n = n; S.seq++;
    UNLOCK();
}

/* --- külső felület --- */
void rig_start(void) {
    S.bfo_ssb = S.bfo_cw = S.bfo_data = S.bfo_rtty = -1;
    snprintf(S.status, sizeof S.status, "Indítás");
    if (pipe2(wake_p, O_NONBLOCK | O_CLOEXEC)) { perror("pipe"); exit(1); }
    pthread_create(&cat_th, NULL, cat_main, NULL);
}

static void wake(void) {
    if (write(wake_p[1], "x", 1) < 0) { /* tele van: úgyis felébred */ }
}

void rig_stop(void) {
    wake();
    pthread_join(cat_th, NULL);
}

void rig_get(rig_state_t *out) { LOCK(); *out = S; UNLOCK(); }
unsigned rig_seq(void) { LOCK(); unsigned v = S.seq; UNLOCK(); return v; }

static int enqueue(op_t op, long long a, long long b, int *done, int *rc) {
    LOCK();
    if (!done)
        for (int i = 0; i < qn; i++)
            if (Q[i].op == op && !Q[i].done &&
                (op == OP_FREQ_A || op == OP_FREQ_B || ((op == OP_LEVEL || op == OP_FILT) && Q[i].a == a))) {
                Q[i].a = a; Q[i].b = b;
                UNLOCK();
                return 0;
            }
    if (qn >= (int)(sizeof Q / sizeof Q[0])) { UNLOCK(); return -1; }
    Q[qn++] = (job_t){op, a, b, done, rc};
    UNLOCK();
    wake();
    return 0;
}

void rig_post(op_t op, long long a, long long b) { enqueue(op, a, b, NULL, NULL); }

int rig_do(op_t op, long long a, long long b) {
    int done = 0, rc = -1;
    if (enqueue(op, a, b, &done, &rc)) return -1;
    LOCK();
    while (!done) pthread_cond_wait(&C_done, &M);
    UNLOCK();
    return rc;
}

void rig_hold(int source, int on, int lease_ms) {
    LOCK();
    if (on) {
        hold_mask |= 1u << source;
        lease[source] = lease_ms ? now_ms() + lease_ms : 0;
    } else hold_mask &= ~(1u << source);
    UNLOCK();
    wake();
}

void rig_set_mode_lock(int on) {
    LOCK(); g_cfg.mode_lock = on ? 1 : 0; S.seq++; config_save(); UNLOCK();
}

void rig_set_tx_max(int s) {
    if (s < 10) s = 10;
    if (s > 900) s = 900;
    LOCK(); g_cfg.tx_max_s = s; S.seq++; config_save(); UNLOCK();
}

void rig_cfg_get(int *ptt, int *lock, int *txm, char *dev, size_t cap) {
    LOCK();
    *ptt = g_cfg.ptt_method; *lock = g_cfg.mode_lock; *txm = g_cfg.tx_max_s;
    snprintf(dev, cap, "%s", g_cfg.audio_dev);
    UNLOCK();
}

void rig_cfg_set_dev(const char *dev) {
    LOCK();
    snprintf(g_cfg.audio_dev, sizeof g_cfg.audio_dev, "%s", dev);
    config_save();
    UNLOCK();
}

void rig_client_count(int d) { LOCK(); clients += d; UNLOCK(); wake(); }
