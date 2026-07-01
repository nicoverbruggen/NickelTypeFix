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
            NTF_LOG("warning: %s/config: line %d: expected key, ignoring line", NTF_CONFIG_DIR_DISP, lineno);
            continue;
        }
        if (!cur) {
            NTF_LOG("warning: %s/config: line %d: expected ':' after key '%s', ignoring line", NTF_CONFIG_DIR_DISP, lineno, key);
            continue;
        }

        char *val = strtrim(cur);
        ntf_config_append(cfg, key, val);
        NTF_LOG("config: %s = %s", key, val);
    }

    free(buf);
    fclose(f);
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

    NTF_LOG("warning: invalid boolean for '%s': '%s'; using default %d", key, val, default_value ? 1 : 0);
    return default_value;
}

double ntf_config_double(ntf_config_t *cfg, const char *key, double default_value) {
    const char *val = ntf_config_get(cfg, key);
    if (!val || !*val)
        return default_value;

    char *end = NULL;
    errno = 0;
    double parsed = strtod(val, &end);
    bool trailing = false;
    for (const char *p = end; p && *p; p++)
        if (!isspace((unsigned char)*p)) { trailing = true; break; }
    if (errno || end == val || trailing) {
        NTF_LOG("warning: invalid number for '%s': '%s'; using default %.4f", key, val, default_value);
        return default_value;
    }
    return parsed;
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

double ntf_global_config_double(const char *key, double default_value) {
    return ntf_config_double(ntf_global_config(), key, default_value);
}
