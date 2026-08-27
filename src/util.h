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
// startup table, so a user-attached log always says exactly which build produced it.
#ifndef NH_VERSION
#define NH_VERSION "dev"
#endif

// Cap the on-device log so it can't grow without bound across many boots. On the first write of
// a boot, if the log is larger than this it's rotated to a single ".old" generation. A healthy
// boot writes nothing, so this is reached only by a long-lived or verbose device.
#ifndef NTF_LOG_MAX_BYTES
#define NTF_LOG_MAX_BYTES (256 * 1024)
#endif

// Shared by every translation unit that includes this header. Keeping this state in the inline
// logger itself would make config.c mistake the config table for a second boot and add another gap.
extern bool ntf_log_setup_done;
extern bool ntf_log_prepend_newline;

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

__attribute__((unused)) static inline void ntf_log_message(const char *file, int line, const char *msg) {
    if (file)
        nh_log("%s (%s:%d)", msg, file, line);
    else
        nh_log("%s", msg);

    // First file write of the boot: create the directory if it's missing, and rotate the log if
    // it grew past the cap. A benign race if two threads hit this first (at most a redundant
    // mkdir or rename); the flag keeps it to one check per process. Doing the mkdir here rather
    // than on every line also means a log call after ntf_uninstall can't recreate the folder.
    if (!ntf_log_setup_done) {
        ntf_log_setup_done = true;
        mkdir(NTF_CONFIG_DIR, 0755);
        struct stat st;
        if (stat(NTF_CONFIG_DIR "/nickel-type-fix.log", &st) == 0) {
            if (st.st_size > NTF_LOG_MAX_BYTES) {
                if (rename(NTF_CONFIG_DIR "/nickel-type-fix.log", NTF_CONFIG_DIR "/nickel-type-fix.log.old") != 0)
                    ntf_log_prepend_newline = true;
            } else {
                ntf_log_prepend_newline = st.st_size > 0;
            }
        }
    }

    FILE *f = fopen(NTF_CONFIG_DIR "/nickel-type-fix.log", "a");
    if (!f)
        return;
    if (ntf_log_prepend_newline) {
        fputc('\n', f);
        ntf_log_prepend_newline = false;
    }

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

    fprintf(f, "NickelTypeFix " NH_VERSION ": %s", msg);
    if (file)
        fprintf(f, " (%s:%d)", file, line);
    fputc('\n', f);
    fclose(f);
}

__attribute__((unused)) static inline void ntf_log_file_line(const char *file, int line, const char *fmt, ...) {
    char msg[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(msg, sizeof(msg), fmt, args);
    va_end(args);
    msg[sizeof(msg) - 1] = '\0';
    ntf_log_message(file, line, msg);
}

// Startup-table rows deliberately omit source locations. They are normal status output, not
// diagnostics, and the repeated suffix makes the aligned columns needlessly hard to scan.
__attribute__((unused)) static inline void ntf_log_plain(const char *fmt, ...) {
    char msg[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(msg, sizeof(msg), fmt, args);
    va_end(args);
    msg[sizeof(msg) - 1] = '\0';
    ntf_log_message(NULL, 0, msg);
}

#define NTF_LOG(fmt, ...) ntf_log_file_line(__FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define NTF_LOG_PLAIN(fmt, ...) ntf_log_plain(fmt, ##__VA_ARGS__)

// Read only the firmware field from Kobo's version file. Never return the serial number in field 0.
// Failures produce a stable value so the startup table stays complete.
__attribute__((unused)) static inline void ntf_get_firmware_version(char *out, size_t out_size) {
    if (!out || !out_size)
        return;
    snprintf(out, out_size, "unavailable");
    FILE *f = fopen("/mnt/onboard/.kobo/version", "r");
    if (!f)
        return;
    char vline[512];
    char *got = fgets(vline, sizeof(vline), f);
    fclose(f);
    if (!got)
        return;
    vline[strcspn(vline, "\r\n")] = '\0';
    char *field = vline;
    for (int i = 0; i < 2; i++) {
        field = strchr(field, ',');
        if (!field)
            return;
        field++;
    }
    char *end = strchr(field, ',');
    if (end)
        *end = '\0';
    if (*field)
        snprintf(out, out_size, "%s", field);
}

#ifdef __cplusplus
}
#endif
#endif
