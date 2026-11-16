/* Soros port és ébresztés: Linux (termios) és Windows (Win32 COM) */
#include "ft891ctl.h"
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Az általános szabály: a PTT-port a CAT-port utáni sorszám */
static int next_number(const char *port, char *out, size_t cap) {
    const char *d = port + strlen(port);
    while (d > port && isdigit((unsigned char)d[-1])) d--;
    if (!*d) return -1;
    snprintf(out, cap, "%.*s%d", (int)(d - port), port, atoi(d) + 1);
    return 0;
}

#ifndef _WIN32
/* ------------------------------------------------------------------ Linux */
#include <dirent.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

static int wake_p[2] = {-1, -1};

static speed_t baud_const(int b) {
    switch (b) {
    case 4800: return B4800;
    case 9600: return B9600;
    case 19200: return B19200;
    default: return B38400;
    }
}

int ser_open(const char *path, int baud, int rts, intptr_t *h) {
    int f = open(path, O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
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
    *h = f;
    return 0;
}

void ser_close(intptr_t h) { if (h >= 0) close((int)h); }
void ser_flush(intptr_t h) { tcflush((int)h, TCIOFLUSH); }
void ser_drain(intptr_t h) { tcdrain((int)h); }

int ser_line(intptr_t h, int which, int on) {
    int bits = (which & SER_RTS ? TIOCM_RTS : 0) | (which & SER_DTR ? TIOCM_DTR : 0);
    return ioctl((int)h, on ? TIOCMBIS : TIOCMBIC, &bits) < 0 ? -errno : 0;
}

int ser_write(intptr_t h, const char *b, size_t n) {
    size_t o = 0;
    long long end = now_ms() + 300;
    while (o < n) {
        ssize_t w = write((int)h, b + o, n - o);
        if (w > 0) { o += (size_t)w; continue; }
        if (w < 0 && errno != EAGAIN && errno != EINTR) return -errno;
        struct pollfd p = {(int)h, POLLOUT, 0};
        poll(&p, 1, 20);
        if (now_ms() > end) return -ETIMEDOUT;
    }
    return 0;
}

int ser_read(intptr_t h, char *b, size_t cap, int timeout_ms, int use_wake) {
    long long end = now_ms() + timeout_ms;
    for (;;) {
        long long left = end - now_ms();
        if (left <= 0) return 0;
        struct pollfd p[2] = {{(int)h, POLLIN, 0}, {wake_p[0], POLLIN, 0}};
        int r = poll(p, use_wake ? 2 : 1, (int)left);
        if (r < 0) { if (errno == EINTR) continue; return -1; }
        if (use_wake && (p[1].revents & POLLIN)) return 0;
        if (p[0].revents & (POLLERR | POLLNVAL)) return -1;
        if (p[0].revents & (POLLIN | POLLHUP)) {
            ssize_t n = read((int)h, b, cap);
            if (n > 0) return (int)n;
            if (n == 0 || (errno != EAGAIN && errno != EINTR)) return -1;
        }
    }
}

void ser_wake_init(void) {
    if (pipe2(wake_p, O_NONBLOCK | O_CLOEXEC)) { perror("pipe"); exit(1); }
}
void ser_wake(void) {
    if (write(wake_p[1], "x", 1) < 0) { /* tele van: úgyis felébred */ }
}
void ser_wake_drain(void) {
    char b[32];
    while (read(wake_p[0], b, sizeof b) > 0) {}
}
void ser_wake_wait(int ms) {
    struct pollfd p = {wake_p[0], POLLIN, 0};
    poll(&p, 1, ms);
}

static int cmp_path(const void *a, const void *b) { return strcmp(((const ser_cand_t *)a)->path, ((const ser_cand_t *)b)->path); }

int ser_scan(ser_cand_t *c, int max) {
    int n = 0;
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
        qsort(c, (size_t)n, sizeof *c, cmp_path);
    }
    return n;
}

