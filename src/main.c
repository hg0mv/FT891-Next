/* FT-891 vezérlő - belépési pont */
#include "ft891ctl.h"
#include <getopt.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifndef _WIN32
static void on_signal(int s) { (void)s; g_stop = 1; }
#endif

static void usage(void) {
    if (!g_cfg.lang_en) {
        puts("FT891 Next " APP_VERSION "\n\n"
             "Használat: ft891ctl [kapcsolók]\n"
             "  --cat-port ÚT     CAT-port (alap: automatikus keresés)\n"
             "  --ptt-port ÚT     RTS/DTR PTT portja (alap: a CAT-port párja)\n"
             "  --web-port N      webes felület portja (alap: 8891)\n"
             "  --web-bind CÍM    webes felület címe (alap: 0.0.0.0)\n"
             "  --tls-port N      HTTPS-port (alap: a webes port + 1, 0 = kikapcsolva)\n"
             "  --rig-port N      rigctld-kompatibilis port (alap: 4532, 0 = kikapcsolva)\n"
             "  --rig-bind CÍM    rigctld-kompatibilis cím (alap: 127.0.0.1)\n"
             "  --poll-ms N       lekérdezés üteme nyitott felületnél (alap: 250)\n"
             "  --arecord ÚT      az arecord program helye\n"
             "  --aplay ÚT        az aplay program helye\n"
             "  --no-ai           ne kapcsolja be a rádió Auto Information funkcióját\n"
             "  --trace           minden CAT-parancs naplózása\n"
             "  --config-dir ÚT   beállítások mappája (alap: ~/.config/ft891ctl)\n"
             "  --lang hu|en      a felület nyelve (alap: a rendszer nyelve)\n"
             "  --new-pin         új PIN-kódot készít\n");
        return;
    }
    puts("FT891 Next " APP_VERSION "\n\n"
         "Usage: ft891ctl [options]\n"
         "  --cat-port PATH   CAT port (default: automatic search)\n"
         "  --ptt-port PATH   RTS/DTR PTT port (default: the CAT port's pair)\n"
         "  --web-port N      web interface port (default: 8891)\n"
         "  --web-bind ADDR   web interface address (default: 0.0.0.0)\n"
         "  --tls-port N      HTTPS port (default: web port + 1, 0 = off)\n"
         "  --rig-port N      rigctld-compatible port (default: 4532, 0 = off)\n"
         "  --rig-bind ADDR   rigctld-compatible address (default: 127.0.0.1)\n"
         "  --poll-ms N       polling interval with an open interface (default: 250)\n"
         "  --arecord PATH    arecord program\n"
         "  --aplay PATH      aplay program\n"
         "  --no-ai           do not use the radio's Auto Information\n"
         "  --trace           log every CAT command\n"
         "  --config-dir PATH settings folder (default: ~/.config/ft891ctl)\n"
         "  --lang hu|en      interface language (default: system language)\n"
         "  --new-pin         make a new PIN\n");
}

int main(int argc, char **argv) {
    static const struct option o[] = {
        {"cat-port", 1, 0, 'c'}, {"ptt-port", 1, 0, 'p'}, {"web-port", 1, 0, 'w'},
        {"web-bind", 1, 0, 'W'}, {"rig-port", 1, 0, 'r'}, {"rig-bind", 1, 0, 'R'},
        {"poll-ms", 1, 0, 'P'},  {"arecord", 1, 0, 'A'}, {"aplay", 1, 0, 'B'}, {"tls-port", 1, 0, 'T'},  {"no-ai", 0, 0, 'a'},    {"trace", 0, 0, 't'},
        {"config-dir", 1, 0, 'd'}, {"lang", 1, 0, 'L'}, {"new-pin", 0, 0, 'n'}, {"help", 0, 0, 'h'}, {0, 0, 0, 0}};
    snprintf(g_cfg.web_bind, sizeof g_cfg.web_bind, "0.0.0.0");
    snprintf(g_cfg.rig_bind, sizeof g_cfg.rig_bind, "127.0.0.1");
    g_cfg.web_port = 8891;
    g_cfg.tls_port = -1;
    g_cfg.rig_port = 4532;
    g_cfg.poll_ms = 250;
    g_cfg.use_ai = 1;
    g_cfg.tx_max_s = 180;
    sys_init();
    sys_default_cfg_dir(g_cfg.cfg_dir, sizeof g_cfg.cfg_dir);

    /* A nyelv alapértéke a rendszer nyelve; a súgóhoz már most kell */
    g_cfg.lang_en = !sys_lang_hu();
    int c, new_pin = 0, lang = -1;
    while ((c = getopt_long(argc, argv, "h", o, NULL)) != -1) {
        switch (c) {
        case 'c': snprintf(g_cfg.cat_port, sizeof g_cfg.cat_port, "%s", optarg); break;
        case 'p': snprintf(g_cfg.ptt_port, sizeof g_cfg.ptt_port, "%s", optarg); break;
        case 'w': g_cfg.web_port = atoi(optarg); break;
        case 'W': snprintf(g_cfg.web_bind, sizeof g_cfg.web_bind, "%s", optarg); break;
        case 'r': g_cfg.rig_port = atoi(optarg); break;
        case 'R': snprintf(g_cfg.rig_bind, sizeof g_cfg.rig_bind, "%s", optarg); break;
        case 'P': g_cfg.poll_ms = atoi(optarg); break;
        case 'a': g_cfg.use_ai = 0; break;
        case 'B': snprintf(g_cfg.aplay, sizeof g_cfg.aplay, "%s", optarg); break;
        case 'T': g_cfg.tls_port = atoi(optarg); break;
        case 'A': snprintf(g_cfg.arecord, sizeof g_cfg.arecord, "%s", optarg); break;
        case 't': g_cfg.trace = 1; break;
        case 'd': snprintf(g_cfg.cfg_dir, sizeof g_cfg.cfg_dir, "%s", optarg); break;
        case 'n': new_pin = 1; break;
        case 'L': lang = !strcmp(optarg, "hu") ? 0 : !strcmp(optarg, "en") ? 1 : -2; break;
        default: usage(); return c == 'h' ? 0 : 2;
        }
    }
    if (g_cfg.poll_ms < 100) g_cfg.poll_ms = 100;
    if (lang == -2) { usage(); return 2; }
    log_init();
    {
        /* A configban tárolt nyelv csak akkor számít, ha már mentették */
        char p[320];
        snprintf(p, sizeof p, "%s/config", g_cfg.cfg_dir);
        FILE *f = fopen(p, "r");
        int had = 0;
        char line[300];
        while (f && fgets(line, sizeof line, f)) if (!strncmp(line, "lang=", 5)) had = 1;
        if (f) fclose(f);
        int sys_en = g_cfg.lang_en;
        config_load();
        if (!had) g_cfg.lang_en = sys_en;
    }
    if (lang >= 0) g_cfg.lang_en = lang;
    config_save();
    if (new_pin) config_new_pin();
    menu_init();

#ifndef _WIN32
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);
#endif

    rig_start();
    if (g_cfg.rig_port > 0) rigsrv_start();
    audio_start();
    web_start();
    web_print_urls();
#ifndef _WIN32
    while (!g_stop) pause();
#else
    while (!g_stop) sleep_ms(200);   /* a Ctrl+C-t a compat.c kezelője jelzi */
#endif
    log_msg(TR("Leállítás", "Shutting down"));
    rig_stop();
    return 0;
}
