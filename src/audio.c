/* Vízesés: az arecord hangját FFT-vel bontja, a böngésző csak az eredményt kapja.
   Az arecord külön folyamat, így a programnak nincs ALSA-függősége. */
#include "ft891ctl.h"
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#else
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>
#endif

#define RATE 48000
#define NFFT 8192
#define HOP  4000      /* 12 kép másodpercenként */
#define DEC  6         /* 48 kHz-ről 8 kHz-re */
#define FIRN 95
#define RX_RING 16

static pthread_mutex_t A = PTHREAD_MUTEX_INITIALIZER;
static int want;
static unsigned frame_seq;
static uint8_t frame[WF_BINS];
static char status_txt[160];
static int running;
static uint8_t rx_ring[RX_RING][RX_CHUNK];
static unsigned rx_seq;

static void feed(const int16_t *buf, size_t frames, int channels);
static void capture(const char *dev, int channels);

static void set_status(const char *s) {
    pthread_mutex_lock(&A);
    if (strcmp(status_txt, s)) { snprintf(status_txt, sizeof status_txt, "%s", s); pthread_mutex_unlock(&A); log_msg(TR("Vízesés: %s", "Waterfall: %s"), s); rig_bump_only(); return; }
    pthread_mutex_unlock(&A);
}

#ifndef _WIN32
/* adási hang */
static pid_t tx_pid;
static int tx_fd = -1, tx_owner = -1;

static const char *arecord_bin(void) { return g_cfg.arecord[0] ? g_cfg.arecord : "arecord"; }

/* Elindít egy parancsot, a kimenetét a visszaadott fájlleíróból lehet olvasni */
static pid_t spawn(char *const argv[], int *out, int *err) {
    int o[2], e[2] = {-1, -1};
    if (pipe2(o, 02000000)) return -1;
    if (err && pipe2(e, 02000000)) { close(o[0]); close(o[1]); return -1; }
    pid_t p = fork();
    if (p < 0) return -1;
    if (p == 0) {
        dup2(o[1], 1);
        dup2(err ? e[1] : o[1], 2);
        execvp(argv[0], argv);
        _exit(127);
    }
    close(o[1]);
    if (err) close(e[1]);
    *out = o[0];
    if (err) *err = e[0];
    return p;
}

void audio_list(sb_t *b) {
    char *argv[] = {(char *)arecord_bin(), (char *)"-L", NULL};
    int fd;
    sb_add(b, "[");
    pid_t p = spawn(argv, &fd, NULL);
    if (p > 0) {
        FILE *f = fdopen(fd, "r");
        char line[256], name[128] = "";
        int first = 1;
        while (f && fgets(line, sizeof line, f)) {
            line[strcspn(line, "\r\n")] = 0;
            if (line[0] != ' ' && line[0] != '\t') { snprintf(name, sizeof name, "%.127s", line); continue; }
            if (!name[0]) continue;
            if (strncmp(name, "plughw:", 7) && strncmp(name, "dsnoop:", 7) && strncmp(name, "default", 7) &&
                strncmp(name, "pipewire", 8) && strncmp(name, "pulse", 5)) { name[0] = 0; continue; }
            char *d = line;
            while (*d == ' ' || *d == '\t') d++;
            for (char *q = d; *q; q++) if (*q == '"' || *q == '\\') *q = ' ';
            sb_add(b, "%s{\"name\":\"%s\",\"desc\":\"%.100s\"}", first ? "" : ",", name, d);
            first = 0;
            name[0] = 0;
        }
        if (f) fclose(f);
        waitpid(p, NULL, 0);
    }
    sb_add(b, "]");
}
#endif

/* Csak a felsorolt eszközök közül lehet választani */
int audio_set_dev(const char *dev) {
    sb_t b = {0};
    char q[140];
    audio_list(&b);
    snprintf(q, sizeof q, "\"name\":\"%.128s\"", dev);
    int ok = b.p && strstr(b.p, q);
    free(b.p);
    if (!ok) return -1;
    rig_cfg_set_dev(dev);
    pthread_mutex_lock(&A);
    running = 0;              /* az új eszközzel indul újra */
    pthread_mutex_unlock(&A);
    return 0;
}

