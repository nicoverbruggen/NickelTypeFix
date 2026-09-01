#ifndef NTF_UTIL_H
#define NTF_UTIL_H
#ifdef __cplusplus
extern "C" {
#endif

#include <ctype.h>
#include <pthread.h>
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

// Buffer diagnostics in memory and write them in one go.
//
// Each line used to open, write and close the log file on its own. On eMMC through vfat that is
// the expensive part, not the bytes, and verbose logging can produce hundreds of lines for a
// single page render — enough to distort the very timings a probe is there to measure.
//
// Problems are not buffered. NTF_LOG means something went wrong, it is rare, and it has to survive
// a crash that happens immediately afterwards, so it writes through and takes any pending
// diagnostics with it to keep the order right. NTF_DBG is high-volume and can wait.
#ifndef NTF_LOG_BUFFER_BYTES
#define NTF_LOG_BUFFER_BYTES (32 * 1024)
#endif
// A quiet buffer is flushed on the next line rather than by a timer, so a burst that stops does
// not sit unwritten for longer than this once anything else is logged.
#ifndef NTF_LOG_BUFFER_MAX_AGE_S
#define NTF_LOG_BUFFER_MAX_AGE_S 2
#endif

// Shared by every translation unit that includes this header. Keeping this state in the inline
// logger itself would make config.c mistake the config table for a second boot and add another gap.
extern bool ntf_log_setup_done;
extern bool ntf_log_prepend_newline;
extern char ntf_log_buffer[NTF_LOG_BUFFER_BYTES];
extern size_t ntf_log_buffer_used;
extern time_t ntf_log_buffer_since;
extern pthread_mutex_t ntf_log_buffer_lock;

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

// Open the log once and append a block. Caller holds ntf_log_buffer_lock.
__attribute__((unused)) static inline void ntf_log_write_block(const char *block, size_t len) {
    if (!block || !len)
        return;

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
    fwrite(block, 1, len, f);
    fclose(f);
}

// Caller holds ntf_log_buffer_lock.
__attribute__((unused)) static inline void ntf_log_flush_locked(void) {
    if (!ntf_log_buffer_used)
        return;
    ntf_log_write_block(ntf_log_buffer, ntf_log_buffer_used);
    ntf_log_buffer_used = 0;
    ntf_log_buffer_since = 0;
}

// Write anything still held in memory. Safe to call from anywhere, including a shutdown path.
__attribute__((unused)) static inline void ntf_log_flush(void) {
    pthread_mutex_lock(&ntf_log_buffer_lock);
    ntf_log_flush_locked();
    pthread_mutex_unlock(&ntf_log_buffer_lock);
}

__attribute__((unused)) static inline void ntf_log_message_ex(const char *file, int line,
                                                              const char *msg, bool buffered) {
    if (file)
        nh_log("%s (%s:%d)", msg, file, line);
    else
        nh_log("%s", msg);

    // localtime_r, not localtime: logging happens on whatever thread hit a
    // problem (the FT hook runs on Nickel's render threads), and localtime's
    // shared static buffer is a data race between concurrent callers.
    time_t now = time(NULL);
    struct tm tmbuf;
    struct tm *tm = localtime_r(&now, &tmbuf);

    char line_buf[1280];
    int n = 0;
    if (tm) {
        n = snprintf(line_buf, sizeof(line_buf), "%04d-%02d-%02d %02d:%02d:%02d ",
            tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday,
            tm->tm_hour, tm->tm_min, tm->tm_sec);
        if (n < 0 || (size_t)n >= sizeof(line_buf)) n = 0;
    }
    int m = snprintf(line_buf + n, sizeof(line_buf) - (size_t)n,
        "NickelTypeFix " NH_VERSION ": %s", msg);
    if (m < 0) return;
    n = (size_t)(n + m) < sizeof(line_buf) ? n + m : (int)sizeof(line_buf) - 1;
    if (file) {
        m = snprintf(line_buf + n, sizeof(line_buf) - (size_t)n, " (%s:%d)", file, line);
        if (m > 0) n = (size_t)(n + m) < sizeof(line_buf) ? n + m : (int)sizeof(line_buf) - 1;
    }
    if ((size_t)n + 1 < sizeof(line_buf)) line_buf[n++] = '\n';

    pthread_mutex_lock(&ntf_log_buffer_lock);
    if (!buffered) {
        // Keep order: whatever is waiting belongs before this line.
        ntf_log_flush_locked();
        ntf_log_write_block(line_buf, (size_t)n);
        pthread_mutex_unlock(&ntf_log_buffer_lock);
        return;
    }
    if (ntf_log_buffer_used + (size_t)n > sizeof(ntf_log_buffer)
        || (ntf_log_buffer_since && now - ntf_log_buffer_since >= NTF_LOG_BUFFER_MAX_AGE_S))
        ntf_log_flush_locked();
    if ((size_t)n <= sizeof(ntf_log_buffer)) {
        if (!ntf_log_buffer_used) ntf_log_buffer_since = now;
        memcpy(ntf_log_buffer + ntf_log_buffer_used, line_buf, (size_t)n);
        ntf_log_buffer_used += (size_t)n;
    }
    pthread_mutex_unlock(&ntf_log_buffer_lock);
}

__attribute__((unused)) static inline void ntf_log_message(const char *file, int line, const char *msg) {
    ntf_log_message_ex(file, line, msg, false);
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
__attribute__((unused)) static inline void ntf_log_file_line_buffered(const char *file, int line, const char *fmt, ...) {
    char msg[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(msg, sizeof(msg), fmt, args);
    va_end(args);
    msg[sizeof(msg) - 1] = '\0';
    ntf_log_message_ex(file, line, msg, true);
}

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
// Verbose diagnostics: held in memory and written in blocks. Use for anything high-volume.
#define NTF_LOG_BUFFERED(fmt, ...) ntf_log_file_line_buffered(__FILE__, __LINE__, fmt, ##__VA_ARGS__)
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
