/*
 * models_config.h
 *
 * Parse and query ~/.agent/models.json for LLM provider configuration.
 */

#ifndef MODELS_CONFIG_H
#define MODELS_CONFIG_H

#include "cjson/cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Parsed model entry.
 */
typedef struct {
    char *provider;       /* provider name, e.g. "deepseek" */
    char *base_url;       /* e.g. "https://api.deepseek.com" */
    char *api_key;        /* provider API key */
    char *model_id;       /* e.g. "deepseek-v4-pro" */
    int   context_window; /* max context window size */
    int   max_tokens;     /* max tokens per response (0 = use API default) */
} model_entry_t;

/*
 * Load models config from ~/.agent/models.json.
 * Returns a NULL-terminated array of model_entry_t pointers.
 * Caller must free with models_config_free().
 * Returns NULL on error (check stderr).
 */
model_entry_t **models_config_load(void);

/*
 * Free a models array returned by models_config_load().
 */
void models_config_free(model_entry_t **entries);

/*
 * Find a model by id. Returns NULL if not found.
 */
const model_entry_t *models_config_find(model_entry_t **entries, const char *model_id);

/*
 * Count entries in the array.
 */
int models_config_count(model_entry_t **entries);

#ifdef __cplusplus
}
#endif

#endif /* MODELS_CONFIG_H */
