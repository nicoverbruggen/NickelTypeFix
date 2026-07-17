#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "config.h"
#include "util.h"

typedef struct ntf_config_entry_t {
    char *key;
    char *val;
    struct ntf_config_entry_t *next;
} ntf_config_entry_t;

struct ntf_config_t {
    ntf_config_entry_t *head;
    ntf_config_entry_t *tail;
};

// Set on any config problem (unknown key, malformed line, invalid value); read by ntf_log() so a
// broken config turns verbose logging on for the whole boot and diagnoses itself in the log.
static bool ntf_config_problem = false;
bool ntf_config_problem_seen(void) {
    return ntf_config_problem;
}

static void ntf_config_append(ntf_config_t *cfg, const char *key, const char *val) {
    ntf_config_entry_t *e = (ntf_config_entry_t*)calloc(1, sizeof(ntf_config_entry_t));
    if (!e || !(e->key = strdup(key)) || !(e->val = strdup(val))) {
        NTF_LOG("warning: out of memory while parsing config, skipping '%s'", key);
        if (e) {
            free(e->key);
            free(e->val);
            free(e);
        }
        return;
    }

    if (cfg->tail)
        cfg->tail->next = e;
    else
        cfg->head = e;
    cfg->tail = e;
}

static void ntf_config_write_default(void) {
    // No shipped 'default' file — the default lives in the code (ntf_default_config) and is
    // written here whenever the config is missing. Write a unique sibling first, flush it, and
    // rename it into place (the same pattern as the safety marker): the config file's existence
    // doubles as the not-a-first-install signal, so a power cut mid-write must not be able to
    // leave a truncated config behind.
    if (mkdir(NTF_CONFIG_DIR, 0755) != 0 && errno != EEXIST) {
        NTF_LOG("warning: could not create %s (%s)", NTF_CONFIG_DIR_DISP, strerror(errno));
        return;
    }

    char tmp[1024];
    int n = snprintf(tmp, sizeof(tmp), NTF_CONFIG_DIR "/config.tmp.%ld", (long)getpid());
    if (n < 0 || (size_t)n >= sizeof(tmp)) {
        NTF_LOG("warning: default config path is too long");
        return;
    }
    int fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0644);
    if (fd < 0) {
        NTF_LOG("warning: could not write default config to %s/config (%s)", NTF_CONFIG_DIR_DISP, strerror(errno));
        return;
    }
    FILE *dst = fdopen(fd, "w");
    if (!dst) {
        int saved_errno = errno;
        close(fd);
        unlink(tmp);
        NTF_LOG("warning: could not write default config to %s/config (%s)", NTF_CONFIG_DIR_DISP, strerror(saved_errno));
        return;
    }

    bool ok = fputs(ntf_default_config, dst) >= 0;
    if (ok && fflush(dst) != 0) ok = false;
    if (ok && fsync(fileno(dst)) != 0) ok = false;
    if (fclose(dst) != 0) ok = false;
    if (!ok) {
        NTF_LOG("warning: could not flush default config to %s/config (%s)", NTF_CONFIG_DIR_DISP, strerror(errno));
        unlink(tmp);
        return;
    }
    if (rename(tmp, NTF_CONFIG_DIR "/config") != 0) {
        NTF_LOG("warning: could not install default config at %s/config (%s)", NTF_CONFIG_DIR_DISP, strerror(errno));
        unlink(tmp);
        return;
    }
    // Best effort: make the rename itself durable. A failure here only means the
    // default config could vanish on a power cut and be rewritten next boot.
    int dir_fd = open(NTF_CONFIG_DIR, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (dir_fd >= 0) {
        fsync(dir_fd);
        close(dir_fd);
    }
    NTF_LOG("wrote built-in default config to %s/config", NTF_CONFIG_DIR_DISP);
}

