/* Egyszerű SSTV-dekóder: a vételi hangból (8 kHz) VIS alapján vagy kézzel indul.
   A sor elején a szinkronimpulzust keresi, így követi az órajel-eltérést. */
#include "ft891ctl.h"
#include <dirent.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define FS 8000.0
#define RING 65536
#define MAXW 800
#define MAXH 616
#define KEEP_IMAGES 50

enum { K_SCOTTIE, K_MARTIN, K_R36, K_R72, K_PD };
typedef struct {
    const char *name; int vis, w, h, kind;
    double scan, sync, porch, sep, period;     /* ms */
} mode_t_;

static const mode_t_ MODES[] = {
    {"Scottie 1",  60, 320, 256, K_SCOTTIE, 138.24, 9.0, 1.5, 1.5, 428.22},
    {"Scottie 2",  56, 320, 256, K_SCOTTIE, 88.064, 9.0, 1.5, 1.5, 277.692},
    {"Scottie DX", 76, 320, 256, K_SCOTTIE, 345.6,  9.0, 1.5, 1.5, 1050.3},
    {"Martin 1",   44, 320, 256, K_MARTIN,  146.432, 4.862, 0.572, 0.572, 446.446},
    {"Martin 2",   40, 320, 256, K_MARTIN,  73.216,  4.862, 0.572, 0.572, 226.798},
    {"Robot 36",    8, 320, 240, K_R36, 88.0,  9.0, 3.0, 4.5, 150.0},
    {"Robot 72",   12, 320, 240, K_R72, 138.0, 9.0, 3.0, 4.5, 300.0},
    {"PD 50",      93, 320, 256, K_PD, 91.52,   20.0, 2.08, 0, 388.16},
    {"PD 90",      99, 320, 256, K_PD, 170.24,  20.0, 2.08, 0, 703.04},
    {"PD 120",     95, 640, 496, K_PD, 121.6,   20.0, 2.08, 0, 508.48},
    {"PD 160",     98, 512, 400, K_PD, 195.584, 20.0, 2.08, 0, 804.416},
    {"PD 180",     96, 640, 496, K_PD, 183.04,  20.0, 2.08, 0, 754.24},
    {"PD 240",     97, 640, 496, K_PD, 244.48,  20.0, 2.08, 0, 1000.0},
    {"PD 290",     94, 800, 616, K_PD, 228.8,   20.0, 2.08, 0, 937.28},
};
#define N_MODES (int)(sizeof MODES / sizeof MODES[0])

static pthread_mutex_t L = PTHREAD_MUTEX_INITIALIZER;
static uint8_t img[MAXH][MAXW * 3];
static int img_id, img_lines, img_w, img_h, active, enabled, armed_want;
static char st_mode[24], st_msg[120];
static int manual_req = -1, stop_req;

/* demodulátor (csak a hangszálon) */
static float fr[RING];
static unsigned long long N;
static float vb[128];
static int vbi;
static double vbsum;
static int vbn;

/* képfolyamat (csak a hangszálon) */
static const mode_t_ *M;
static double d_ofs, tnext, period;
static int line, misses;
static int fresh;
static uint8_t cu[MAXW], cv[MAXW];

#define F(i) fr[(unsigned long long)(i) % RING]
static double ms(double m) { return m * FS / 1000.0; }

/* --- PNG tömörítés nélkül: nem kell hozzá külső könyvtár --- */
static uint32_t crc32_(const uint8_t *d, size_t n, uint32_t c) {
    static uint32_t t[256];
    if (!t[1])
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t x = i;
            for (int k = 0; k < 8; k++) x = x & 1 ? 0xEDB88320u ^ (x >> 1) : x >> 1;
            t[i] = x;
        }
    for (size_t i = 0; i < n; i++) c = t[(c ^ d[i]) & 255] ^ (c >> 8);
    return c;
}

static void be32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }

static void chunk(FILE *f, const char *type, const uint8_t *d, size_t n) {
    uint8_t b[4];
    be32(b, (uint32_t)n);
    fwrite(b, 1, 4, f);
    fwrite(type, 1, 4, f);
    if (n) fwrite(d, 1, n, f);
    uint32_t c = crc32_((const uint8_t *)type, 4, 0xFFFFFFFFu);
    c = ~crc32_(d, n, c);
    be32(b, c);
    fwrite(b, 1, 4, f);
}

static int save_png(const char *path, int w, int h) {
    size_t row = (size_t)w * 3 + 1, raw = row * (size_t)h, blocks = raw / 65535 + 1;
    size_t zn = 2 + raw + blocks * 5 + 4;
    uint8_t *z = malloc(zn), *line_b = malloc(row);
    FILE *f = fopen(path, "wb");
    if (!z || !line_b || !f) { free(z); free(line_b); if (f) fclose(f); return -1; }
    static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', 13, 10, 26, 10};
    fwrite(sig, 1, 8, f);
    uint8_t ih[13] = {0};
    be32(ih, (uint32_t)w); be32(ih + 4, (uint32_t)h);
    ih[8] = 8; ih[9] = 2;
    chunk(f, "IHDR", ih, 13);
    size_t o = 0, left = raw, y = 0, col = 0;
    uint32_t a = 1, b = 0;
    z[o++] = 0x78; z[o++] = 0x01;
    while (left) {
        size_t n = left > 65535 ? 65535 : left;
        z[o++] = left == n;
        z[o++] = (uint8_t)n; z[o++] = (uint8_t)(n >> 8);
        z[o++] = (uint8_t)~n; z[o++] = (uint8_t)(~n >> 8);
        for (size_t i = 0; i < n; i++) {
            if (col == 0) {
                line_b[0] = 0;
                pthread_mutex_lock(&L);
                memcpy(line_b + 1, img[y], (size_t)w * 3);
                pthread_mutex_unlock(&L);
            }
            uint8_t v = line_b[col++];
            if (col == row) { col = 0; y++; }
            z[o++] = v;
            a = (a + v) % 65521;
            b = (b + a) % 65521;
        }
        left -= n;
    }
    be32(z + o, b << 16 | a);
    o += 4;
    chunk(f, "IDAT", z, o);
    chunk(f, "IEND", NULL, 0);
    free(z);
    free(line_b);
    return fclose(f);
}

static void img_dir(char *o, size_t c) { snprintf(o, c, "%s/sstv", g_cfg.cfg_dir); }

static int cmp_d(const void *a, const void *b) { return strcmp(*(char *const *)b, *(char *const *)a); }

static int list_imgs(char **l, int max) {
    char d[320];
    img_dir(d, sizeof d);
    DIR *h = opendir(d);
    int n = 0;
    if (!h) return 0;
    struct dirent *e;
    while ((e = readdir(h)) && n < max)
        if (strstr(e->d_name, ".png")) l[n++] = strdup(e->d_name);
    closedir(h);
    qsort(l, (size_t)n, sizeof *l, cmp_d);
    return n;
}

static void msg(const char *t) {
    pthread_mutex_lock(&L);
    snprintf(st_msg, sizeof st_msg, "%s", t);
    pthread_mutex_unlock(&L);
    rig_bump_only();
}

