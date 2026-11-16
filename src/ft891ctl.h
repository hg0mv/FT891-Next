/* FT-891 vezérlő - közös definíciók */
#ifndef FT891CTL_H
#define FT891CTL_H
#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include "compat.h"

#define APP_VERSION "0.7-beta1"
#define RX_CHUNK 640    /* 80 ms hang: 8 kHz, mu-law */
#define WF_BINS 512
#define MENU_MAX 16

/* --- beállítások --- */
typedef struct {
    char cat_port[256];   /* üres: automatikus keresés */
    char ptt_port[256];   /* üres: a CAT-port párja */
    char web_bind[64];
    int  web_port;
    char rig_bind[64];
    int  rig_port;
    int  poll_ms;
    int  use_ai;
    int  trace;
    char cfg_dir[256];
    /* a configban tárolt, felületről is állítható értékek */
    char pin[32];
    int  ptt_method;      /* 0 CAT, 1 RTS, 2 DTR */
    int  mode_lock;
    int  tx_max_s;
    int  last_baud;
    char audio_dev[128];
    char arecord[200];
    char aplay[200];
    _Atomic int lang_en;  /* 1: angol felület és üzenetek */
    int  tls_port;
} config_t;
extern config_t g_cfg;
/* Kétnyelvű szöveg: a két változat formátuma azonos kell legyen */
#define TR(hu, en) (g_cfg.lang_en ? (en) : (hu))
void set_lang(int en);
extern _Atomic int g_stop;
void config_load(void);
void config_save(void);
void config_new_pin(void);

/* --- naplózás és segédfüggvények --- */
#define LOG_LINES 80
#define LOG_SHOW  25   /* ennyi sor megy ki a felületre */
void utf8_trim(char *s);
void log_init(void);
void log_msg(const char *fmt, ...) __attribute__((format(PRINTF_FMT, 1, 2)));
void log_trace(const char *dir, const char *frame);
int  log_copy_json(char *out, size_t cap, unsigned *seq_out);
unsigned log_seq(void);
long long now_ms(void);
void sha1(const uint8_t *data, size_t len, uint8_t out[20]);
void base64(const uint8_t *in, size_t len, char *out);
int  random_bytes(void *buf, size_t n);

/* --- módok --- */
enum { M_LSB, M_USB, M_SB_AUTO, M_CW, M_CWR, M_AM, M_AMN, M_FM, M_FMN,
       M_RTTY, M_RTTYR, M_PKTUSB, M_PKTLSB, M__COUNT };
enum { L_AF, L_RF, L_SQL, L_PWR, L_MIC, L_NRL, L_NBL, L_PROC, L__COUNT };
enum { FN_NR, FN_NB, FN_NOTCH, FN_AMP, FN_ATT, FN_PROC, FN_LOCK, FN_EQ, FN_COUNT };
enum { FT_IFS_ON, FT_IFS, FT_NOTCH_ON, FT_NOTCH, FT_CONT_ON, FT_CONT, FT_APF_ON, FT_APF, FT_COUNT };

typedef enum {
    OP_NONE, OP_FREQ_A, OP_FREQ_B, OP_MODE, OP_BAND, OP_LEVEL, OP_FUNC,
    OP_AGC, OP_WIDTH, OP_SWAP, OP_A2B, OP_B2A, OP_SPLIT, OP_CLAR,
    OP_CLAR_CLEAR, OP_CLAR_STEP, OP_PTT_METHOD, OP_PTT_TEST, OP_MENU_READ,
    OP_PTT_SYNC, OP_MENU_SWEEP, OP_MENU_APPLY, OP_MEM_READ, OP_MEM_WRITE, OP_MEM_RECALL, OP_FILT, OP_POWER
} op_t;

/* --- a rádió állapota --- */
typedef struct {
    unsigned seq;
    int  connected;
    char status[160];     /* magyar állapotüzenet */
    char port[256];
    char ptt_port[256];
    int  baud;
    int  rts_on;          /* a CAT-porton RTS magas */
    int  menu_ok;         /* a menütérkép a 2017-es CAT-könyvvel egyezik */
    char fw_main[8], fw_dsp[8], fw_lcd[8];

    long long freq_a, freq_b;
    char md;              /* az MD parancs kódja: '1'...'D' */
    int  bfo_ssb, bfo_cw, bfo_data, bfo_rtty; /* -1: nem ismert */
    int  mem_mode;        /* IF P7 */
    char channel[4];
    int  split, clar, clar_ofs;
    int  tx;              /* TX; válasz: 0, 1 CAT, 2 rádió */
    int  smeter, po, swr, alc;
    int  lvl[L__COUNT];
    int  fn[FN_COUNT];
    int  agc;
    int  width_idx, narrow;
    int  ft[FT_COUNT];
    int  pitch;

    int  keyed;           /* a program tartja adásban */
    int  ptt_drops;
    int  poll_cnt, timeouts;
    char job[40];         /* hosszabb művelet neve és állása */
    int  job_i, job_n;
    unsigned data_seq;    /* a menü- vagy memóriaadat változott */
    char menu_id[MENU_MAX][5];
    char menu_val[MENU_MAX][8];
    int  menu_n;
    unsigned notice_seq;
    char notice[200];
    int  notice_bad;
} rig_state_t;