int ser_pair(const char *port, char *out, size_t cap) {
    const char *p = strstr(port, "-if00");
    if (p) {                      /* by-id: a Standard port az if01 */
        snprintf(out, cap, "%.*s-if01%s", (int)(p - port), port, p + 5);
        return 0;
    }
    return next_number(port, out, cap);
}

#else
/* ---------------------------------------------------------------- Windows */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <setupapi.h>

static HANDLE wake_ev;

/* "COM12" -> "\\.\COM12"; a 9 fölötti portok csak így nyithatók */
static void win_path(const char *path, char *out, size_t cap) {
    if (!strncmp(path, "\\\\", 2)) snprintf(out, cap, "%s", path);
    else snprintf(out, cap, "\\\\.\\%s", path);
}

int ser_open(const char *path, int baud, int rts, intptr_t *h) {
    char p[300];
    win_path(path, p, sizeof p);
    HANDLE f = CreateFileA(p, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) {
        DWORD e = GetLastError();
        if (e == ERROR_ACCESS_DENIED || e == ERROR_SHARING_VIOLATION) return -EBUSY;
        if (e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND) return -ENOENT;
        return -EIO;
    }
    DCB d;
    memset(&d, 0, sizeof d);
    d.DCBlength = sizeof d;
    GetCommState(f, &d);
    d.BaudRate = (DWORD)baud;
    d.ByteSize = 8;
    d.Parity = NOPARITY;
    d.StopBits = TWOSTOPBITS;     /* 8N2 */
    d.fBinary = TRUE;
    d.fParity = FALSE;
    d.fOutxCtsFlow = FALSE;
    d.fOutxDsrFlow = FALSE;
    d.fDsrSensitivity = FALSE;
    d.fOutX = FALSE;
    d.fInX = FALSE;
    d.fNull = FALSE;
    d.fAbortOnError = FALSE;
    d.fDtrControl = DTR_CONTROL_DISABLE;
    d.fRtsControl = rts ? RTS_CONTROL_ENABLE : RTS_CONTROL_DISABLE;
    if (!SetCommState(f, &d)) { CloseHandle(f); return -EIO; }
    SetupComm(f, 4096, 4096);
    PurgeComm(f, PURGE_RXCLEAR | PURGE_TXCLEAR);
    *h = (intptr_t)f;
    return 0;
}

void ser_close(intptr_t h) { if (h != -1) CloseHandle((HANDLE)h); }
void ser_flush(intptr_t h) { PurgeComm((HANDLE)h, PURGE_RXCLEAR | PURGE_TXCLEAR); }
void ser_drain(intptr_t h) { FlushFileBuffers((HANDLE)h); }

int ser_line(intptr_t h, int which, int on) {
    int ok = 1;
    if (which & SER_RTS) ok &= EscapeCommFunction((HANDLE)h, on ? SETRTS : CLRRTS) != 0;
    if (which & SER_DTR) ok &= EscapeCommFunction((HANDLE)h, on ? SETDTR : CLRDTR) != 0;
    return ok ? 0 : -EIO;
}

static void set_timeouts(HANDLE f, DWORD read_ms) {
    COMMTIMEOUTS t;
    /* Ha van bejövő bájt, azonnal visszatér; különben legfeljebb read_ms-ig vár */
    t.ReadIntervalTimeout = MAXDWORD;
    t.ReadTotalTimeoutMultiplier = MAXDWORD;
    t.ReadTotalTimeoutConstant = read_ms ? read_ms : 1;
    t.WriteTotalTimeoutMultiplier = 0;
    t.WriteTotalTimeoutConstant = 300;
    SetCommTimeouts(f, &t);
}

int ser_write(intptr_t h, const char *b, size_t n) {
    HANDLE f = (HANDLE)h;
    DWORD w = 0;
    set_timeouts(f, 20);
    if (!WriteFile(f, b, (DWORD)n, &w, NULL)) return -EIO;
    return w == n ? 0 : -ETIMEDOUT;
}