/* G.711 mu-law */
static uint8_t mulaw(int s) {
    int sign = 0;
    if (s < 0) { s = -s; sign = 0x80; }
    if (s > 32635) s = 32635;
    s += 132;
    int e = 7;
    for (int m = 0x4000; (s & m) == 0 && e > 0; e--, m >>= 1) {}
    return (uint8_t)~(sign | (e << 4) | ((s >> (e + 3)) & 15));
}

/* 3,6 kHz-es aluláteresztő, majd minden hatodik minta megy tovább */
static void rx_push(float x) {
    static float h[FIRN], line[FIRN];
    static int ready, lp, dc, cn;
    static uint8_t chunk[RX_CHUNK];
    if (!ready) {
        float sum = 0;
        for (int i = 0; i < FIRN; i++) {
            double n = i - (FIRN - 1) / 2.0, fc = 3600.0 / RATE;
            double v = n == 0 ? 2 * fc : sin(2 * M_PI * fc * n) / (M_PI * n);
            h[i] = (float)(v * (0.54 - 0.46 * cos(2 * M_PI * i / (FIRN - 1))));
            sum += h[i];
        }
        for (int i = 0; i < FIRN; i++) h[i] /= sum;
        ready = 1;
    }
    line[lp] = x;
    lp = (lp + 1) % FIRN;
    if (++dc < DEC) return;
    dc = 0;
    float y = 0;
    for (int i = 0; i < FIRN; i++) y += h[i] * line[(lp + i) % FIRN];
    sstv_push(y);
    chunk[cn++] = mulaw((int)(y * 32767));
    if (cn == RX_CHUNK) {
        cn = 0;
        pthread_mutex_lock(&A);
        memcpy(rx_ring[rx_seq % RX_RING], chunk, RX_CHUNK);
        rx_seq++;
        pthread_mutex_unlock(&A);
    }
}

/* A következő hangcsomag; ha az ügyfél lemaradt, az újakhoz ugrik. 0, ha nincs új */
unsigned audio_rx(uint8_t *out, unsigned *cursor) {
    unsigned got = 0;
    pthread_mutex_lock(&A);
    if (rx_seq - *cursor > RX_RING - 2) *cursor = rx_seq > 2 ? rx_seq - 2 : 0;
    if (*cursor != rx_seq) {
        memcpy(out, rx_ring[*cursor % RX_RING], RX_CHUNK);
        (*cursor)++;
        got = 1;
    }
    pthread_mutex_unlock(&A);
    return got;
}

#ifndef _WIN32
static void tx_close_locked(void) {
    if (tx_fd >= 0) close(tx_fd);
    tx_fd = -1;
    tx_owner = -1;
    if (tx_pid > 0) {
        kill(tx_pid, SIGTERM);
        waitpid(tx_pid, NULL, 0);
    }
    tx_pid = 0;
}