static void finish(const char *why) {
    int lines;
    pthread_mutex_lock(&L);
    lines = img_lines;
    active = 0;
    pthread_mutex_unlock(&L);
    if (lines >= 16) {
        char dir[320], ts[32], nm[64], path[420], *l[256];
        time_t t = time(NULL);
        struct tm m;
        localtime_r(&t, &m);
        strftime(ts, sizeof ts, "%Y%m%d-%H%M%S", &m);
        snprintf(nm, sizeof nm, "%s-%s", ts, M->name);
        for (char *p = nm; *p; p++) if (*p == ' ') *p = '_';
        img_dir(dir, sizeof dir);
        mkdir(dir, 0700);
        /* Átnevezéssel lesz kész: a galéria nem lát félig írt fájlt */
        char tmp[430];
        snprintf(path, sizeof path, "%s/%s.png", dir, nm);
        snprintf(tmp, sizeof tmp, "%s/%s.part", dir, nm);
        if (save_png(tmp, M->w, M->h) || rename(tmp, path)) log_msg(TR("Az SSTV-kép nem menthető", "Cannot save the SSTV picture"));
        else log_msg(TR("SSTV-kép mentve: %s.png", "SSTV picture saved: %s.png"), nm);
        int n = list_imgs(l, 256);
        for (int i = 0; i < n; i++) {
            if (i >= KEEP_IMAGES) { snprintf(path, sizeof path, "%s/%s", dir, l[i]); unlink(path); }
            free(l[i]);
        }
        rig_bump();
    }
    msg(why);
    M = NULL;
}

static void start(int m, double ofs, double t0) {
    const mode_t_ *x = &MODES[m];
    if (M) finish(TR("Új kép kezdődött.", "A new picture started."));
    M = x;
    d_ofs = ofs;
    period = ms(x->period);
    line = 0;
    misses = 0;
    fresh = 1;
    /* az első szinkron helye */
    tnext = t0;
    if (x->kind == K_SCOTTIE) tnext += ms(x->sync + 2 * (x->sep + x->scan));
    memset(cu, 128, sizeof cu);
    memset(cv, 128, sizeof cv);
    pthread_mutex_lock(&L);
    memset(img, 0, sizeof img);
    img_id++;
    img_lines = 0;
    img_w = x->w;
    img_h = x->h;
    active = 1;
    snprintf(st_mode, sizeof st_mode, "%s", x->name);
    pthread_mutex_unlock(&L);
    msg(TR("Kép vétele folyamatban.", "Receiving a picture."));
    log_msg(TR("SSTV: %s, elhangolás %+.0f Hz", "SSTV: %s, offset %+.0f Hz"), x->name, ofs);
}

/* Egy csatorna képpontjai t-től kezdve */
static void channel(double t, double scan_ms, uint8_t *out, int w) {
    double px = ms(scan_ms) / w;
    for (int x = 0; x < w; x++) {
        double a = t + x * px, b = a + px;
        long ia = lround(a), ib = lround(b);
        if (ib <= ia) ib = ia + 1;
        double s = 0;
        for (long i = ia; i < ib; i++) s += F(i);
        double v = ((s / (double)(ib - ia)) - d_ofs - 1500.0) / 800.0 * 255.0;
        out[x] = (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v + 0.5);
    }
}

static uint8_t clamp8(double v) { return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v + 0.5); }

static void put_rgb(int y, const uint8_t *r, const uint8_t *g, const uint8_t *b, int w) {
    pthread_mutex_lock(&L);
    for (int x = 0; x < w; x++) { img[y][x * 3] = r[x]; img[y][x * 3 + 1] = g[x]; img[y][x * 3 + 2] = b[x]; }
    if (y + 1 > img_lines) img_lines = y + 1;
    pthread_mutex_unlock(&L);
}

static void put_yuv(int y, const uint8_t *Y, const uint8_t *V, const uint8_t *U, int w) {
    static uint8_t r[MAXW], g[MAXW], b[MAXW];
    for (int x = 0; x < w; x++) {
        double yy = Y[x], u = U[x] - 128.0, v = V[x] - 128.0;
        r[x] = clamp8(yy + 1.402 * v);
        g[x] = clamp8(yy - 0.344 * u - 0.714 * v);
        b[x] = clamp8(yy + 1.772 * u);
    }
    put_rgb(y, r, g, b, w);
}

