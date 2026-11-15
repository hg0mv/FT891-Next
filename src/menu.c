/* Menük, profilok, mentések és memóriacsatornák */
#include "ft891ctl.h"
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

enum { K_ENUM, K_NUM, K_SIGN, K_OFFSTEP };
typedef struct {
    const char *id, *name, *grp, *hu;
    int kind, digits, min, max, step;
    const char *unit, *labels;
    int prot;
    const char *grp_en, *en, *unit_en, *labels_en;
} mdef_t;

static const mdef_t D[] = {
#define M(...) {__VA_ARGS__},
#include "menus.def"
#undef M
};
#define N_MENU (int)(sizeof D / sizeof D[0])
#define N_MEM 117
#define KEEP_BACKUPS 20

typedef struct {
    char ch[4]; int present, tag_ok; long long f; char sign; int ofs, clar; char md; int tone, shift;
    char tag[13];
} mem_t;
typedef struct { char id[5], v[8]; } pend_t;

static pthread_mutex_t MM = PTHREAD_MUTEX_INITIALIZER;
static char V[N_MENU][8];
static int  swept, mem_read_ok;
static mem_t MEM[N_MEM], MP[N_MEM];
static pend_t P[N_MENU];
static int  pn, pmode = -1, busy, mpn;
static char plabel[80];

/* --- szöveg-építő --- */
void sb_add(sb_t *b, const char *fmt, ...) {
    for (;;) {
        va_list ap;
        size_t room = b->cap - b->n;
        va_start(ap, fmt);
        int r = b->p ? vsnprintf(b->p + b->n, room, fmt, ap) : -1;
        va_end(ap);
        if (r >= 0 && (size_t)r < room) { b->n += (size_t)r; return; }
        size_t nc = b->cap ? b->cap * 2 : 65536;
        char *np = realloc(b->p, nc);
        if (!np) return;
        b->p = np; b->cap = nc;
    }
}

static void sb_str(sb_t *b, const char *s) {
    sb_add(b, "\"");
    for (; *s; s++) {
        if (*s == '"' || *s == '\\') sb_add(b, "\\%c", *s);
        else if ((unsigned char)*s >= 0x20) sb_add(b, "%c", *s);
    }
    sb_add(b, "\"");
}

static int find(const char *id) {
    for (int i = 0; i < N_MENU; i++) if (!strcmp(D[i].id, id)) return i;
    return -1;
}

/* A rádió formátumát követjük: a hossz a beolvasott értékből jön, ha ismert */
static int valid(int i, const char *v, size_t curlen) {
    const mdef_t *d = &D[i];
    size_t n = strlen(v), want = curlen ? curlen : (size_t)d->digits;
    if (n != want || n >= sizeof V[0]) return 0;
    const char *p = v;
    if (d->kind == K_SIGN) {
        if (*p != '+' && *p != '-') return 0;
        p++;
    }
    if (!*p) return 0;
    for (const char *q = p; *q; q++) if (!isdigit((unsigned char)*q)) return 0;
    int x = atoi(p);
    if (d->kind == K_SIGN) { if (*v == '-') x = -x; return x >= d->min && x <= d->max; }
    if (d->kind == K_OFFSTEP) return x >= 0 && x <= d->max;
    return x >= d->min && x <= d->max;
}

/* --- beolvasás és írás a CAT-szálon --- */
int menu_sweep(void) {
    char cmd[12], r[96];
    if (!rig_menu_ok()) { rig_notice(1, TR("Ehhez a rádióhoz nincs ellenőrzött menütábla.", "There is no verified menu table for this radio.")); return -1; }
    for (int i = 0; i < N_MENU; i++) {
        rig_job(TR("Menük beolvasása", "Reading menus"), i, N_MENU);
        if (!cat_step()) { rig_job("", 0, 0); rig_notice(1, TR("Adás vagy kapcsolatvesztés miatt leállt a menük beolvasása.", "Reading menus stopped because of TX or lost connection.")); return -1; }
        if (D[i].prot == 2) continue;
        snprintf(cmd, sizeof cmd, "EX%s;", D[i].id);
        int q = cat_q(cmd, r, sizeof r);
        pthread_mutex_lock(&MM);
        if (q == 0) snprintf(V[i], sizeof V[i], "%.*s", (int)strcspn(r + 6, ";"), r + 6);
        else if (q == 1) V[i][0] = 0;
        pthread_mutex_unlock(&MM);
        if (q < 0) { rig_job("", 0, 0); return -1; }
    }
    pthread_mutex_lock(&MM); swept = 1; pthread_mutex_unlock(&MM);
    rig_job("", 0, 0);
    rig_bump();
    return 0;
}