/* A hang csak PTT alatt megy a hangkártyára, ezért az aplay is csak addig fut */
int audio_tx_begin(int owner) {
    char dev[128], out[128], *argv[13];
    int x, y, z, p[2];
    rig_cfg_get(&x, &y, &z, dev, sizeof dev);
    if (!dev[0]) { rig_notice(1, TR("Válaszd ki a hangkártyát a Beállításban.", "Choose the sound card in Settings.")); return -1; }
    if (!strncmp(dev, "dsnoop:", 7)) snprintf(out, sizeof out, "dmix:%.120s", dev + 7);
    else snprintf(out, sizeof out, "%s", dev);
    pthread_mutex_lock(&A);
    if (tx_fd >= 0) { pthread_mutex_unlock(&A); return tx_owner == owner ? 0 : -1; }
    if (pipe2(p, 02000000)) { pthread_mutex_unlock(&A); return -1; }
    int i = 0;
    argv[i++] = (char *)(g_cfg.aplay[0] ? g_cfg.aplay : "aplay");
    argv[i++] = (char *)"-q"; argv[i++] = (char *)"-D"; argv[i++] = out;
    argv[i++] = (char *)"-f"; argv[i++] = (char *)"MU_LAW"; argv[i++] = (char *)"-r"; argv[i++] = (char *)"8000";
    argv[i++] = (char *)"-c"; argv[i++] = (char *)"1"; argv[i++] = (char *)"-t"; argv[i++] = (char *)"raw";
    argv[i] = NULL;
    pid_t c = fork();
    if (c == 0) {
        dup2(p[0], 0);
        execvp(argv[0], argv);
        _exit(127);
    }
    close(p[0]);
    if (c < 0) { close(p[1]); pthread_mutex_unlock(&A); return -1; }
    fcntl(p[1], F_SETFL, O_NONBLOCK);
    tx_pid = c;
    tx_fd = p[1];
    tx_owner = owner;
    pthread_mutex_unlock(&A);
    return 0;
}

void audio_tx_end(int owner) {
    pthread_mutex_lock(&A);
    if (tx_owner == owner) tx_close_locked();
    pthread_mutex_unlock(&A);
}

void audio_tx_write(int owner, const uint8_t *d, size_t n) {
    pthread_mutex_lock(&A);
    if (tx_owner == owner && tx_fd >= 0 && write(tx_fd, d, n) < 0 && errno == EPIPE) {
        tx_close_locked();
        pthread_mutex_unlock(&A);
        rig_notice(1, TR("A hangkártyára nem lehet írni. Nézd meg a kiválasztott eszközt.", "Cannot write to the sound card. Check the selected device."));
        return;
    }
    pthread_mutex_unlock(&A);
}
#endif

static void fft(float *re, float *im) {
    static float wr[NFFT / 2], wi[NFFT / 2];
    static int ready;
    if (!ready) {
        for (int i = 0; i < NFFT / 2; i++) {
            wr[i] = (float)cos(2 * M_PI * i / NFFT);
            wi[i] = (float)-sin(2 * M_PI * i / NFFT);
        }
        ready = 1;
    }
    for (int i = 1, j = 0; i < NFFT; i++) {
        int bit = NFFT >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) {
            float t = re[i]; re[i] = re[j]; re[j] = t;
            t = im[i]; im[i] = im[j]; im[j] = t;
        }
    }
    for (int len = 2; len <= NFFT; len <<= 1) {
        int st = NFFT / len;
        for (int i = 0; i < NFFT; i += len)
            for (int k = 0; k < len / 2; k++) {
                float a = wr[k * st], c = wi[k * st];
                int u = i + k, v = u + len / 2;
                float xr = re[v] * a - im[v] * c, xi = re[v] * c + im[v] * a;
                re[v] = re[u] - xr; im[v] = im[u] - xi;
                re[u] += xr; im[u] += xi;
            }
    }
}