int ser_read(intptr_t h, char *b, size_t cap, int timeout_ms, int use_wake) {
    HANDLE f = (HANDLE)h;
    long long end = now_ms() + timeout_ms;
    for (;;) {
        if (use_wake && WaitForSingleObject(wake_ev, 0) == WAIT_OBJECT_0) return 0;
        long long left = end - now_ms();
        if (left <= 0) return 0;
        /* rövid szeletekben várunk, hogy az ébresztést is észrevegyük */
        set_timeouts(f, (DWORD)(left > 15 ? 15 : left));
        DWORD got = 0;
        if (!ReadFile(f, b, (DWORD)cap, &got, NULL)) {
            DWORD err = 0;
            COMSTAT cs;
            /* hibajelzés (pl. keretezési hiba) után a portot újra olvashatóvá tesszük */
            if (ClearCommError(f, &err, &cs) && err) continue;
            return -1;
        }
        if (got > 0) return (int)got;
    }
}

void ser_wake_init(void) {
    wake_ev = CreateEventA(NULL, TRUE, FALSE, NULL);
    if (!wake_ev) { fprintf(stderr, "CreateEvent\n"); exit(1); }
}
void ser_wake(void) { SetEvent(wake_ev); }
void ser_wake_drain(void) { ResetEvent(wake_ev); }
void ser_wake_wait(int ms) { WaitForSingleObject(wake_ev, (DWORD)ms); }

/* A Portok (COM és LPT) eszközosztály */
static const GUID PORTS_GUID = {0x4D36E978, 0xE325, 0x11CE, {0xBF, 0xC1, 0x08, 0x00, 0x2B, 0xE1, 0x03, 0x18}};

typedef struct { char com[32]; char name[256]; } winport_t;

static int list_ports(winport_t *p, int max) {
    int n = 0;
    HDEVINFO di = SetupDiGetClassDevsA(&PORTS_GUID, NULL, NULL, DIGCF_PRESENT);
    if (di == INVALID_HANDLE_VALUE) return 0;
    SP_DEVINFO_DATA dd;
    dd.cbSize = sizeof dd;
    for (DWORD i = 0; n < max && SetupDiEnumDeviceInfo(di, i, &dd); i++) {
        char name[256] = "";
        if (!SetupDiGetDeviceRegistryPropertyA(di, &dd, SPDRP_FRIENDLYNAME, NULL, (BYTE *)name, sizeof name - 1, NULL)) continue;
        const char *c = strstr(name, "(COM");
        if (!c) continue;
        int k = 0;
        for (c++; *c && *c != ')' && k < 31; c++) p[n].com[k++] = *c;
        p[n].com[k] = 0;
        snprintf(p[n].name, sizeof p[n].name, "%s", name);
        n++;
    }
    SetupDiDestroyDeviceInfoList(di);
    return n;
}

static int cmp_com(const void *a, const void *b) {
    return atoi(((const ser_cand_t *)a)->path + 3) - atoi(((const ser_cand_t *)b)->path + 3);
}

int ser_scan(ser_cand_t *c, int max) {
    winport_t p[32];
    int np = list_ports(p, 32), n = 0;
    /* A CP2105 két portja közül az Enhanced a CAT */
    for (int i = 0; i < np && n < max; i++)
        if (strstr(p[i].name, "CP2105") && strstr(p[i].name, "Enhanced")) {
            snprintf(c[n].path, sizeof c[n].path, "%s", p[i].com);
            c[n++].trusted = 1;
        }
    if (n) return n;
    for (int i = 0; i < np && n < max; i++) {
        snprintf(c[n].path, sizeof c[n].path, "%s", p[i].com);
        c[n++].trusted = 0;
    }
    qsort(c, (size_t)n, sizeof *c, cmp_com);
    return n;
}

int ser_pair(const char *port, char *out, size_t cap) {
    winport_t p[32];
    int np = list_ports(p, 32);
    for (int i = 0; i < np; i++)
        if (strstr(p[i].name, "CP2105") && strstr(p[i].name, "Standard")) {
            snprintf(out, cap, "%s", p[i].com);
            return 0;
        }
    return next_number(port, out, cap);
}
#endif