/* A várt hely körül megkeresi a szinkront. Eltérés mintákban, *score 0..1 */
static double find_sync(double e, double win, double len, double *score) {
    long a = lround(e - win), b = lround(e + win), n = lround(len);
    double lim = 1350.0 + d_ofs;
    int cnt = 0;
    for (long i = a; i < a + n; i++) cnt += F(i) < lim;
    int best = cnt, bk = (int)a;
    for (long k = a + 1; k <= b; k++) {
        cnt += (F(k + n - 1) < lim) - (F(k - 1) < lim);
        if (cnt > best || (cnt == best && labs(k - lround(e)) < labs(bk - lround(e)))) { best = cnt; bk = (int)k; }
    }
    *score = (double)best / (double)n;
    return bk - e;
}

/* A szinkron óta eltelt idő, amíg a sor adatai megérkeznek */
static double data_after(const mode_t_ *m) {
    switch (m->kind) {
    case K_SCOTTIE: return ms(m->sync + m->porch + m->scan);
    case K_MARTIN:  return ms(m->sync + m->porch + 3 * m->scan + 3 * m->sep);
    case K_R36:     return ms(m->sync + m->porch + m->scan + m->sep + 1.5 + m->scan / 2);
    case K_R72:     return ms(m->sync + m->porch + m->scan + 2 * (m->sep + 1.5 + m->scan / 2));
    default:        return ms(m->sync + m->porch + 4 * m->scan);
    }
}

static void decode_line(double t) {
    static uint8_t a[MAXW], b[MAXW], c[MAXW], d[MAXW];
    const mode_t_ *m = M;
    int w = m->w;
    double p = t + ms(m->sync + m->porch), s = ms(m->scan);
    switch (m->kind) {
    case K_SCOTTIE: {
        channel(t - ms(2 * m->scan + m->sep), m->scan, a, w);     /* zöld */
        channel(t - s, m->scan, b, w);                            /* kék */
        channel(p, m->scan, c, w);              /* piros */
        put_rgb(line, c, a, b, w);
        break; }
    case K_MARTIN: {
        double sc = ms(m->scan + m->sep);
        channel(p, m->scan, a, w);
        channel(p + sc, m->scan, b, w);
        channel(p + 2 * sc, m->scan, c, w);
        put_rgb(line, c, a, b, w);
        break; }
    case K_R36: {
        channel(p, m->scan, a, w);
        double q = p + s, sum = 0;
        int n = (int)ms(m->sep);
        for (int i = 0; i < n; i++) sum += F((long)q + i);
        double t2 = q + ms(m->sep + 1.5);
        if (sum / n - d_ofs < 1900) channel(t2, m->scan / 2, cv, w);
        else channel(t2, m->scan / 2, cu, w);
        put_yuv(line, a, cv, cu, w);
        break; }
    case K_R72: {
        channel(p, m->scan, a, w);
        double q = p + s + ms(m->sep + 1.5);
        channel(q, m->scan / 2, cv, w);
        channel(q + ms(m->scan / 2 + m->sep + 1.5), m->scan / 2, cu, w);
        put_yuv(line, a, cv, cu, w);
        break; }
    default:
        channel(p, m->scan, a, w);
        channel(p + s, m->scan, b, w);
        channel(p + 2 * s, m->scan, c, w);
        channel(p + 3 * s, m->scan, d, w);
        put_yuv(line, a, b, c, w);
        if (line + 1 < m->h) put_yuv(line + 1, d, b, c, w);
        break;
    }
}