static void stamp(char *o, size_t c) {
    time_t t = time(NULL);
    struct tm m;
    localtime_r(&t, &m);
    strftime(o, c, "%Y%m%d-%H%M%S", &m);
}

static int cmp_desc(const void *a, const void *b) { return strcmp(*(char *const *)b, *(char *const *)a); }

static int list_dir(const char *dir, const char *prefix, char **out, int max) {
    DIR *d = opendir(dir);
    int n = 0;
    if (!d) return 0;
    struct dirent *e;
    while ((e = readdir(d)) && n < max)
        if (e->d_name[0] != '.' && !strncmp(e->d_name, prefix, strlen(prefix))) out[n++] = strdup(e->d_name);
    closedir(d);
    qsort(out, (size_t)n, sizeof *out, cmp_desc);
    return n;
}

static void prune(const char *prefix) {
    char dir[320], path[600], *l[256];
    snprintf(dir, sizeof dir, "%s/backups", g_cfg.cfg_dir);
    int n = list_dir(dir, prefix, l, 256);
    for (int i = 0; i < n; i++) {
        if (i >= KEEP_BACKUPS) { snprintf(path, sizeof path, "%s/%s", dir, l[i]); unlink(path); }
        free(l[i]);
    }
}

static int save_backup(char *name, size_t cap) {
    char ts[32], path[400];
    stamp(ts, sizeof ts);
    snprintf(name, cap, "menu-%s", ts);
    snprintf(path, sizeof path, "%s/backups/%s.txt", g_cfg.cfg_dir, name);
    FILE *f = fopen(path, "w");
    if (!f) { rig_notice(1, TR("A mentés nem készült el: %s. Nem írok a rádióba.", "Backup failed: %s. Nothing written to the radio."), strerror(errno)); return -1; }
    fprintf(f, "# FT-891 menümentés\n");
    pthread_mutex_lock(&MM);
    for (int i = 0; i < N_MENU; i++) if (V[i][0]) fprintf(f, "%s=%s\n", D[i].id, V[i]);
    pthread_mutex_unlock(&MM);
    int bad = ferror(f);
    if (fclose(f) || bad) { rig_notice(1, TR("A mentés nem készült el. Nem írok a rádióba.", "Backup failed. Nothing written to the radio.")); return -1; }
    prune("menu-");
    return 0;
}

