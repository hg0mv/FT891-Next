/* Napló, beállításfájl, SHA-1, base64 */
#include "ft891ctl.h"
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

config_t g_cfg;
_Atomic int g_stop;

static pthread_mutex_t log_m = PTHREAD_MUTEX_INITIALIZER;
static char log_ring[LOG_LINES][180];
static unsigned log_total;
static FILE *log_f;

long long now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (long long)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

static void mkdirs(const char *path) {
    char tmp[256];
    snprintf(tmp, sizeof tmp, "%s", path);
    for (char *p = tmp + 1; *p; p++)
        if (*p == '/') { *p = 0; mkdir(tmp, 0700); *p = '/'; }
    mkdir(tmp, 0700);
}

void log_init(void) {
    char path[300];
    mkdirs(g_cfg.cfg_dir);
    snprintf(path, sizeof path, "%.280s/ft891ctl.log", g_cfg.cfg_dir);
    log_f = fopen(path, "a");
    if (log_f) setvbuf(log_f, NULL, _IOLBF, 0);
}

/* A levágott szöveg végéről eltávolítja a csonka UTF-8 karaktert:
   érvénytelen szöveg miatt a böngésző bontaná a WebSocket-kapcsolatot */
void utf8_trim(char *s) {
    size_t n = strlen(s), i = n;
    while (i > 0 && ((unsigned char)s[i - 1] & 0xC0) == 0x80) i--;
    if (i == 0) return;
    unsigned char c = (unsigned char)s[i - 1];
    size_t need = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1;
    if (i - 1 + need > n) s[i - 1] = 0;
}

static void log_store(const char *line) {
    char ts[16];
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    strftime(ts, sizeof ts, "%H:%M:%S", &tm);
    pthread_mutex_lock(&log_m);
    snprintf(log_ring[log_total % LOG_LINES], sizeof log_ring[0], "%s %s", ts, line);
    utf8_trim(log_ring[log_total % LOG_LINES]);
    log_total++;
    pthread_mutex_unlock(&log_m);
    fprintf(stderr, "%s %s\n", ts, line);
    if (log_f) fprintf(log_f, "%s %s\n", ts, line);
}

void log_msg(const char *fmt, ...) {
    char b[150];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    utf8_trim(b);
    log_store(b);
}

void log_trace(const char *dir, const char *frame) {
    if (g_cfg.trace) fprintf(stderr, "CAT %s %s\n", dir, frame);
    if (g_cfg.trace && log_f) fprintf(log_f, "CAT %s %s\n", dir, frame);
}

unsigned log_seq(void) {
    pthread_mutex_lock(&log_m);
    unsigned v = log_total;
    pthread_mutex_unlock(&log_m);
    return v;
}

static size_t json_str(char *o, size_t cap, const char *s) {
    size_t n = 0;
    if (cap < 3) return 0;
    o[n++] = '"';
    for (; *s && n + 8 < cap; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') { o[n++] = '\\'; o[n++] = (char)c; }
        else if (c < 0x20) n += (size_t)snprintf(o + n, cap - n, "\\u%04x", c);
        else o[n++] = (char)c;
    }
    o[n++] = '"';
    o[n] = 0;
    return n;
}

int log_copy_json(char *out, size_t cap, unsigned *seq_out) {
    size_t n = 0;
    n += (size_t)snprintf(out, cap, "{\"t\":\"log\",\"l\":[");
    pthread_mutex_lock(&log_m);
    unsigned total = log_total;
    /* a legfrissebb sorok férjenek bele */
    unsigned first = total > LOG_SHOW ? total - LOG_SHOW : 0;
    for (unsigned i = first; i < total && n + 400 < cap; i++) {
        if (i != first) out[n++] = ',';
        n += json_str(out + n, cap - n, log_ring[i % LOG_LINES]);
    }
    pthread_mutex_unlock(&log_m);
    n += (size_t)snprintf(out + n, cap - n, "]}");
    if (seq_out) *seq_out = total;
    return (int)n;
}

int random_bytes(void *buf, size_t n) {
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0) return -1;
    ssize_t r = read(fd, buf, n);
    close(fd);
    return r == (ssize_t)n ? 0 : -1;
}

