/* Linux és Windows közötti különbségek egy helyen */
#ifndef COMPAT_H
#define COMPAT_H
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <time.h>
#include <sys/stat.h>

/* A MinGW-ben a "printf" a Microsoft-féle formátumot jelenti */
#ifdef _WIN32
#define PRINTF_FMT gnu_printf
#else
#define PRINTF_FMT printf
#endif

/* --- soros port (serial.c) --- */
#define SER_RTS 1
#define SER_DTR 2
typedef struct { char path[256]; int trusted; } ser_cand_t;
int  ser_open(const char *path, int baud, int rts, intptr_t *h);   /* 0 vagy -errno */
void ser_close(intptr_t h);
void ser_flush(intptr_t h);
void ser_drain(intptr_t h);
int  ser_line(intptr_t h, int which, int on);
int  ser_write(intptr_t h, const char *b, size_t n);               /* 0 vagy -errno */
int  ser_read(intptr_t h, char *b, size_t cap, int timeout_ms, int use_wake); /* >0, 0 idő/ébresztés, -1 hiba */
void ser_wake_init(void);
void ser_wake(void);
void ser_wake_drain(void);
void ser_wake_wait(int ms);
int  ser_scan(ser_cand_t *c, int max);
int  ser_pair(const char *port, char *out, size_t cap);

/* --- rendszer (compat.c) --- */
void sys_init(void);                      /* Windowson: Winsock, UTF-8 konzol */
int  sys_lang_hu(void);                   /* a rendszer nyelve magyar-e */
void sys_default_cfg_dir(char *out, size_t cap);
int  sys_local_ipv4(char (*out)[64], int max);  /* a gép nem 127.x címei */
int  rename_replace(const char *from, const char *to);
void sleep_ms(int ms);

#ifdef _WIN32
#include <direct.h>
#include <io.h>
#define mkdir(p, m) _mkdir(p)
#undef localtime_r
#define localtime_r(t, m) (localtime_s((m), (t)) ? NULL : (m))
void *memmem(const void *h, size_t hn, const void *n, size_t nn);
#endif
#endif