int menu_apply(void) {
    int done = 0, fail = 0, skip = 0, total = 0, rc = 0;
    char bk[64] = "", cmd[24], r[96];
    if (!rig_menu_ok()) { rig_notice(1, TR("Eltérő menütábla: a program nem ír a menükbe.", "Different menu table: the program does not write menus.")); rc = -1; goto out; }
    pthread_mutex_lock(&MM);
    int have = swept;
    pthread_mutex_unlock(&MM);
    if (!have && menu_sweep()) { rc = -1; goto out; }
    for (int k = 0; k < pn; k++) {
        int i = find(P[k].id);
        if (i < 0 || D[i].prot) { skip++; continue; }
        if (!V[i][0] || !valid(i, P[k].v, strlen(V[i]))) { skip++; continue; }
        if (strcmp(V[i], P[k].v)) total++;
    }
    if (!total && pmode < 0) {
        rig_notice(0, skip ? TR("Nincs írható változás, %d tételt kihagytam.", "No writable change, %d items skipped.") : TR("Nincs mit írni, a rádió már így áll.", "Nothing to write, the radio is already set this way."), skip);
        goto out;
    }
    if (total) {
        if (save_backup(bk, sizeof bk)) { rc = -1; goto out; }
        log_msg(TR("Mentés kész: %s", "Backup saved: %s"), bk);
    }
    for (int k = 0; k < pn; k++) {
        int i = find(P[k].id);
        if (i < 0 || D[i].prot || !V[i][0] || !valid(i, P[k].v, strlen(V[i])) || !strcmp(V[i], P[k].v)) continue;
        rig_job(TR("Menük írása", "Writing menus"), done + fail, total);
        if (!cat_step()) { rig_job("", 0, 0); rig_notice(1, TR("Adás vagy kapcsolatvesztés miatt leállt az írás. %d tétel készült el, a mentés: %s.", "Writing stopped because of TX or lost connection. %d items done, backup: %s."), done, bk); rc = -1; goto out; }
        snprintf(cmd, sizeof cmd, "EX%s%s;", P[k].id, P[k].v);
        if (cat_s(cmd)) { rc = -1; goto out; }
        snprintf(cmd, sizeof cmd, "EX%s;", P[k].id);
        int q = cat_q(cmd, r, sizeof r);
        char got[8] = "";
        if (q == 0) snprintf(got, sizeof got, "%.*s", (int)strcspn(r + 6, ";"), r + 6);
        pthread_mutex_lock(&MM);
        if (got[0]) snprintf(V[i], sizeof V[i], "%s", got);
        pthread_mutex_unlock(&MM);
        if (!strcmp(got, P[k].v)) done++;
        else { fail++; log_msg(TR("A %s menüt a rádió nem fogadta el: kért %s, kapott %s", "The radio did not accept menu %s: wanted %s, got %s"), P[k].id, P[k].v, got); }
    }
    rig_job("", 0, 0);
    rig_menu_refresh();
    if (pmode >= 0 && rig_set_mode_now(pmode)) fail++;
    if (fail) rig_notice(1, TR("%s: %d tétel kész, %d nem sikerült. A napló mutatja, melyik.", "%s: %d items done, %d failed. The log shows which."), plabel, done, fail);
    else if (!done) rig_notice(0, TR("%s: alkalmazva.", "%s: applied."), plabel);
    else rig_notice(0, TR("%s: %d beállítás kiírva.%s%s", "%s: %d settings written.%s%s"), plabel, done, bk[0] ? TR(" Mentés: ", " Backup: ") : "", bk);
    rc = fail ? -1 : 0;
out:
    rig_job("", 0, 0);
    pthread_mutex_lock(&MM); pn = 0; pmode = -1; busy = 0; pthread_mutex_unlock(&MM);
    rig_bump();
    return rc;
}

/* A sor formátuma: azonosító=érték, a profilban mode=N is lehet */
static int queue_text(char *text, const char *label) {
    pthread_mutex_lock(&MM);
    if (busy) { pthread_mutex_unlock(&MM); return -1; }
    pn = 0; pmode = -1;
    char *save = NULL;
    for (char *l = strtok_r(text, "\r\n", &save); l; l = strtok_r(NULL, "\r\n", &save)) {
        char *eq = strchr(l, '=');
        if (*l == '#' || !eq) continue;
        *eq = 0;
        if (!strcmp(l, "mode")) { pmode = atoi(eq + 1); continue; }
        if (pn < N_MENU && strlen(l) == 4 && strlen(eq + 1) < sizeof P[0].v) {
            snprintf(P[pn].id, sizeof P[pn].id, "%s", l);
            snprintf(P[pn].v, sizeof P[pn].v, "%s", eq + 1);
            pn++;
        }
    }
    snprintf(plabel, sizeof plabel, "%s", label);
    busy = 1;
    pthread_mutex_unlock(&MM);
    rig_post(OP_MENU_APPLY, 0, 0);
    return 0;
}

int menu_queue(char *text, const char *label) { return queue_text(text, label); }

static int safe_name(const char *n) {
    size_t l = strlen(n);
    if (!l || l > 40 || n[0] == '.') return 0;
    for (const char *p = n; *p; p++)
        if (*p == '/' || *p == '\\' || (unsigned char)*p < 0x20) return 0;
    return 1;
}