// Self-heal: after loading an existing config, append any known keys the file is missing (keys a
// newer version added, so an upgrading user never got them). The user's file is copied byte-for-byte
// and only the missing keys are appended, each with its baked-in default from ntf_config_keys and a
// short comment. Existing lines are never modified, reordered, or rewritten, so a user's own values
// are preserved exactly. If nothing is missing, nothing is written. The write is atomic (unique temp
// file, flushed, then renamed into place), mirroring ntf_config_write_default() so a power cut can't
// leave a truncated config behind.
static void ntf_config_append_missing(ntf_config_t *cfg) {
    // Parse what's present first: a key already in the loaded config is not missing.
    bool any_missing = false;
    for (size_t i = 0; ntf_config_keys[i].key; i++)
        if (!ntf_config_get(cfg, ntf_config_keys[i].key)) { any_missing = true; break; }
    if (!any_missing)
        return;

    char tmp[1024];
    int n = snprintf(tmp, sizeof(tmp), NTF_CONFIG_DIR "/config.tmp.%ld", (long)getpid());
    if (n < 0 || (size_t)n >= sizeof(tmp)) {
        NTF_LOG("warning: config path is too long to add missing keys");
        return;
    }

    // Reopen the file to copy it verbatim, so the appended block is added to the exact bytes on disk.
    FILE *src = fopen(NTF_CONFIG_DIR "/config", "r");
    if (!src) {
        NTF_LOG("warning: could not reopen %s/config to add missing keys (%s)", NTF_CONFIG_DIR_DISP, strerror(errno));
        return;
    }

    int fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0644);
    if (fd < 0) {
        NTF_LOG("warning: could not add missing keys to %s/config (%s)", NTF_CONFIG_DIR_DISP, strerror(errno));
        fclose(src);
        return;
    }
    FILE *dst = fdopen(fd, "w");
    if (!dst) {
        int saved_errno = errno;
        close(fd);
        unlink(tmp);
        fclose(src);
        NTF_LOG("warning: could not add missing keys to %s/config (%s)", NTF_CONFIG_DIR_DISP, strerror(saved_errno));
        return;
    }

    // Copy the user's file unchanged. Track the last byte so the appended block starts on its own
    // line even if the file did not end with a newline.
    bool ok = true;
    char cpbuf[4096];
    size_t r;
    int last = '\n';
    while ((r = fread(cpbuf, 1, sizeof(cpbuf), src)) > 0) {
        last = (unsigned char)cpbuf[r - 1];
        if (fwrite(cpbuf, 1, r, dst) != r) { ok = false; break; }
    }
    if (ok && ferror(src)) ok = false;
    fclose(src);

    if (ok && last != '\n' && fputc('\n', dst) == EOF) ok = false;
    if (ok && fputs("\n# --- keys added by a newer NickelTypeFix version (defaults shown; edit to disable) ---\n", dst) < 0) ok = false;
    for (size_t i = 0; ok && ntf_config_keys[i].key; i++) {
        if (ntf_config_get(cfg, ntf_config_keys[i].key))
            continue;
        if (fprintf(dst, "# %s\n%s:%s\n", ntf_config_keys[i].comment, ntf_config_keys[i].key, ntf_config_keys[i].value) < 0)
            ok = false;
    }

    if (ok && fflush(dst) != 0) ok = false;
    if (ok && fsync(fileno(dst)) != 0) ok = false;
    if (fclose(dst) != 0) ok = false;
    if (!ok) {
        NTF_LOG("warning: could not add missing keys to %s/config (%s)", NTF_CONFIG_DIR_DISP, strerror(errno));
        unlink(tmp);
        return;
    }
    if (rename(tmp, NTF_CONFIG_DIR "/config") != 0) {
        NTF_LOG("warning: could not install updated %s/config (%s)", NTF_CONFIG_DIR_DISP, strerror(errno));
        unlink(tmp);
        return;
    }
    // Best effort: make the rename durable, same as the default-config write.
    int dir_fd = open(NTF_CONFIG_DIR, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (dir_fd >= 0) {
        fsync(dir_fd);
        close(dir_fd);
    }
    NTF_LOG("added missing keys to %s/config", NTF_CONFIG_DIR_DISP);
}