static void analyse(const float *ring, int pos) {
    static float win[NFFT], re[NFFT], im[NFFT];
    static int ready;
    uint8_t out[WF_BINS];
    if (!ready) {
        for (int i = 0; i < NFFT; i++) win[i] = 0.5f - 0.5f * (float)cos(2 * M_PI * i / (NFFT - 1));
        ready = 1;
    }
    for (int i = 0; i < NFFT; i++) { re[i] = ring[(pos + i) % NFFT] * win[i]; im[i] = 0; }
    fft(re, im);
    /* WF_BINS oszlop 0-3000 Hz között; egy FFT-bin 5,86 Hz */
    for (int i = 0; i < WF_BINS; i++) {
        float p = re[i + 1] * re[i + 1] + im[i + 1] * im[i + 1];
        float db = 10 * log10f(p / (NFFT * NFFT / 16.0f) + 1e-14f);  /* 0 dB: teljes kivezérlésű szinusz */
        int v = (int)((db + 120) * 2);
        out[i] = (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
    }
    pthread_mutex_lock(&A);
    memcpy(frame, out, WF_BINS);
    frame_seq++;
    pthread_mutex_unlock(&A);
}

static float ring[NFFT];
static int ring_pos, ring_hop;
static long long fed;

/* A hangkártya mintái: vízesés, vételi hang és SSTV */
static void feed(const int16_t *buf, size_t frames, int channels) {
    for (size_t i = 0; i < frames; i++) {
        ring[ring_pos] = buf[i * (size_t)channels] / 32768.0f;
        rx_push(ring[ring_pos]);
        ring_pos = (ring_pos + 1) % NFFT;
        if (++ring_hop >= HOP) {
            ring_hop = 0;
            analyse(ring, ring_pos);
        }
    }
    if (!fed && frames) set_status("");
    fed += (long long)frames;
}

#ifndef _WIN32
static void capture(const char *dev, int channels) {
    char ch[8], rate[16];
    snprintf(ch, sizeof ch, "%d", channels);
    snprintf(rate, sizeof rate, "%d", RATE);
    char *argv[] = {(char *)arecord_bin(), (char *)"-q", (char *)"-D", (char *)dev, (char *)"-f", (char *)"S16_LE",
                    (char *)"-r", rate, (char *)"-c", ch, (char *)"-t", (char *)"raw", NULL};
    int fd, efd;
    pid_t p = spawn(argv, &fd, &efd);
    if (p < 0) { set_status(TR("Az arecord nem indítható.", "Cannot start arecord.")); return; }
    int16_t buf[2048];
    size_t have = 0;
    char err[200] = "";
    size_t en = 0;
    pthread_mutex_lock(&A);
    running = 1;
    pthread_mutex_unlock(&A);
    fed = 0;
    while (!g_stop) {
        pthread_mutex_lock(&A);
        int go = want > 0 && running;
        pthread_mutex_unlock(&A);
        if (!go) break;
        struct pollfd f[2] = {{fd, POLLIN, 0}, {efd, POLLIN, 0}};
        int r = poll(f, 2, 200);
        if (r < 0 && errno != EINTR) break;
        if (f[1].revents & POLLIN) {
            ssize_t n = read(efd, err + en, sizeof err - 1 - en);
            if (n > 0) { en += (size_t)n; err[en] = 0; }
            else f[1].fd = -1;
        }
        if (!(f[0].revents & (POLLIN | POLLHUP))) continue;
        ssize_t n = read(fd, (char *)buf + have, sizeof buf - have);
        if (n <= 0) break;
        have += (size_t)n;
        size_t frames = have / (sizeof(int16_t) * (size_t)channels);
        feed(buf, frames, channels);
        size_t used = frames * sizeof(int16_t) * (size_t)channels;
        memmove(buf, (char *)buf + used, have - used);
        have -= used;
    }
    kill(p, SIGTERM);
    close(fd);
    int st = 0;
    if (en == 0 && read(efd, err, sizeof err - 1) > 0) en = strlen(err);
    close(efd);
    waitpid(p, &st, 0);
    pthread_mutex_lock(&A);
    int wanted = want > 0 && running;
    running = 0;
    pthread_mutex_unlock(&A);
    if (wanted && !g_stop && !fed) {
        char m[160];
        if (WIFEXITED(st) && WEXITSTATUS(st) == 127) snprintf(m, sizeof m, TR("Nincs telepítve az arecord. Futtasd: sudo apt install alsa-utils", "arecord is not installed. Run: sudo apt install alsa-utils"));
        else if (strstr(err, "busy")) snprintf(m, sizeof m, TR("A hangkártyát másik program használja. Válassz dsnoop eszközt a Beállításban.", "Another program is using the sound card. Choose a dsnoop device in Settings."));
        else snprintf(m, sizeof m, TR("A hangkártya nem nyitható meg: %.100s", "Cannot open the sound card: %.100s"), dev);
        utf8_trim(m);
        set_status(m);
    } else if (wanted && !g_stop) set_status(TR("A hangkártya kapcsolata megszakadt.", "The sound card connection was lost."));
}
#endif

static void *audio_main(void *arg) {
    (void)arg;
    int channels = 1;
    while (!g_stop) {
        pthread_mutex_lock(&A);
        int go = want > 0;
        pthread_mutex_unlock(&A);
        char dev[128];
        int x, y, z;
        rig_cfg_get(&x, &y, &z, dev, sizeof dev);
        if (!go || !dev[0]) {
            if (go) set_status(TR("Válaszd ki a hangkártyát a Beállításban.", "Choose the sound card in Settings."));
            sleep_ms(200);
            continue;
        }
        long long t0 = now_ms();
        capture(dev, channels);
        /* Egyes kártyák csak sztereót tudnak */
        if (now_ms() - t0 < 1000) { channels = channels == 1 ? 2 : 1; sleep_ms(channels == 2 ? 100 : 1500); }
    }
    return NULL;
}

void audio_start(void) {
    pthread_t t;
    pthread_create(&t, NULL, audio_main, NULL);
    pthread_detach(t);
}

void audio_want(int delta) {
    pthread_mutex_lock(&A);
    want += delta;
    pthread_mutex_unlock(&A);
}

unsigned audio_frame(uint8_t *out, unsigned last) {
    pthread_mutex_lock(&A);
    unsigned s = frame_seq;
    if (s != last) memcpy(out, frame, WF_BINS);
    pthread_mutex_unlock(&A);
    return s;
}

void audio_status(char *out, size_t cap) {
    pthread_mutex_lock(&A);
    snprintf(out, cap, "%s", status_txt);
    pthread_mutex_unlock(&A);
}


#ifdef _WIN32
/* ---------------------------------------------------- Windows: waveIn/waveOut */
#define WBUFS   8
#define WFRAMES 1920        /* 40 ms 48 kHz-en */
#define TXBUFS  32
#define TXMAX   2048        /* minta egy adási pufferben */

/* Az eszköz neve a Windows által mutatott név; JSON-ba tehetővé tesszük */
static void clean_name(const char *in, char *out, size_t cap) {
    size_t k = 0;
    for (; *in && k + 1 < cap; in++) out[k++] = (*in == '"' || *in == '\\' || (unsigned char)*in < 32) ? ' ' : *in;
    out[k] = 0;
}

static int find_in(const char *dev) {
    UINT n = waveInGetNumDevs();
    for (UINT i = 0; i < n; i++) {
        WAVEINCAPSA c;
        char nm[64];
        if (waveInGetDevCapsA(i, &c, sizeof c) != MMSYSERR_NOERROR) continue;
        clean_name(c.szPname, nm, sizeof nm);
        if (!strcmp(nm, dev)) return (int)i;
    }
    return -1;
}

/* A bemenet párja: a zárójelben álló eszköznév (pl. "USB Audio CODEC") egyezik */
static int find_out(const char *dev) {
    char key[64];
    const char *o = strchr(dev, '(');
    snprintf(key, sizeof key, "%s", o ? o + 1 : dev);
    key[strcspn(key, ")")] = 0;
    while (*key && key[strlen(key) - 1] == ' ') key[strlen(key) - 1] = 0;
    UINT n = waveOutGetNumDevs();
    for (UINT i = 0; i < n; i++) {
        WAVEOUTCAPSA c;
        char nm[64];
        if (waveOutGetDevCapsA(i, &c, sizeof c) != MMSYSERR_NOERROR) continue;
        clean_name(c.szPname, nm, sizeof nm);
        if (*key && strstr(nm, key)) return (int)i;
    }
    return -1;
}

void audio_list(sb_t *b) {
    UINT n = waveInGetNumDevs();
    int first = 1;
    sb_add(b, "[");
    for (UINT i = 0; i < n; i++) {
        WAVEINCAPSA c;
        char nm[64];
        if (waveInGetDevCapsA(i, &c, sizeof c) != MMSYSERR_NOERROR) continue;
        clean_name(c.szPname, nm, sizeof nm);
        sb_add(b, "%s{\"name\":\"%s\",\"desc\":\"%s\"}", first ? "" : ",", nm,
               find_out(nm) >= 0 ? TR("vétel és adás", "receive and transmit") : TR("csak vétel", "receive only"));
        first = 0;
    }
    sb_add(b, "]");
}

static void capture(const char *dev, int channels) {
    int id = find_in(dev);
    if (id < 0) { set_status(TR("A kiválasztott hangkártya nem található.", "The selected sound card is not found.")); sleep_ms(1000); return; }
    HANDLE ev = CreateEventA(NULL, FALSE, FALSE, NULL);
    WAVEFORMATEX f;
    memset(&f, 0, sizeof f);
    f.wFormatTag = WAVE_FORMAT_PCM;
    f.nChannels = (WORD)channels;
    f.nSamplesPerSec = RATE;
    f.wBitsPerSample = 16;
    f.nBlockAlign = (WORD)(2 * channels);
    f.nAvgBytesPerSec = RATE * f.nBlockAlign;
    HWAVEIN h;
    if (!ev || waveInOpen(&h, (UINT)id, &f, (DWORD_PTR)ev, 0, CALLBACK_EVENT) != MMSYSERR_NOERROR) {
        if (ev) CloseHandle(ev);
        char m[160];
        snprintf(m, sizeof m, TR("A hangkártya nem nyitható meg: %.100s", "Cannot open the sound card: %.100s"), dev);
        utf8_trim(m);
        set_status(m);
        return;
    }
    static int16_t data[WBUFS][WFRAMES * 2];
    WAVEHDR hd[WBUFS];
    for (int i = 0; i < WBUFS; i++) {
        memset(&hd[i], 0, sizeof hd[i]);
        hd[i].lpData = (LPSTR)data[i];
        hd[i].dwBufferLength = (DWORD)(WFRAMES * f.nBlockAlign);
        waveInPrepareHeader(h, &hd[i], sizeof hd[i]);
        waveInAddBuffer(h, &hd[i], sizeof hd[i]);
    }
    pthread_mutex_lock(&A);
    running = 1;
    pthread_mutex_unlock(&A);
    fed = 0;
    waveInStart(h);
    long long last = now_ms();
    int lost = 0;
    while (!g_stop) {
        pthread_mutex_lock(&A);
        int go = want > 0 && running;
        pthread_mutex_unlock(&A);
        if (!go) break;
        WaitForSingleObject(ev, 200);
        for (int i = 0; i < WBUFS; i++) {
            if (!(hd[i].dwFlags & WHDR_DONE)) continue;
            feed((const int16_t *)hd[i].lpData, hd[i].dwBytesRecorded / f.nBlockAlign, channels);
            hd[i].dwFlags &= ~(DWORD)WHDR_DONE;
            hd[i].dwBytesRecorded = 0;
            if (waveInAddBuffer(h, &hd[i], sizeof hd[i]) != MMSYSERR_NOERROR) lost = 1;
            last = now_ms();
        }
        if (lost || now_ms() - last > 2000) { lost = 1; break; }   /* kihúzták vagy elakadt */
    }
    waveInReset(h);
    for (int i = 0; i < WBUFS; i++) waveInUnprepareHeader(h, &hd[i], sizeof hd[i]);
    waveInClose(h);
    CloseHandle(ev);
    pthread_mutex_lock(&A);
    int wanted = want > 0 && running;
    running = 0;
    pthread_mutex_unlock(&A);
    if (wanted && !g_stop && (lost || !fed)) set_status(TR("A hangkártya kapcsolata megszakadt.", "The sound card connection was lost."));
}

/* --- adás: a böngésző 8 kHz-es mu-law hangja PCM-ként megy a hangkártyára --- */
static HWAVEOUT tx_h;
static int tx_owner = -1;
static WAVEHDR tx_hd[TXBUFS];
static int16_t tx_data[TXBUFS][TXMAX];

static int16_t unmulaw(uint8_t u) {
    u = (uint8_t)~u;
    int t = ((u & 15) << 3) + 132;
    t <<= (u >> 4) & 7;
    return (int16_t)((u & 0x80) ? 132 - t : t - 132);
}

static void tx_close_locked(void) {
    if (tx_h) {
        waveOutReset(tx_h);
        for (int i = 0; i < TXBUFS; i++)
            if (tx_hd[i].dwFlags & WHDR_PREPARED) waveOutUnprepareHeader(tx_h, &tx_hd[i], sizeof tx_hd[i]);
        waveOutClose(tx_h);
    }
    tx_h = NULL;
    tx_owner = -1;
}

int audio_tx_begin(int owner) {
    char dev[128];
    int x, y, z;
    rig_cfg_get(&x, &y, &z, dev, sizeof dev);
    if (!dev[0]) { rig_notice(1, TR("Válaszd ki a hangkártyát a Beállításban.", "Choose the sound card in Settings.")); return -1; }
    pthread_mutex_lock(&A);
    if (tx_h) { pthread_mutex_unlock(&A); return tx_owner == owner ? 0 : -1; }
    int id = find_out(dev);
    WAVEFORMATEX f;
    memset(&f, 0, sizeof f);
    f.wFormatTag = WAVE_FORMAT_PCM;
    f.nChannels = 1;
    f.nSamplesPerSec = 8000;
    f.wBitsPerSample = 16;
    f.nBlockAlign = 2;
    f.nAvgBytesPerSec = 16000;
    if (id < 0 || waveOutOpen(&tx_h, (UINT)id, &f, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR) {
        tx_h = NULL;
        pthread_mutex_unlock(&A);
        rig_notice(1, TR("A hangkártya kimenete nem nyitható meg. Nézd meg a kiválasztott eszközt.", "Cannot open the sound card output. Check the selected device."));
        return -1;
    }
    memset(tx_hd, 0, sizeof tx_hd);
    tx_owner = owner;
    pthread_mutex_unlock(&A);
    return 0;
}

void audio_tx_end(int owner) {
    pthread_mutex_lock(&A);
    if (tx_owner == owner) tx_close_locked();
    pthread_mutex_unlock(&A);
}

void audio_tx_write(int owner, const uint8_t *d, size_t n) {
    pthread_mutex_lock(&A);
    while (tx_owner == owner && tx_h && n) {
        int slot = -1;
        for (int i = 0; i < TXBUFS && slot < 0; i++) {
            WAVEHDR *w = &tx_hd[i];
            if (!(w->dwFlags & WHDR_PREPARED)) slot = i;
            else if (w->dwFlags & WHDR_DONE) { waveOutUnprepareHeader(tx_h, w, sizeof *w); slot = i; }
        }
        if (slot < 0) break;          /* túl sok vár lejátszásra: eldobjuk, hogy ne késsen */
        size_t k = n < TXMAX ? n : TXMAX;
        for (size_t i = 0; i < k; i++) tx_data[slot][i] = unmulaw(d[i]);
        WAVEHDR *w = &tx_hd[slot];
        memset(w, 0, sizeof *w);
        w->lpData = (LPSTR)tx_data[slot];
        w->dwBufferLength = (DWORD)(k * 2);
        if (waveOutPrepareHeader(tx_h, w, sizeof *w) != MMSYSERR_NOERROR ||
            waveOutWrite(tx_h, w, sizeof *w) != MMSYSERR_NOERROR) {
            tx_close_locked();
            pthread_mutex_unlock(&A);
            rig_notice(1, TR("A hangkártyára nem lehet írni. Nézd meg a kiválasztott eszközt.", "Cannot write to the sound card. Check the selected device."));
            return;
        }
        d += k;
        n -= k;
    }
    pthread_mutex_unlock(&A);
}
#endif