static void set_path(char *o, size_t c, int backup, const char *name) {
    snprintf(o, c, "%s/%s/%s.txt", g_cfg.cfg_dir, backup ? "backups" : "profiles", name);
}

static char *read_file(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return NULL;
    char *b = calloc(1, 16384);
    if (b) { size_t n = fread(b, 1, 16383, f); b[n] = 0; }
    fclose(f);
    return b;
}

int menu_queue_set(int backup, const char *name) {
    char path[400], label[80];
    if (!safe_name(name)) return -2;
    set_path(path, sizeof path, backup, name);
    char *t = read_file(path);
    if (!t) return -2;
    snprintf(label, sizeof label, "%s", backup ? TR("Visszaállítás", "Restore") : name);
    int r = queue_text(t, label);
    free(t);
    return r;
}

int profile_save(const char *name, const char *body) {
    char path[400];
    if (!safe_name(name) || strlen(body) > 16000) return -1;
    set_path(path, sizeof path, 0, name);
    FILE *f = fopen(path, "w");
    if (!f) return -1;
    fputs(body, f);
    return fclose(f) ? -1 : 0;
}

int profile_delete(const char *name) {
    char path[400];
    if (!safe_name(name)) return -1;
    set_path(path, sizeof path, 0, name);
    return unlink(path);
}

/* --- memóriacsatornák --- */
static void mem_ids(void) {
    for (int i = 0; i < 99; i++) snprintf(MEM[i].ch, 4, "%03d", i + 1);
    for (int i = 0; i < 18; i++) snprintf(MEM[99 + i].ch, 4, "P%d%c", i / 2 + 1, i % 2 ? 'U' : 'L');
}

static int mem_parse(const char *r, mem_t *m) {
    size_t n = strlen(r);
    if (n < 28) return -1;
    char f[10], o[5];
    memcpy(f, r + 5, 9); f[9] = 0;
    memcpy(o, r + 15, 4); o[4] = 0;
    m->f = atoll(f);
    m->sign = r[14];
    m->ofs = atoi(o);
    m->clar = r[19] == '1';
    m->md = r[21];
    m->tone = r[23] - '0';
    m->shift = r[26] - '0';
    m->tag[0] = 0;
    m->tag_ok = 0;
    if (n >= 41) {
        m->tag_ok = 1;
        if (r[27] == '1') {
            memcpy(m->tag, r + 28, 12);
            m->tag[12] = 0;
            for (int i = 11; i >= 0 && m->tag[i] == ' '; i--) m->tag[i] = 0;
        }
    }
    m->present = 1;
    return 0;
}

int mem_read(void) {
    char cmd[12], r[96];
    for (int i = 0; i < N_MEM; i++) {
        rig_job(TR("Memóriák beolvasása", "Reading memories"), i, N_MEM);
        if (!cat_step()) { rig_job("", 0, 0); rig_notice(1, TR("Adás vagy kapcsolatvesztés miatt leállt a memóriák beolvasása.", "Reading memories stopped because of TX or lost connection.")); return -1; }
        mem_t m = MEM[i];
        m.present = 0;
        snprintf(cmd, sizeof cmd, "MT%s;", m.ch);
        int q = cat_q(cmd, r, sizeof r);
        if (q == 1) {
            snprintf(cmd, sizeof cmd, "MR%s;", m.ch);
            q = cat_q(cmd, r, sizeof r);
        }
        if (q < 0) { rig_job("", 0, 0); return -1; }
        if (q == 0) mem_parse(r, &m);
        pthread_mutex_lock(&MM);
        MEM[i] = m;
        pthread_mutex_unlock(&MM);
    }
    pthread_mutex_lock(&MM); mem_read_ok = 1; pthread_mutex_unlock(&MM);
    rig_job("", 0, 0);
    rig_bump();
    return 0;
}

