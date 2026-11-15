/* rigctld-kompatibilis TCP-szerver: a helyi programok ezen át érik el a rádiót */
#include "ft891ctl.h"
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <unistd.h>

#define MAX_CLIENTS 8
#define RIG_OK 0
#define RIG_EINVAL -1
#define RIG_EIO -6
#define RIG_ENAVAIL -11

static int slot_used[MAX_CLIENTS];
static pthread_mutex_t slot_m = PTHREAD_MUTEX_INITIALIZER;

typedef struct { int fd, slot, ext; char sep; char out[4096]; size_t n; } cl_t;

static void flush_out(cl_t *c) {
    size_t o = 0;
    while (o < c->n) {
        ssize_t w = send(c->fd, c->out + o, c->n - o, MSG_NOSIGNAL);
        if (w <= 0) break;
        o += (size_t)w;
    }
    c->n = 0;
}

static void pr(cl_t *c, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void pr(cl_t *c, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = vsnprintf(c->out + c->n, sizeof c->out - c->n, fmt, ap);
    va_end(ap);
    if (r > 0) c->n += (size_t)r;
    if (c->n >= sizeof c->out) c->n = sizeof c->out - 1;
}

/* Érték kiírása: alap módban csak az érték, kibővítettben "Kulcs: érték" */
static void val(cl_t *c, const char *key, const char *v) {
    if (c->ext) pr(c, "%s: %s%c", key, v, c->sep);
    else pr(c, "%s\n", v);
}

static void val_ll(cl_t *c, const char *key, long long v) {
    char b[32];
    snprintf(b, sizeof b, "%lld", v);
    val(c, key, b);
}

static int mode_from(const char *n) {
    static const struct { const char *n; int m; } T[] = {
        {"USB", M_USB}, {"LSB", M_LSB}, {"CW", M_CW}, {"CWR", M_CWR}, {"AM", M_AM},
        {"AMN", M_AMN}, {"FM", M_FM}, {"FMN", M_FMN}, {"RTTY", M_RTTY}, {"RTTYR", M_RTTYR},
        {"PKTUSB", M_PKTUSB}, {"PKTLSB", M_PKTLSB}, {"PKTFM", M_FM}};
    for (size_t i = 0; i < sizeof T / sizeof T[0]; i++)
        if (!strcasecmp(n, T[i].n)) return T[i].m;
    return -1;
}

/* A Hamlib módmaszkjai: AM 1, CW 2, USB 4, LSB 8, RTTY 16, FM 32, CWR 128, RTTYR 256,
   PKTLSB 1024, PKTUSB 2048 */
#define MODES_ALL 0xDBFULL
static void dump_state(cl_t *c) {
    pr(c, "1\n1036\n0\n");
    pr(c, "30000 56000000 0x%llx -1 -1 0x3 0x1\n", MODES_ALL);
    pr(c, "0 0 0 0 0 0 0\n");
    pr(c, "1800000 2000000 0x%llx 5000 100000 0x3 0x1\n", MODES_ALL);
    pr(c, "3500000 4000000 0x%llx 5000 100000 0x3 0x1\n", MODES_ALL);
    pr(c, "5250000 5450000 0x%llx 5000 100000 0x3 0x1\n", MODES_ALL);
    pr(c, "7000000 7300000 0x%llx 5000 100000 0x3 0x1\n", MODES_ALL);
    pr(c, "10100000 10150000 0x%llx 5000 100000 0x3 0x1\n", MODES_ALL);
    pr(c, "14000000 14350000 0x%llx 5000 100000 0x3 0x1\n", MODES_ALL);
    pr(c, "18068000 18168000 0x%llx 5000 100000 0x3 0x1\n", MODES_ALL);
    pr(c, "21000000 21450000 0x%llx 5000 100000 0x3 0x1\n", MODES_ALL);
    pr(c, "24890000 24990000 0x%llx 5000 100000 0x3 0x1\n", MODES_ALL);
    pr(c, "28000000 29700000 0x%llx 5000 100000 0x3 0x1\n", MODES_ALL);
    pr(c, "50000000 54000000 0x%llx 5000 100000 0x3 0x1\n", MODES_ALL);
    pr(c, "0 0 0 0 0 0 0\n");
    pr(c, "0x%llx 10\n0x%llx 100\n0 0\n", MODES_ALL, MODES_ALL);
    pr(c, "0x%llx 2400\n0x%llx 500\n0 0\n", MODES_ALL, MODES_ALL);
    pr(c, "9999\n0\n1200\n0\n");
    pr(c, "10 \n12 \n");
    pr(c, "0x0\n0x0\n0x40000000\n0x0\n0x0\n0x0\n");
    pr(c, "vfo_ops=0x0\nptt_type=0x1\ntargetable_vfo=0x0\nhas_set_vfo=1\nhas_get_vfo=1\n"
          "has_set_freq=1\nhas_get_freq=1\nhas_set_conf=0\nhas_get_conf=0\n"
          "has_power2mW=0\nhas_mW2power=0\ntimeout=2000\nrig_model=1036\ndone\n");
}

static int strength_db(int raw) {
    return raw <= 130 ? (raw - 130) * 54 / 130 : (raw - 130) * 60 / 125;
}

static const char *const LONGNAME[][2] = {
    {"f", "get_freq"}, {"F", "set_freq"}, {"m", "get_mode"}, {"M", "set_mode"},
    {"v", "get_vfo"}, {"V", "set_vfo"}, {"t", "get_ptt"}, {"T", "set_ptt"},
    {"s", "get_split_vfo"}, {"S", "set_split_vfo"}, {"i", "get_split_freq"},
    {"I", "set_split_freq"}, {"x", "get_split_mode"}, {"X", "set_split_mode"},
    {"l", "get_level"}, {"L", "set_level"}, {"_", "get_info"}, {"j", "get_rit"},
    {"z", "get_xit"}, {"q", "quit"}, {"Q", "quit"}, {NULL, NULL}};

static const char *canon(const char *w) {
    for (int i = 0; LONGNAME[i][0]; i++)
        if (!strcmp(w, LONGNAME[i][0])) return LONGNAME[i][1];
    return w;
}

/* Egy parancssor. 1-et ad vissza, ha a kapcsolat zárandó */
static int handle(cl_t *c, char *line) {
    /* Böngészőből küldött HTTP-kérés ne tudjon rigctld-parancsot csempészni */
    if (strstr(line, "HTTP/1.") || !strncasecmp(line, "Host:", 5) || !strncasecmp(line, "Origin:", 7)) {
        log_msg("HTTP-kérés érkezett a rigctld-portra, a kapcsolatot bontom");
        return 1;
    }
    char *save = NULL, *w = strtok_r(line, " \t\r\n", &save);
    if (!w) return 0;
    c->ext = 0; c->sep = '\n';
    if (*w == '+' || *w == ';' || *w == '|' || *w == ',') {
        c->ext = 1;
        c->sep = *w == '+' ? '\n' : *w;
        w++;
        if (!*w) w = strtok_r(NULL, " \t\r\n", &save);
        if (!w) return 0;
    }
    if (*w == '\\') w++;
    const char *cmd = canon(w);
    char *a1 = strtok_r(NULL, " \t\r\n", &save), *a2 = strtok_r(NULL, " \t\r\n", &save);
    rig_state_t s;
    rig_get(&s);
    int rc = RIG_OK, is_get = 0;

    if (c->ext) pr(c, "%s:%c", cmd, c->sep);

    if (!strcmp(cmd, "quit")) return 1;
    else if (!strcmp(cmd, "chk_vfo")) { val(c, "ChkVFO", "0"); is_get = 1; }
    else if (!strcmp(cmd, "dump_state")) { dump_state(c); is_get = 1; }
    else if (!strcmp(cmd, "dump_caps")) { pr(c, "Model name: FT-891\nMfg name: Yaesu\n"); is_get = 1; }
    else if (!strcmp(cmd, "get_info")) { val(c, "Info", "FT-891 (ft891ctl)"); is_get = 1; }
    else if (!strcmp(cmd, "get_powerstat")) { val(c, "Power Status", s.connected ? "1" : "0"); is_get = 1; }
    else if (!strcmp(cmd, "get_lock_mode")) { val(c, "Locked", "0"); is_get = 1; }
    else if (!strcmp(cmd, "set_lock_mode")) { /* nincs teendő */ }
    else if (!strcmp(cmd, "set_vfo_opt")) { /* nincs teendő */ }
    else if (!strcmp(cmd, "get_vfo")) { val(c, "VFO", "VFOA"); is_get = 1; }
    else if (!strcmp(cmd, "set_vfo")) {
        if (!a1 || (strcasecmp(a1, "VFOA") && strcasecmp(a1, "currVFO") && strcasecmp(a1, "Main")))
            rc = RIG_ENAVAIL;
    }
    else if (!strcmp(cmd, "get_rit")) { val_ll(c, "RIT", s.clar ? s.clar_ofs : 0); is_get = 1; }
    else if (!strcmp(cmd, "get_xit")) { val(c, "XIT", "0"); is_get = 1; }
    else if (!s.connected) rc = RIG_EIO;
    else if (!strcmp(cmd, "get_freq")) { val_ll(c, "Frequency", s.freq_a); is_get = 1; }
    else if (!strcmp(cmd, "set_freq")) {
        if (!a1) rc = RIG_EINVAL;
        else rc = rig_do(OP_FREQ_A, atoll(a1), 0) ? RIG_EINVAL : RIG_OK;
    }
    else if (!strcmp(cmd, "get_mode")) {
        val(c, "Mode", rig_hamlib_mode(&s));
        val_ll(c, "Passband", rig_width_hz(&s));
        is_get = 1;
    }
    else if (!strcmp(cmd, "set_mode")) {
        int m = a1 ? mode_from(a1) : -1;
        if (m < 0) rc = RIG_EINVAL;
        else rc = rig_do(OP_MODE, m, 1) ? RIG_EIO : RIG_OK;
    }
    else if (!strcmp(cmd, "get_ptt")) { val_ll(c, "PTT", s.keyed || s.tx); is_get = 1; }
    else if (!strcmp(cmd, "set_ptt")) {
        if (!a1) rc = RIG_EINVAL;
        else {
            rig_hold(SRC_RIG(c->slot), atoi(a1) != 0, 0);
            rig_do(OP_PTT_SYNC, 0, 0);
        }
    }
    else if (!strcmp(cmd, "get_split_vfo")) {
        val_ll(c, "Split", s.split);
        val(c, "TX VFO", s.split ? "VFOB" : "VFOA");
        is_get = 1;
    }
    else if (!strcmp(cmd, "set_split_vfo")) {
        if (!a1) rc = RIG_EINVAL;
        else rc = rig_do(OP_SPLIT, atoi(a1) != 0, 0) ? RIG_EIO : RIG_OK;
    }
    else if (!strcmp(cmd, "get_split_freq")) { val_ll(c, "TX Frequency", s.freq_b); is_get = 1; }
    else if (!strcmp(cmd, "set_split_freq")) {
        if (!a1) rc = RIG_EINVAL;
        else rc = rig_do(OP_FREQ_B, atoll(a1), 0) ? RIG_EINVAL : RIG_OK;
    }
    else if (!strcmp(cmd, "get_split_mode")) {
        val(c, "TX Mode", rig_hamlib_mode(&s));
        val_ll(c, "TX Passband", rig_width_hz(&s));
        is_get = 1;
    }
    else if (!strcmp(cmd, "set_split_mode")) { /* a mód a két VFO-n közös */ }
    else if (!strcmp(cmd, "get_dcd")) { val_ll(c, "DCD", s.smeter > 0); is_get = 1; }
    else if (!strcmp(cmd, "get_level")) {
        if (a1 && !strcasecmp(a1, "STRENGTH")) { val_ll(c, "Level Value", strength_db(s.smeter)); is_get = 1; }
        else if (a1 && !strcasecmp(a1, "RFPOWER")) {
            char b[16];
            snprintf(b, sizeof b, "%.2f", s.lvl[L_PWR] / 100.0);
            val(c, "Level Value", b);
            is_get = 1;
        } else rc = RIG_ENAVAIL;
    }
    else if (!strcmp(cmd, "set_level")) {
        if (a1 && a2 && !strcasecmp(a1, "RFPOWER"))
            rc = rig_do(OP_LEVEL, L_PWR, (long long)(atof(a2) * 100 + 0.5)) ? RIG_EIO : RIG_OK;
        else rc = RIG_ENAVAIL;
    }
    else if (!strcmp(cmd, "get_vfo_info")) {
        val_ll(c, "Freq", s.freq_a);
        val(c, "Mode", rig_hamlib_mode(&s));
        val_ll(c, "Width", rig_width_hz(&s));
        val_ll(c, "Split", s.split);
        val(c, "SatMode", "0");
        is_get = 1;
    }
    else rc = RIG_ENAVAIL;

    if (c->ext) pr(c, "RPRT %d\n", rc);
    else if (!is_get || rc) pr(c, "RPRT %d\n", rc);
    return 0;
}

typedef struct { int fd, slot; } arg_t;

static void *client_main(void *p) {
    arg_t a = *(arg_t *)p;
    free(p);
    cl_t c;
    memset(&c, 0, sizeof c);
    c.fd = a.fd; c.slot = a.slot; c.sep = '\n';
    char buf[512];
    size_t n = 0;
    int done = 0;
    while (!g_stop && !done) {
        struct pollfd pf = {a.fd, POLLIN, 0};
        int r = poll(&pf, 1, 500);
        if (r == 0) continue;
        if (r < 0 && errno == EINTR) continue;
        if (r < 0) break;
        ssize_t got = recv(a.fd, buf + n, sizeof buf - 1 - n, 0);
        if (got <= 0) break;
        n += (size_t)got;
        buf[n] = 0;
        char *nl;
        while ((nl = memchr(buf, '\n', n)) != NULL) {
            *nl = 0;
            size_t len = (size_t)(nl - buf) + 1;
            done = handle(&c, buf);
            flush_out(&c);
            n -= len;
            memmove(buf, buf + len, n);
            buf[n] = 0;
            if (done) break;
        }
        if (n >= sizeof buf - 1) n = 0;
    }
    /* Ha a program adás közben szűnt meg, az adást elengedjük */
    rig_hold(SRC_RIG(a.slot), 0, 0);
    close(a.fd);
    pthread_mutex_lock(&slot_m);
    slot_used[a.slot] = 0;
    pthread_mutex_unlock(&slot_m);
    return NULL;
}

static void *accept_main(void *p) {
    int ls = *(int *)p;
    free(p);
    while (!g_stop) {
        int f = accept(ls, NULL, NULL);
        if (f < 0) { if (errno == EINTR) continue; break; }
        int one = 1;
        setsockopt(f, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
        int slot = -1;
        pthread_mutex_lock(&slot_m);
        for (int i = 0; i < MAX_CLIENTS; i++) if (!slot_used[i]) { slot = i; slot_used[i] = 1; break; }
        pthread_mutex_unlock(&slot_m);
        if (slot < 0) { close(f); continue; }
        arg_t *a = malloc(sizeof *a);
        a->fd = f; a->slot = slot;
        pthread_t t;
        pthread_create(&t, NULL, client_main, a);
        pthread_detach(t);
    }
    return NULL;
}

void rigsrv_start(void) {
    int ls = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0), one = 1;
    struct sockaddr_in sa = {0};
    sa.sin_family = AF_INET;
    sa.sin_port = htons((uint16_t)g_cfg.rig_port);
    inet_pton(AF_INET, g_cfg.rig_bind, &sa.sin_addr);
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    if (bind(ls, (struct sockaddr *)&sa, sizeof sa) || listen(ls, 8)) {
        log_msg("A rigctld-port (%d) foglalt: %s. Állítsd le a rigctld-t.", g_cfg.rig_port, strerror(errno));
        close(ls);
        return;
    }
    log_msg("rigctld-kompatibilis port: %s:%d", g_cfg.rig_bind, g_cfg.rig_port);
    int *a = malloc(sizeof *a);
    *a = ls;
    pthread_t t;
    pthread_create(&t, NULL, accept_main, a);
    pthread_detach(t);
}
