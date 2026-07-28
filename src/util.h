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

    mkdir(NTF_CONFIG_DIR, 0755);

    // Rotate once per process, on the first file write of the boot. A benign race if two threads
    // hit this first (at most a redundant rename); the flag keeps it to one check per process.
    static bool ntf_log_rotate_checked = false;
    if (!ntf_log_rotate_checked) {
        ntf_log_rotate_checked = true;
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

    fprintf(f, "NickelTypeFix: %s (%s:%d)\n", msg, file, line);
    fclose(f);
}

#define NTF_LOG(fmt, ...) ntf_log_file_line(__FILE__, __LINE__, fmt, ##__VA_ARGS__)

#ifdef __cplusplus
}
#endif
#endif
