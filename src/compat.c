/* Rendszerfüggő segédek: Linux és Windows */
#include "ft891ctl.h"
#include <stdlib.h>
#include <string.h>

#ifndef _WIN32
#include <ifaddrs.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

void sys_init(void) {}

int sys_lang_hu(void) {
    const char *loc = getenv("LC_ALL");
    if (!loc || !*loc) loc = getenv("LC_MESSAGES");
    if (!loc || !*loc) loc = getenv("LANG");
    return loc && !strncmp(loc, "hu", 2);
}

void sys_default_cfg_dir(char *out, size_t cap) {
    const char *home = getenv("HOME");
    snprintf(out, cap, "%s/.config/ft891ctl", home ? home : ".");
}

int sys_local_ipv4(char (*out)[64], int max) {
    struct ifaddrs *ifa = NULL;
    int n = 0;
    if (getifaddrs(&ifa)) return 0;
    for (struct ifaddrs *i = ifa; i && n < max; i = i->ifa_next) {
        if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET) continue;
        char h[64];
        if (getnameinfo(i->ifa_addr, sizeof(struct sockaddr_in), h, sizeof h, NULL, 0, NI_NUMERICHOST)) continue;
        if (strncmp(h, "127.", 4)) snprintf(out[n++], 64, "%s", h);
    }
    freeifaddrs(ifa);
    return n;
}

int rename_replace(const char *from, const char *to) { return rename(from, to); }
void sleep_ms(int ms) { usleep((useconds_t)ms * 1000); }

#else
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <windows.h>

static BOOL WINAPI on_ctrl(DWORD t) {
    (void)t;
    g_stop = 1;
    return TRUE;
}

void sys_init(void) {
    WSADATA w;
    if (WSAStartup(MAKEWORD(2, 2), &w)) { fprintf(stderr, "WSAStartup\n"); exit(1); }
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCtrlHandler(on_ctrl, TRUE);
}

int sys_lang_hu(void) {
    const char *loc = getenv("LANG");        /* ha valaki mégis megadta */
    if (loc && *loc) return !strncmp(loc, "hu", 2);
    return PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_HUNGARIAN;
}

void sys_default_cfg_dir(char *out, size_t cap) {
    const char *a = getenv("APPDATA");
    snprintf(out, cap, "%s/ft891ctl", a ? a : ".");
}

int sys_local_ipv4(char (*out)[64], int max) {
    ULONG sz = 16384;
    IP_ADAPTER_ADDRESSES *aa = malloc(sz);
    int n = 0;
    if (!aa) return 0;
    if (GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER, NULL, aa, &sz) == ERROR_BUFFER_OVERFLOW) {
        free(aa);
        aa = malloc(sz);
        if (!aa || GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER, NULL, aa, &sz) != NO_ERROR) { free(aa); return 0; }
    }
    for (IP_ADAPTER_ADDRESSES *a = aa; a && n < max; a = a->Next) {
        if (a->OperStatus != IfOperStatusUp) continue;
        for (IP_ADAPTER_UNICAST_ADDRESS *u = a->FirstUnicastAddress; u && n < max; u = u->Next) {
            char h[64];
            if (getnameinfo(u->Address.lpSockaddr, (socklen_t)u->Address.iSockaddrLength, h, sizeof h, NULL, 0, NI_NUMERICHOST)) continue;
            if (strncmp(h, "127.", 4) && strncmp(h, "169.254.", 8)) snprintf(out[n++], 64, "%s", h);
        }
    }
    free(aa);
    return n;
}

int rename_replace(const char *from, const char *to) {
    return MoveFileExA(from, to, MOVEFILE_REPLACE_EXISTING) ? 0 : -1;
}

void sleep_ms(int ms) { Sleep((DWORD)ms); }

void *memmem(const void *h, size_t hn, const void *nd, size_t nn) {
    const char *p = h;
    if (!nn) return (void *)h;
    for (size_t i = 0; i + nn <= hn; i++)
        if (p[i] == *(const char *)nd && !memcmp(p + i, nd, nn)) return (void *)(p + i);
    return NULL;
}
#endif

#ifdef _WIN32
#include <bcrypt.h>
int random_bytes(void *buf, size_t n) {
    return BCryptGenRandom(NULL, buf, (ULONG)n, BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0 ? 0 : -1;
}
#endif
