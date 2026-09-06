#define NTF_LOG_MAX_BYTES 4096
#define NTF_LOG_BUFFER_BYTES 512
#define NTF_LOG_BUFFER_MAX_AGE_S 3600
#define NH_VERSION "test"
#include "../../src/util.h"
#include <assert.h>
#include <errno.h>
#include <stdlib.h>
#include <unistd.h>

#define CURRENT NTF_CONFIG_DIR "/nickel-type-fix.log"
#define OLD CURRENT ".old"

bool ntf_log_setup_done;
bool ntf_log_prepend_newline;
char ntf_log_buffer[NTF_LOG_BUFFER_BYTES];
size_t ntf_log_buffer_used;
time_t ntf_log_buffer_since;
pthread_mutex_t ntf_log_buffer_lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned syslog_calls;

void nh_log(const char *fmt, ...) {
    (void)fmt;
    __sync_fetch_and_add(&syslog_calls, 1);
}

static void reset_log(void) {
    unlink(CURRENT);
    unlink(OLD);
    assert(mkdir(NTF_CONFIG_DIR, 0755) == 0 || errno == EEXIST);
    ntf_log_setup_done = false;
    ntf_log_prepend_newline = false;
    ntf_log_buffer_used = 0;
    ntf_log_buffer_since = 0;
}

static void append(const char *text, size_t size) {
    pthread_mutex_lock(&ntf_log_buffer_lock);
    ntf_log_write_block(text, size);
    pthread_mutex_unlock(&ntf_log_buffer_lock);
}

static off_t size_of(const char *path) {
    struct stat st;
    assert(stat(path, &st) == 0);
    return st.st_size;
}

static char *read_log(const char *path) {
    const size_t size = (size_t)size_of(path);
    char *text = malloc(size + 1);
    assert(text);
    FILE *f = fopen(path, "rb");
    assert(f);
    assert(fread(text, 1, size, f) == size);
    assert(fclose(f) == 0);
    text[size] = 0;
    return text;
}

static void boundaries_and_repeated_rotation(void) {
    reset_log();
    char block[NTF_LOG_MAX_BYTES + 2];
    memset(block, 's', sizeof(block));
    append(block, NTF_LOG_MAX_BYTES - 4);
    append("end\n", 4);
    assert(size_of(CURRENT) == NTF_LOG_MAX_BYTES);
    assert(access(OLD, F_OK) != 0);
    append("a\n", 2);
    assert(size_of(OLD) == NTF_LOG_MAX_BYTES && size_of(CURRENT) == 2);
    append(block, NTF_LOG_MAX_BYTES - 2);
    append("b\n", 2);
    char *old = read_log(OLD);
    assert(strncmp(old, "a\n", 2) == 0 && strlen(old) == NTF_LOG_MAX_BYTES);
    free(old);
    char *current = read_log(CURRENT);
    assert(strcmp(current, "b\n") == 0);
    free(current);
    append(block, sizeof(block));
    assert(size_of(CURRENT) == 2); // Even an oversized block cannot exceed the cap.
}

static void boot_separator_and_old_oversized_log(void) {
    reset_log();
    FILE *f = fopen(CURRENT, "w");
    assert(f && fputs("previous boot\n", f) >= 0 && fclose(f) == 0);
    append("first\n", 6);
    append("second\n", 7);
    char *text = read_log(CURRENT);
    assert(strcmp(text, "previous boot\n\nfirst\nsecond\n") == 0);
    free(text);

    reset_log();
    f = fopen(CURRENT, "w");
    assert(f);
    for (int i = 0; i < NTF_LOG_MAX_BYTES - 2; ++i) assert(fputc('s', f) != EOF);
    assert(fclose(f) == 0);
    append("x\n", 2); // The boot separator would make this one byte too large.
    assert(size_of(OLD) == NTF_LOG_MAX_BYTES - 2 && size_of(CURRENT) == 2);

    reset_log();
    f = fopen(CURRENT, "w");
    assert(f);
    for (int i = 0; i < NTF_LOG_MAX_BYTES + 100; ++i) assert(fputc('s', f) != EOF);
    assert(fclose(f) == 0);
    append("new\n", 4);
    assert(size_of(OLD) == NTF_LOG_MAX_BYTES + 100 && size_of(CURRENT) == 4);
}

static void buffered_order_and_rotation_failure(void) {
    reset_log();
    ntf_log_message_ex(NULL, 0, "buffered-a", true);
    ntf_log_message_ex(NULL, 0, "buffered-b", true);
    assert(access(CURRENT, F_OK) != 0);
    ntf_log_message(NULL, 0, "immediate");
    char *text = read_log(CURRENT);
    const char *a = strstr(text, "buffered-a");
    const char *b = strstr(text, "buffered-b");
    const char *c = strstr(text, "immediate");
    assert(a && b && c && a < b && b < c && ntf_log_buffer_used == 0);
    free(text);

    reset_log();
    char block[NTF_LOG_MAX_BYTES];
    memset(block, 's', sizeof(block));
    append(block, sizeof(block));
    assert(mkdir(OLD, 0755) == 0); // A file cannot replace a directory: rename must fail.
    unsigned before = syslog_calls;
    ntf_log_message_ex(NULL, 0, "buffered failure", true);
    ntf_log_message(NULL, 0, "immediate failure");
    assert(syslog_calls >= before + 2);
    text = read_log(CURRENT);
    assert(strlen(text) == sizeof(block) && memcmp(text, block, sizeof(block)) == 0);
    free(text);
    assert(rmdir(OLD) == 0);
    ntf_log_message(NULL, 0, "recovered");
    text = read_log(CURRENT);
    assert(strstr(text, "recovered") && size_of(OLD) == NTF_LOG_MAX_BYTES);
    free(text);
}

static void *writer(void *arg) {
    const int id = *(const int *)arg;
    for (int i = 0; i < 200; ++i)
        ntf_log_file_line_buffered(NULL, 0, "thread %d entry %d", id, i);
    return NULL;
}

static void concurrent_writes_and_uninstall(void) {
    reset_log();
    pthread_t threads[4];
    int ids[4] = {0, 1, 2, 3};
    const unsigned before = syslog_calls;
    for (int i = 0; i < 4; ++i) assert(pthread_create(&threads[i], NULL, writer, &ids[i]) == 0);
    for (int i = 0; i < 4; ++i) assert(pthread_join(threads[i], NULL) == 0);
    ntf_log_flush();
    assert(syslog_calls == before + 800);
    const char *paths[] = {CURRENT, OLD};
    for (int i = 0; i < 2; ++i) {
        assert(size_of(paths[i]) <= NTF_LOG_MAX_BYTES);
        FILE *f = fopen(paths[i], "r");
        assert(f);
        char line[256];
        while (fgets(line, sizeof(line), f)) {
            assert(strchr(line, '\n'));
            assert(strstr(line, "NickelTypeFix test: thread "));
        }
        assert(fclose(f) == 0);
    }
    assert(unlink(CURRENT) == 0 && unlink(OLD) == 0 && rmdir(NTF_CONFIG_DIR) == 0);
    ntf_log_message(NULL, 0, "after uninstall");
    assert(access(NTF_CONFIG_DIR, F_OK) != 0 && errno == ENOENT);
}

int main(void) {
    boundaries_and_repeated_rotation();
    boot_separator_and_old_oversized_log();
    buffered_order_and_rotation_failure();
    concurrent_writes_and_uninstall();
    puts("log rotation tests passed");
    return 0;
}
