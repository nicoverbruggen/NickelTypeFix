#ifndef NTF_UTIL_H
#define NTF_UTIL_H
#ifdef __cplusplus
extern "C" {
#endif

#include <ctype.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include <NickelHook.h>

// The mod version, baked in by NickelHook.mk (git describe). Logged on every line and in the
// startup block, so a user-attached log always says exactly which build produced it.
#ifndef NH_VERSION
#define NH_VERSION "dev"
#endif

// Cap the on-device log so it can't grow without bound across many boots. On the first write of
// a boot, if the log is larger than this it's rotated to a single ".old" generation. A healthy
// boot writes nothing, so this is reached only by a long-lived or verbose device.
#ifndef NTF_LOG_MAX_BYTES
#define NTF_LOG_MAX_BYTES (256 * 1024)
#endif

__attribute__((unused)) static inline char *strtrim(char *s) {
    if (!s)
        return NULL;

    char *a = s;
    char *b = s + strlen(s);
    for (; a < b && isspace((unsigned char)(*a)); a++);
    for (; b > a && isspace((unsigned char)(*(b - 1))); b--);
    *b = '\0';
    return a;
}

__attribute__((unused)) static inline void ntf_log_file_line(const char *file, int line, const char *fmt, ...) {
    char msg[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(msg, sizeof(msg), fmt, args);
    va_end(args);
    msg[sizeof(msg) - 1] = '\0';

    nh_log("%s (%s:%d)", msg, file, line);

    // First file write of the boot: create the directory if it's missing, and rotate the log if
    // it grew past the cap. A benign race if two threads hit this first (at most a redundant
    // mkdir or rename); the flag keeps it to one check per process. Doing the mkdir here rather
    // than on every line also means a log call after ntf_uninstall can't recreate the folder.
    static bool ntf_log_setup_done = false;
    if (!ntf_log_setup_done) {
        ntf_log_setup_done = true;
        mkdir(NTF_CONFIG_DIR, 0755);
        struct stat st;
        if (stat(NTF_CONFIG_DIR "/nickel-type-fix.log", &st) == 0 && st.st_size > NTF_LOG_MAX_BYTES)
            rename(NTF_CONFIG_DIR "/nickel-type-fix.log", NTF_CONFIG_DIR "/nickel-type-fix.log.old");
    }

    FILE *f = fopen(NTF_CONFIG_DIR "/nickel-type-fix.log", "a");
    if (!f)
        return;

    // localtime_r, not localtime: logging happens on whatever thread hit a
    // problem (the FT hook runs on Nickel's render threads), and localtime's
    // shared static buffer is a data race between concurrent callers.
    time_t now = time(NULL);
    struct tm tmbuf;
    struct tm *tm = localtime_r(&now, &tmbuf);
    if (tm) {
        fprintf(f, "%04d-%02d-%02d %02d:%02d:%02d ",
            tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday,
            tm->tm_hour, tm->tm_min, tm->tm_sec);
    }

    fprintf(f, "NickelTypeFix " NH_VERSION ": %s (%s:%d)\n", msg, file, line);
    fclose(f);
}

#define NTF_LOG(fmt, ...) ntf_log_file_line(__FILE__, __LINE__, fmt, ##__VA_ARGS__)

// ntf_log_firmware logs the running firmware version once at startup, next to the mod version
// and the resolved-symbol map, so a future-firmware breakage report shows which firmware ran.
// The serial number (field 0 of /mnt/onboard/.kobo/version) is deliberately dropped. Failures
// are silent.
__attribute__((unused)) static inline void ntf_log_firmware(void) {
    FILE *f = fopen("/mnt/onboard/.kobo/version", "r");
    if (!f) {
        NTF_LOG("startup: firmware version unavailable");
        return;
    }
    char vline[512];
    char *got = fgets(vline, sizeof(vline), f);
    fclose(f);
    if (!got)
        return;
    vline[strcspn(vline, "\r\n")] = '\0';
    const char *comma = strchr(vline, ',');   // <serial>,<...>,<firmware>,<model>,...
    NTF_LOG("startup: firmware %s", comma ? comma + 1 : vline);
}

#ifdef __cplusplus
}
#endif
#endif
