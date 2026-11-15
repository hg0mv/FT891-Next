/* FT-891 vezérlő - belépési pont */
#include "ft891ctl.h"
#include <getopt.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void on_signal(int s) { (void)s; g_stop = 1; }

static void usage(void) {
    puts("FT-891 vezérlő " APP_VERSION "\n\n"
         "Használat: ft891ctl [kapcsolók]\n"
         "  --cat-port ÚT     CAT-port (alap: automatikus keresés)\n"
         "  --ptt-port ÚT     RTS/DTR PTT portja (alap: a CAT-port párja)\n"
         "  --web-port N      webes felület portja (alap: 8891)\n"
         "  --web-bind CÍM    webes felület címe (alap: 0.0.0.0)\n"
         "  --rig-port N      rigctld-kompatibilis port (alap: 4532, 0 = kikapcsolva)\n"
         "  --rig-bind CÍM    rigctld-kompatibilis cím (alap: 127.0.0.1)\n"
         "  --poll-ms N       lekérdezés üteme nyitott felületnél (alap: 250)\n"
         "  --tls-port N      HTTPS-port (alap: a webes port + 1, 0 = kikapcsolva)\n"
         "  --arecord ÚT      az arecord program helye\n"
         "  --aplay ÚT        az aplay program helye\n"
         "  --no-ai           ne kapcsolja be a rádió Auto Information funkcióját\n"
         "  --trace           minden CAT-parancs naplózása\n"
         "  --config-dir ÚT   beállítások mappája (alap: ~/.config/ft891ctl)\n"
         "  --new-pin         új PIN-kódot készít\n");
}

int main(int argc, char **argv) {
    static const struct option o[] = {
        {"cat-port", 1, 0, 'c'}, {"ptt-port", 1, 0, 'p'}, {"web-port", 1, 0, 'w'},
        {"web-bind", 1, 0, 'W'}, {"rig-port", 1, 0, 'r'}, {"rig-bind", 1, 0, 'R'},
        {"poll-ms", 1, 0, 'P'},  {"arecord", 1, 0, 'A'}, {"aplay", 1, 0, 'B'}, {"tls-port", 1, 0, 'T'},  {"no-ai", 0, 0, 'a'},    {"trace", 0, 0, 't'},
        {"config-dir", 1, 0, 'd'}, {"new-pin", 0, 0, 'n'}, {"help", 0, 0, 'h'}, {0, 0, 0, 0}};
    snprintf(g_cfg.web_bind, sizeof g_cfg.web_bind, "0.0.0.0");
    snprintf(g_cfg.rig_bind, sizeof g_cfg.rig_bind, "127.0.0.1");
    g_cfg.web_port = 8891;
    g_cfg.tls_port = -1;
    g_cfg.rig_port = 4532;
    g_cfg.poll_ms = 250;
    g_cfg.use_ai = 1;
    g_cfg.tx_max_s = 180;
    const char *home = getenv("HOME");
    snprintf(g_cfg.cfg_dir, sizeof g_cfg.cfg_dir, "%s/.config/ft891ctl", home ? home : ".");

    int c, new_pin = 0;
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
        default: usage(); return c == 'h' ? 0 : 2;
        }
    }
    if (g_cfg.poll_ms < 100) g_cfg.poll_ms = 100;
    log_init();
    config_load();
    if (new_pin) config_new_pin();
    menu_init();

    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    rig_start();
    if (g_cfg.rig_port > 0) rigsrv_start();
    audio_start();
    web_start();
    web_print_urls();
    while (!g_stop) pause();
    log_msg("Leállítás");
    rig_stop();
    return 0;
}
