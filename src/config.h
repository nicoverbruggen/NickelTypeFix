#ifndef NTF_CONFIG_H
#define NTF_CONFIG_H
#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>

#if !(defined(NTF_CONFIG_DIR) && defined(NTF_CONFIG_DIR_DISP))
#error "NTF_CONFIG_DIR not set (it should be done by the Makefile)"
#endif

// The built-in default config, written to <config-dir>/config when it's missing.
// Defined in the mod's main source (nickeltypefix.cc) so it lives next to the documented keys.
extern const char *const ntf_default_config;

// Null-terminated list of the valid config keys (defined next to ntf_default_config so the two
// stay in sync). The parser warns about any key not in this list.
extern const char *const ntf_known_keys[];

// True once any config problem was seen (unknown key, malformed line, invalid value). A broken
// config forces verbose logging for the boot, so it diagnoses itself in the log.
bool ntf_config_problem_seen(void);

typedef struct ntf_config_t ntf_config_t;

ntf_config_t *ntf_config_parse(void);
const char *ntf_config_get(ntf_config_t *cfg, const char *key);
bool ntf_config_bool(ntf_config_t *cfg, const char *key, bool default_value);
void ntf_config_free(ntf_config_t *cfg);
const char *ntf_global_config_get(const char *key);
bool ntf_global_config_bool(const char *key, bool default_value);

#ifdef __cplusplus
}
#endif
#endif