static void run_lines(void) {
    while (M) {
        double win = fresh == 2 ? period * 0.55 : fresh ? ms(16) : ms(8);
        double need = tnext + win + data_after(M) + ms(2);
        if ((double)N < need + 40) return;
        double sc, err = find_sync(tnext, win, ms(M->sync), &sc);
        if (sc > 0.65) {
            tnext += fresh ? err : err * 0.5;
            if (!fresh) {
                period += err * 0.03;
                double nom = ms(M->period);
                if (period < nom * 0.995) period = nom * 0.995;
                if (period > nom * 1.005) period = nom * 1.005;
            }
            fresh = 0;
            misses = 0;
        } else if (++misses >= 3) fresh = 2;   /* elveszett a szinkron: újra teljes soron keres */
        decode_line(tnext);
        line += M->kind == K_PD ? 2 : 1;
        tnext += period;
        if (line >= M->h) { finish(TR("A kép elkészült.", "Picture complete.")); return; }
        /* Hosszú jelvesztés után leáll, de legalább 5 mp-ig kitart */
        if (misses * M->period > 8000 && misses >= 6) { finish(TR("A jel elveszett, a kép megszakadt.", "Signal lost, picture stopped.")); return; }
    }
}

static void check_vis(void) {
#define V(k) vb[(vbi - (k)) & 127]
    double sum = 0, mn = 1e9, mx = -1e9;
    for (int k = 33; k <= 58; k++) {   /* a második vivő közepe */
        double v = V(k);
        sum += v;
        if (v < mn) mn = v;
        if (v > mx) mx = v;
    }
    double lead = sum / 26, d = lead - 1900;
    if (fabs(d) > 150 || mx - mn > 70) return;
    double base = 1200 + d;
    if (fabs(V(29) - base) > 55 || fabs(V(2) - base) > 55) return;
    int code = 0, ones = 0;
    for (int b = 0; b < 8; b++) {
        double f = V(26 - 3 * b);
        if (fabs(fabs(f - base) - 100) > 55) return;
        if (f < base) { if (b < 7) code |= 1 << b; ones++; }
    }
    if (ones & 1) return;      /* páros paritás */
    for (int i = 0; i < N_MODES; i++)
        if (MODES[i].vis == code) { start(i, d, (double)N); return; }
    log_msg(TR("SSTV: ismeretlen VIS-kód: %d", "SSTV: unknown VIS code: %d"), code);
}

/* --- hangminták: 8 kHz, a hangszálon --- */
void sstv_push(float y) {
    static float c[31], ir[31], iq[31];
    static int ready, pos;
    static double ph, pi_, pq;
    int on, man, stop;
    pthread_mutex_lock(&L);
    on = enabled;
    man = manual_req; manual_req = -1;
    stop = stop_req; stop_req = 0;
    pthread_mutex_unlock(&L);
    if (!on && !M) return;
    if (!ready) {
        double sum = 0;
        for (int i = 0; i < 31; i++) {
            double n = i - 15, fc = 1000.0 / FS;
            double v = n == 0 ? 2 * fc : sin(2 * M_PI * fc * n) / (M_PI * n);
            c[i] = (float)(v * (0.54 - 0.46 * cos(2 * M_PI * i / 30)));
            sum += c[i];
        }
        for (int i = 0; i < 31; i++) c[i] /= (float)sum;
        ready = 1;
    }
    ph += 2 * M_PI * 1900.0 / FS;
    if (ph > 2 * M_PI) ph -= 2 * M_PI;
    ir[pos] = y * (float)cos(ph);
    iq[pos] = -y * (float)sin(ph);
    pos = (pos + 1) % 31;
    double re = 0, im = 0;
    for (int i = 0; i < 31; i++) {
        int k = (pos + i) % 31;
        re += c[i] * ir[k];
        im += c[i] * iq[k];
    }
    double dr = re * pi_ + im * pq, di = im * pi_ - re * pq;
    pi_ = re; pq = im;
    double f = 1900.0 + atan2(di, dr) * FS / (2 * M_PI);
    F(N) = (float)f;
    N++;
    vbsum += f;
    if (++vbn == 80) {
        vb[vbi & 127] = (float)(vbsum / 80);
        vbi = (vbi + 1) & 127;
        vbsum = 0;
        vbn = 0;
        if (on) check_vis();
    }
    if (stop && M) finish(TR("A vételt leállítottad.", "Reception stopped."));
    if (man >= 0 && man < N_MODES) {
        /* Kézi indítás: nem tudjuk, hol tart a sor, ezért egy teljes sorhosszon keresünk */
        start(man, 0, (double)N + ms(MODES[man].period) * 0.6);
        tnext = (double)N + ms(MODES[man].period) * 0.6;
        fresh = 2;
    }
    if (M) {
        if (rig_keyed()) { finish(TR("Adás miatt megszakítva.", "Stopped because of TX.")); return; }
        run_lines();
    }
}