static int mem_backup(char *name, size_t cap) {
    char ts[32], path[400];
    stamp(ts, sizeof ts);
    snprintf(name, cap, "mem-%s", ts);
    snprintf(path, sizeof path, "%s/backups/%s.csv", g_cfg.cfg_dir, name);
    FILE *f = fopen(path, "w");
    if (!f) { rig_notice(1, TR("A memóriamentés nem készült el. Nem írok a rádióba.", "Memory backup failed. Nothing written to the radio.")); return -1; }
    fprintf(f, "csatorna,frekvencia,mod,clar_be,clar_hz,ton,eltolas,nev\n");
    pthread_mutex_lock(&MM);
    for (int i = 0; i < N_MEM; i++) {
        mem_t *m = &MEM[i];
        if (m->present) fprintf(f, "%s,%lld,%c,%d,%c%d,%d,%d,\"%s\"\n", m->ch, m->f, m->md, m->clar, m->sign, m->ofs, m->tone, m->shift, m->tag);
    }
    pthread_mutex_unlock(&MM);
    if (fclose(f)) return -1;
    prune("mem-");
    return 0;
}

int mem_write(void) {
    int done = 0, fail = 0, rc = 0;
    char bk[64], cmd[64], r[96];
    pthread_mutex_lock(&MM);
    int have = mem_read_ok;
    pthread_mutex_unlock(&MM);
    if (!have && mem_read()) { rc = -1; goto out; }
    if (mem_backup(bk, sizeof bk)) { rc = -1; goto out; }
    for (int k = 0; k < mpn; k++) {
        mem_t *m = &MP[k];
        rig_job(TR("Memóriák írása", "Writing memories"), k, mpn);
        if (!cat_step()) { rig_notice(1, TR("Adás vagy kapcsolatvesztés miatt leállt az írás. %d csatorna készült el.", "Writing stopped because of TX or lost connection. %d channels done."), done); rc = -1; goto out; }
        snprintf(cmd, sizeof cmd, "MT%.3s%09lld%c%04d%d0%c0%d00%d%d%-12.12s;", m->ch, m->f, m->sign, m->ofs,
                 m->clar, m->md, m->tone, m->shift, m->tag[0] ? 1 : 0, m->tag);
        if (cat_s(cmd)) { rc = -1; goto out; }
        snprintf(cmd, sizeof cmd, "MT%.3s;", m->ch);
        int q = cat_q(cmd, r, sizeof r);
        mem_t g = *m;
        int ok = q == 0 && mem_parse(r, &g) == 0 && g.f == m->f && g.md == m->md &&
                 (!g.tag_ok || !strcmp(g.tag, m->tag));
        for (int i = 0; ok && i < N_MEM; i++)
            if (!strcmp(MEM[i].ch, m->ch)) {
                pthread_mutex_lock(&MM); MEM[i] = g; pthread_mutex_unlock(&MM);
            }
        if (ok) done++;
        else { fail++; log_msg(TR("A %s csatorna visszaolvasása eltér", "Read-back of channel %s differs"), m->ch); }
    }
    if (fail) rig_notice(1, TR("%d csatorna kész, %d nem sikerült. Mentés: %s.", "%d channels done, %d failed. Backup: %s."), done, fail, bk);
    else rig_notice(0, TR("%d csatorna kiírva. Mentés: %s.", "%d channels written. Backup: %s."), done, bk);
    rc = fail ? -1 : 0;
out:
    rig_job("", 0, 0);
    pthread_mutex_lock(&MM); mpn = 0; busy = 0; pthread_mutex_unlock(&MM);
    rig_bump();
    return rc;
}

int mem_recall(int i) {
    char c[16];
    if (i < 0 || i >= N_MEM) return -1;
    snprintf(c, sizeof c, "MC%s;", MEM[i].ch);
    if (cat_s(c) || cat_s("MA;")) return -1;
    return 0;
}