ntf_config_t *ntf_config_parse(void) {
    ntf_config_t *cfg = (ntf_config_t*)calloc(1, sizeof(ntf_config_t));
    if (!cfg)
        return NULL;

    FILE *f = fopen(NTF_CONFIG_DIR "/config", "r");
    if (!f && errno == ENOENT) {
        NTF_LOG("no config file at %s/config; writing a default one", NTF_CONFIG_DIR_DISP);
        ntf_config_write_default();
        f = fopen(NTF_CONFIG_DIR "/config", "r");
    }
    if (!f) {
        NTF_LOG("could not open %s/config (%s); using built-in defaults", NTF_CONFIG_DIR_DISP, strerror(errno));
        return cfg;
    }

    char *buf = NULL;
    size_t bufsz = 0;
    ssize_t len;
    int lineno = 0;
    while ((len = getline(&buf, &bufsz, f)) != -1) {
        (void)len;
        lineno++;

        char *hash = strchr(buf, '#');
        if (hash)
            *hash = '\0';

        char *line = strtrim(buf);
        if (!*line)
            continue;

        char *cur = line;
        char *key = strsep(&cur, ":");
        key = strtrim(key);
        if (!key || !*key) {
            ntf_config_problem = true;
            NTF_LOG("warning: %s/config: line %d: expected key, ignoring line", NTF_CONFIG_DIR_DISP, lineno);
            continue;
        }
        if (!cur) {
            ntf_config_problem = true;
            NTF_LOG("warning: %s/config: line %d: expected ':' after key '%s', ignoring line", NTF_CONFIG_DIR_DISP, lineno, key);
            continue;
        }

        bool known = false;
        for (size_t i = 0; ntf_config_keys[i].key; i++)
            if (!strcmp(key, ntf_config_keys[i].key)) { known = true; break; }
        if (!known) {
            ntf_config_problem = true;
            NTF_LOG("warning: %s/config: line %d: unknown setting '%s' (it does nothing — likely a typo; the doc file lists the valid settings)", NTF_CONFIG_DIR_DISP, lineno, key);
        }

        char *val = strtrim(cur);
        ntf_config_append(cfg, key, val);
    }

    free(buf);
    fclose(f);

    // Echo the parsed keys only under verbose logging — or when the config has a problem, so a
    // broken config always shows what was actually parsed. A healthy boot with ntf_log:0 writes
    // nothing. Read the flag from the config just parsed — NTF_DBG (ntf_global_config_bool) can't
    // be used mid-parse, since the global config is what this function is building and asking for
    // it would recurse into this parser.
    if (ntf_config_bool(cfg, "ntf_log", false) || ntf_config_problem)
        for (ntf_config_entry_t *e = cfg->head; e; e = e->next)
            NTF_LOG("config: %s = %s", e->key, e->val);

    // The file existed and was parsed (this is the normal load path as well as right after a fresh
    // default was written). Top it up with any keys a newer version added but this file lacks. On a
    // fresh write the template already has them all, so this is a no-op there.
    ntf_config_append_missing(cfg);
    return cfg;
}

const char *ntf_config_get(ntf_config_t *cfg, const char *key) {
    if (!cfg)
        return NULL;
    for (ntf_config_entry_t *e = cfg->head; e; e = e->next)
        if (!strcmp(e->key, key))
            return e->val;
    return NULL;
}

bool ntf_config_bool(ntf_config_t *cfg, const char *key, bool default_value) {
    const char *val = ntf_config_get(cfg, key);
    if (!val || !*val)
        return default_value;
    if (!strcmp(val, "1") || !strcasecmp(val, "true") || !strcasecmp(val, "yes") || !strcasecmp(val, "on"))
        return true;
    if (!strcmp(val, "0") || !strcasecmp(val, "false") || !strcasecmp(val, "no") || !strcasecmp(val, "off"))
        return false;

    ntf_config_problem = true;
    NTF_LOG("warning: invalid boolean for '%s': '%s'; using default %d", key, val, default_value ? 1 : 0);
    return default_value;
}

void ntf_config_free(ntf_config_t *cfg) {
    if (!cfg)
        return;

    ntf_config_entry_t *e = cfg->head;
    while (e) {
        ntf_config_entry_t *next = e->next;
        free(e->key);
        free(e->val);
        free(e);
        e = next;
    }
    free(cfg);
}

// The global config is primed from ntf_init before any hook is installed, so
// hook-thread readers normally see an already-published pointer. pthread_once
// removes the reliance on that ordering: even a hook that somehow ran first
// (or a future refactor of init) gets exactly one, fully built config instead
// of a lazy-init race.
static ntf_config_t *ntf_global_cfg = NULL;
static pthread_once_t ntf_global_cfg_once = PTHREAD_ONCE_INIT;
static void ntf_global_config_init(void) {
    ntf_global_cfg = ntf_config_parse();
}
static ntf_config_t *ntf_global_config(void) {
    pthread_once(&ntf_global_cfg_once, ntf_global_config_init);
    return ntf_global_cfg;
}

const char *ntf_global_config_get(const char *key) {
    return ntf_config_get(ntf_global_config(), key);
}

bool ntf_global_config_bool(const char *key, bool default_value) {
    return ntf_config_bool(ntf_global_config(), key, default_value);
}