/* --- beállításfájl --- */
void config_load(void) {
    char path[300], line[300];
    snprintf(path, sizeof path, "%s/config", g_cfg.cfg_dir);
    FILE *f = fopen(path, "r");
    if (f) {
        while (fgets(line, sizeof line, f)) {
            char *eq = strchr(line, '=');
            if (!eq) continue;
            *eq = 0;
            char *v = eq + 1;
            v[strcspn(v, "\r\n")] = 0;
            if (!strcmp(line, "pin")) snprintf(g_cfg.pin, sizeof g_cfg.pin, "%s", v);
            else if (!strcmp(line, "ptt_method")) g_cfg.ptt_method = atoi(v);
            else if (!strcmp(line, "mode_lock")) g_cfg.mode_lock = atoi(v);
            else if (!strcmp(line, "tx_max_s")) g_cfg.tx_max_s = atoi(v);
            else if (!strcmp(line, "last_baud")) g_cfg.last_baud = atoi(v);
            else if (!strcmp(line, "lang")) g_cfg.lang_en = strcmp(v, "hu") != 0;
            else if (!strcmp(line, "audio_dev")) snprintf(g_cfg.audio_dev, sizeof g_cfg.audio_dev, "%s", v);
        }
        fclose(f);
    }
    if (!g_cfg.pin[0]) config_new_pin();
}

void config_new_pin(void) {
    uint32_t r = 0;
    if (random_bytes(&r, sizeof r)) { fprintf(stderr, "%s", TR("Nem olvasható /dev/urandom\n", "Cannot read /dev/urandom\n")); exit(1); }
    snprintf(g_cfg.pin, sizeof g_cfg.pin, "%06u", r % 1000000u);
    config_save();
}

void config_save(void) {
    char path[300], tmp[310];
    snprintf(path, sizeof path, "%s/config", g_cfg.cfg_dir);
    snprintf(tmp, sizeof tmp, "%s.new", path);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) { log_msg(TR("A beállítások nem menthetők: %s", "Cannot save settings: %s"), strerror(errno)); return; }
    FILE *f = fdopen(fd, "w");
    fprintf(f, "pin=%s\nlang=%s\nptt_method=%d\nmode_lock=%d\ntx_max_s=%d\nlast_baud=%d\naudio_dev=%s\n",
            g_cfg.pin, g_cfg.lang_en ? "en" : "hu", g_cfg.ptt_method, g_cfg.mode_lock, g_cfg.tx_max_s, g_cfg.last_baud, g_cfg.audio_dev);
    fclose(f);
    rename(tmp, path);
}

/* --- SHA-1 (a WebSocket kézfogáshoz) --- */
#define ROL(x, n) (((x) << (n)) | ((x) >> (32 - (n))))
void sha1(const uint8_t *data, size_t len, uint8_t out[20]) {
    uint32_t h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
    size_t total = ((len + 8) / 64 + 1) * 64;
    uint8_t *m = calloc(total, 1);
    if (!m) { memset(out, 0, 20); return; }
    memcpy(m, data, len);
    m[len] = 0x80;
    uint64_t bits = (uint64_t)len * 8;
    for (int i = 0; i < 8; i++) m[total - 1 - (size_t)i] = (uint8_t)(bits >> (8 * i));
    for (size_t o = 0; o < total; o += 64) {
        uint32_t w[80], a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 16; i++)
            w[i] = (uint32_t)m[o + (size_t)i * 4] << 24 | (uint32_t)m[o + (size_t)i * 4 + 1] << 16 |
                   (uint32_t)m[o + (size_t)i * 4 + 2] << 8 | m[o + (size_t)i * 4 + 3];
        for (int i = 16; i < 80; i++) w[i] = ROL(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        for (int i = 0; i < 80; i++) {
            uint32_t f, k;
            if (i < 20)      { f = (b & c) | (~b & d);          k = 0x5A827999; }
            else if (i < 40) { f = b ^ c ^ d;                   k = 0x6ED9EBA1; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
            else             { f = b ^ c ^ d;                   k = 0xCA62C1D6; }
            uint32_t t = ROL(a, 5) + f + e + k + w[i];
            e = d; d = c; c = ROL(b, 30); b = a; a = t;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
    }
    free(m);
    for (int i = 0; i < 5; i++)
        for (int j = 0; j < 4; j++) out[i * 4 + j] = (uint8_t)(h[i] >> (24 - 8 * j));
}

void base64(const uint8_t *in, size_t len, char *out) {
    static const char t[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t o = 0;
    for (size_t i = 0; i < len; i += 3) {
        uint32_t v = (uint32_t)in[i] << 16;
        if (i + 1 < len) v |= (uint32_t)in[i + 1] << 8;
        if (i + 2 < len) v |= in[i + 2];
        out[o++] = t[(v >> 18) & 63];
        out[o++] = t[(v >> 12) & 63];
        out[o++] = i + 1 < len ? t[(v >> 6) & 63] : '=';
        out[o++] = i + 2 < len ? t[v & 63] : '=';
    }
    out[o] = 0;
}