void rig_start(void);
void rig_stop(void);
void rig_get(rig_state_t *out);
unsigned rig_seq(void);
/* Várakozás nélküli kérés (a webről jön, ugyanaz a beállítás összevonódik) */
void rig_post(op_t op, long long a, long long b);
/* Megvárja az eredményt; 0 ha sikerült, -1 ha nem */
int  rig_do(op_t op, long long a, long long b);
/* PTT-forrás tartása. A deadline 0: nincs határidő, különben ms-ban */
void rig_hold(int source, int on, int lease_ms);
void rig_set_mode_lock(int on);
void rig_set_tx_max(int seconds);
void rig_cfg_get(int *ptt, int *lock, int *txm, char *dev, size_t cap);
void rig_cfg_set_dev(const char *dev);
void rig_client_count(int delta);
void rig_notice(int bad, const char *fmt, ...) __attribute__((format(PRINTF_FMT, 2, 3)));

/* a jelenlegi mód nevei */
const char *rig_mode_name(const rig_state_t *s);    /* magyar felület: DATA-U */
const char *rig_hamlib_mode(const rig_state_t *s);
int  rig_lower_side(const rig_state_t *s);
const char *rig_adif_mode(const rig_state_t *s);
void rig_bump_only(void);  /* Hamlib: PKTUSB */
int  rig_width_hz(const rig_state_t *s);
int  rig_ssb_eff(int bfo, long long freq);          /* 0 USB, 1 LSB */

/* a CAT-szálon futó hosszabb műveletek segédei */
int  cat_q(const char *cmd, char *reply, size_t cap);
int  cat_s(const char *cmd);
int  cat_step(void);
int  rig_menu_ok(void);
int  rig_keyed(void);
int  rig_set_mode_now(int m);
void rig_menu_refresh(void);
void rig_job(const char *name, int i, int n);
void rig_bump(void);

/* menük, profilok, memóriák */
typedef struct { char *p; size_t n, cap; } sb_t;
void sb_add(sb_t *b, const char *fmt, ...) __attribute__((format(PRINTF_FMT, 2, 3)));
void menu_init(void);
void menu_on_connect(void);
void menu_cancel(void);
int  menu_sweep(void);
int  menu_apply(void);
int  menu_queue(char *text, const char *label);
int  menu_queue_set(int backup, const char *name);
int  profile_save(const char *name, const char *body);
int  profile_delete(const char *name);
int  mem_read(void);
int  mem_write(void);
int  mem_recall(int i);
int  mem_queue(char *text, char *err, size_t cap);
void menu_json(sb_t *b);
void sets_json(sb_t *b, int backup);
void mem_json(sb_t *b);

/* PTT-források */
#define SRC_RIG(i) (i)            /* 0..7  rigctld-ügyfelek */
#define SRC_WEB(i) (8 + (i))      /* 8..15 webes ügyfelek */
#define SRC_TEST   30

/* vízesés */
void audio_start(void);
void audio_want(int delta);
unsigned audio_frame(uint8_t *out, unsigned last);
void audio_status(char *out, size_t cap);
void audio_list(sb_t *b);
unsigned audio_rx(uint8_t *out, unsigned *cursor);
int  audio_tx_begin(int owner);
void audio_tx_end(int owner);
void audio_tx_write(int owner, const uint8_t *d, size_t n);
int  audio_set_dev(const char *dev);

/* SSTV */
void sstv_push(float y);
void sstv_cmd(const char *what, int v);
void sstv_state(sb_t *b);
int  sstv_line(int *id, int *ln, uint8_t *out, size_t cap);
void sstv_list(sb_t *b);
char *sstv_read(const char *name, size_t *len);

/* szerverek */
void rigsrv_start(void);
void web_start(void);
void web_print_urls(void);
#endif
