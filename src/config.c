#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

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
    // written here whenever the config is missing.
    mkdir(NTF_CONFIG_DIR, 0755);

    FILE *dst = fopen(NTF_CONFIG_DIR "/config", "w");
    if (!dst) {
        NTF_LOG("warning: could not write default config to %s/config (%s)", NTF_CONFIG_DIR_DISP, strerror(errno));
        return;
    }
    fputs(ntf_default_config, dst);
    fclose(dst);
    NTF_LOG("wrote built-in default config to %s/config", NTF_CONFIG_DIR_DISP);
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
        for (size_t i = 0; ntf_known_keys[i]; i++)
            if (!strcmp(key, ntf_known_keys[i])) { known = true; break; }
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

static ntf_config_t *ntf_global_config(void) {
    static ntf_config_t *global = NULL;
    if (!global)
        global = ntf_config_parse();
    return global;
}

const char *ntf_global_config_get(const char *key) {
    return ntf_config_get(ntf_global_config(), key);
}

bool ntf_global_config_bool(const char *key, bool default_value) {
    return ntf_config_bool(ntf_global_config(), key, default_value);
}