/* sor: csatorna|frekvencia|mód|clar|előjeles eltérés|hang|eltolás|név */
int mem_queue(char *text, char *err, size_t cap) {
    static const char MODES[] = "123456789BCD";
    int n = 0, line = 0;
    char *save = NULL;
    mem_t tmp[N_MEM];
    for (char *l = strtok_r(text, "\n", &save); l; l = strtok_r(NULL, "\n", &save)) {
        char *f[8], *p = l;
        int k = 0;
        line++;
        while (k < 8) { f[k++] = p; char *e = strchr(p, '|'); if (!e) break; *e = 0; p = e + 1; }
        if (k != 8 || n >= N_MEM) { snprintf(err, cap, TR("Hibás sor: %d.", "Bad line: %d."), line); return -1; }
        mem_t m;
        memset(&m, 0, sizeof m);
        int idx = -1;
        pthread_mutex_lock(&MM);
        for (int i = 0; i < N_MEM; i++) if (!strcmp(MEM[i].ch, f[0])) idx = i;
        pthread_mutex_unlock(&MM);
        m.f = atoll(f[1]);
        int ofs = atoi(f[4]);
        m.md = f[2][0];
        m.clar = atoi(f[3]) != 0;
        m.tone = atoi(f[5]);
        m.shift = atoi(f[6]);
        f[7][strcspn(f[7], "\r")] = 0;
        if (idx < 0) { snprintf(err, cap, "Ismeretlen csatorna: %.8s.", f[0]); return -1; }
        if (m.f < 30000 || m.f > 56000000) { snprintf(err, cap, TR("%.8s: a frekvencia a rádió tartományán kívül esik.", "%.8s: frequency outside the radio's range."), f[0]); return -1; }
        if (!m.md || !strchr(MODES, m.md)) { snprintf(err, cap, TR("%.8s: ismeretlen mód.", "%.8s: unknown mode."), f[0]); return -1; }
        if (ofs < -9999 || ofs > 9999 || m.tone < 0 || m.tone > 2 || m.shift < 0 || m.shift > 2) { snprintf(err, cap, TR("%.8s: hibás érték.", "%.8s: bad value."), f[0]); return -1; }
        if (strlen(f[7]) > 12) { snprintf(err, cap, TR("%.8s: a név legfeljebb 12 karakter.", "%.8s: the name is at most 12 characters."), f[0]); return -1; }
        for (char *q = f[7]; *q; q++)
            if ((unsigned char)*q < 0x20 || (unsigned char)*q > 0x7E || *q == ';') {
                snprintf(err, cap, TR("%.8s: a név csak ékezet nélküli betű és szám lehet.", "%.8s: the name may only contain ASCII letters and digits."), f[0]);
                return -1;
            }
        snprintf(m.ch, sizeof m.ch, "%s", f[0]);
        m.sign = ofs < 0 ? '-' : '+';
        m.ofs = abs(ofs);
        snprintf(m.tag, sizeof m.tag, "%s", f[7]);
        tmp[n++] = m;
    }
    if (!n) { snprintf(err, cap, TR("Nincs írandó csatorna.", "No channel to write.")); return -1; }
    pthread_mutex_lock(&MM);
    if (busy) { pthread_mutex_unlock(&MM); snprintf(err, cap, TR("Már fut egy művelet.", "Another operation is running.")); return -1; }
    memcpy(MP, tmp, sizeof(mem_t) * (size_t)n);
    mpn = n;
    busy = 1;
    pthread_mutex_unlock(&MM);
    rig_post(OP_MEM_WRITE, 0, 0);
    return 0;
}

/* --- JSON --- */
void menu_json(sb_t *b) {
    pthread_mutex_lock(&MM);
    int have = swept;
    pthread_mutex_unlock(&MM);
    sb_add(b, "{\"read\":%s,\"defs\":[", have ? "true" : "false");
    for (int i = 0; i < N_MENU; i++) {
        const mdef_t *d = &D[i];
        int en = g_cfg.lang_en;
        sb_add(b, "%s{\"id\":\"%s\",\"name\":\"%s\",\"grp\":", i ? "," : "", d->id, d->name);
        sb_str(b, en ? d->grp_en : d->grp);
        sb_add(b, ",\"desc\":");
        sb_str(b, en ? d->en : d->hu);
        sb_add(b, ",\"k\":%d,\"d\":%d,\"min\":%d,\"max\":%d,\"step\":%d,\"unit\":", d->kind, d->digits, d->min, d->max, d->step);
        sb_str(b, en ? d->unit_en : d->unit);
        sb_add(b, ",\"labels\":");
        sb_str(b, en ? d->labels_en : d->labels);
        sb_add(b, ",\"prot\":%d}", d->prot);
    }
    sb_add(b, "],\"vals\":{");
    int first = 1;
    pthread_mutex_lock(&MM);
    for (int i = 0; i < N_MENU; i++)
        if (V[i][0]) { sb_add(b, "%s\"%s\":\"%s\"", first ? "" : ",", D[i].id, V[i]); first = 0; }
    pthread_mutex_unlock(&MM);
    sb_add(b, "}}");
}