/* --- felület --- */
void sstv_cmd(const char *what, int v) {
    pthread_mutex_lock(&L);
    if (!strcmp(what, "on")) {
        enabled = v != 0;
        if (!v) stop_req = 1;
        snprintf(st_msg, sizeof st_msg, "%s", v ? TR("VIS-jelre várok.", "Waiting for a VIS header.") : "");
    } else if (!strcmp(what, "go")) manual_req = v;
    else if (!strcmp(what, "stop")) stop_req = 1;
    int want = enabled;
    int ch = want != armed_want;
    armed_want = want;
    pthread_mutex_unlock(&L);
    if (ch) audio_want(want ? 1 : -1);
    rig_bump_only();
}

void sstv_state(sb_t *b) {
    pthread_mutex_lock(&L);
    sb_add(b, "\"sstv\":{\"on\":%d,\"active\":%d,\"id\":%d,\"line\":%d,\"w\":%d,\"h\":%d,\"mode\":\"%s\",\"msg\":\"%s\"},",
           enabled, active, img_id, img_lines, img_w, img_h, st_mode, st_msg);
    pthread_mutex_unlock(&L);
}

/* A következő kész sor az ügyfélnek: [kép, sor, adat]. Hossz, vagy 0 */
int sstv_line(int *id, int *ln, uint8_t *out, size_t cap) {
    int n = 0;
    pthread_mutex_lock(&L);
    if (*id != img_id) { *id = img_id; *ln = 0; }
    if (*ln < img_lines && (size_t)img_w * 3 + 4 <= cap) {
        out[0] = (uint8_t)(img_id >> 8); out[1] = (uint8_t)img_id;
        out[2] = (uint8_t)(*ln >> 8); out[3] = (uint8_t)*ln;
        memcpy(out + 4, img[*ln], (size_t)img_w * 3);
        n = img_w * 3 + 4;
        (*ln)++;
    }
    pthread_mutex_unlock(&L);
    return n;
}

void sstv_list(sb_t *b) {
    char *l[256];
    int n = list_imgs(l, 256);
    sb_add(b, "{\"modes\":[");
    for (int i = 0; i < N_MODES; i++) sb_add(b, "%s\"%s\"", i ? "," : "", MODES[i].name);
    sb_add(b, "],\"images\":[");
    for (int i = 0; i < n; i++) {
        sb_add(b, "%s\"%s\"", i ? "," : "", l[i]);
        free(l[i]);
    }
    sb_add(b, "]}");
}

/* Csak saját mentett kép olvasható ki */
char *sstv_read(const char *name, size_t *len) {
    char dir[320], path[420];
    size_t l = strlen(name);
    if (l < 5 || l > 60 || strcmp(name + l - 4, ".png") || name[0] == '.') return NULL;
    for (const char *p = name; *p; p++)
        if (*p == '/' || *p == '\\' || (unsigned char)*p <= 0x20) return NULL;
    img_dir(dir, sizeof dir);
    snprintf(path, sizeof path, "%s/%s", dir, name);
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *b = sz > 0 && sz < 8000000 ? malloc((size_t)sz) : NULL;
    if (b && fread(b, 1, (size_t)sz, f) != (size_t)sz) { free(b); b = NULL; }
    fclose(f);
    *len = (size_t)sz;
    return b;
}