void sets_json(sb_t *b, int backup) {
    char dir[320], path[600], *l[256];
    snprintf(dir, sizeof dir, "%s/%s", g_cfg.cfg_dir, backup ? "backups" : "profiles");
    int n = list_dir(dir, backup ? "menu-" : "", l, 256);
    sb_add(b, "[");
    for (int i = 0; i < n; i++) {
        char *dot = strrchr(l[i], '.');
        if (dot) *dot = 0;
        snprintf(path, sizeof path, "%s/%s.txt", dir, l[i]);
        char *t = read_file(path);
        sb_add(b, "%s{\"name\":", i ? "," : "");
        sb_str(b, l[i]);
        sb_add(b, ",\"items\":{");
        int first = 1;
        char *save = NULL;
        for (char *x = t ? strtok_r(t, "\r\n", &save) : NULL; x; x = strtok_r(NULL, "\r\n", &save)) {
            char *eq = strchr(x, '=');
            if (*x == '#' || !eq) continue;
            *eq = 0;
            sb_add(b, "%s", first ? "" : ",");
            sb_str(b, x);
            sb_add(b, ":");
            sb_str(b, eq + 1);
            first = 0;
        }
        sb_add(b, "}}");
        free(t);
        free(l[i]);
    }
    sb_add(b, "]");
}

void mem_json(sb_t *b) {
    pthread_mutex_lock(&MM);
    sb_add(b, "{\"read\":%s,\"rows\":[", mem_read_ok ? "true" : "false");
    for (int i = 0; i < N_MEM; i++) {
        mem_t *m = &MEM[i];
        sb_add(b, "%s{\"ch\":\"%s\",\"on\":%d", i ? "," : "", m->ch, m->present);
        if (m->present) {
            sb_add(b, ",\"f\":%lld,\"md\":\"%c\",\"clar\":%d,\"ofs\":%d,\"tone\":%d,\"shift\":%d,\"tag\":",
                   m->f, m->md, m->clar, m->sign == '-' ? -m->ofs : m->ofs, m->tone, m->shift);
            sb_str(b, m->tag);
        }
        sb_add(b, "}");
    }
    pthread_mutex_unlock(&MM);
    sb_add(b, "]}");
}

/* Ha a sorba állított írás nem futott le (nincs kapcsolat), a tiltás ne ragadjon be */
void menu_cancel(void) {
    pthread_mutex_lock(&MM);
    int was = busy;
    pn = 0; mpn = 0; pmode = -1; busy = 0;
    pthread_mutex_unlock(&MM);
    if (was) rig_notice(1, TR("A rádió nem elérhető, az írás nem történt meg.", "The radio is not available, nothing was written."));
}

void menu_on_connect(void) {
    pthread_mutex_lock(&MM);
    swept = 0;
    mem_read_ok = 0;
    pthread_mutex_unlock(&MM);
}

static void put(const char *name, const char *body) {
    char path[400];
    set_path(path, sizeof path, 0, name);
    if (access(path, F_OK)) { FILE *f = fopen(path, "w"); if (f) { fputs(body, f); fclose(f); } }
}

void menu_init(void) {
    char p[320];
    snprintf(p, sizeof p, "%s/backups", g_cfg.cfg_dir);
    mkdir(p, 0700);
    snprintf(p, sizeof p, "%s/profiles", g_cfg.cfg_dir);
    int fresh = mkdir(p, 0700) == 0;
    mem_ids();
    if (fresh) {
        put("SSB", "1105=0\n1107=2\nmode=2\n");
        put("Digi", "0809=1\n0812=0\nmode=11\n");
        put("CW", "0707=2\nmode=3\n");
    }
}
