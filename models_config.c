/*
 * models_config.c
 *
 * Parse ~/.cop/models.json into model_entry_t array.
 */

#include "models_config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* ============================================================================
 * Helpers
 * ============================================================================ */

static char *expand_path(const char *path) {
    if (!path) return NULL;
    if (path[0] == '~') {
        const char *home = getenv("HOME");
        if (!home) home = "/tmp";
        size_t len = strlen(home) + strlen(path);
        char *result = malloc(len);
        if (result) snprintf(result, len, "%s%s", home, path + 1);
        return result;
    }
    return strdup(path);
}

/* Read entire file into a string. Caller must free. */
static char *read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (sz <= 0) { fclose(f); return NULL; }

    char *buf = malloc(sz + 1);
    if (!buf) { fclose(f); return NULL; }

    size_t n = fread(buf, 1, sz, f);
    fclose(f);
    buf[n] = '\0';
    return buf;
}

/* ============================================================================
 * Public API
 * ============================================================================ */

model_entry_t **models_config_load(void) {
    char *path = expand_path("~/.cop/models.json");
    if (!path) return NULL;

    char *json_str = read_file(path);
    free(path);

    if (!json_str) {
        fprintf(stderr, "[models] cannot read ~/.cop/models.json\n");
        return NULL;
    }

    cJSON *root = cJSON_Parse(json_str);
    free(json_str);

    if (!root) {
        fprintf(stderr, "[models] invalid JSON in ~/.cop/models.json\n");
        return NULL;
    }

    cJSON *providers = cJSON_GetObjectItem(root, "providers");
    if (!providers) {
        fprintf(stderr, "[models] missing 'providers' key\n");
        cJSON_Delete(root);
        return NULL;
    }

    /* First pass: count total models */
    int total = 0;
    cJSON *prov_iter = providers->child;
    while (prov_iter) {
        cJSON *models = cJSON_GetObjectItem(prov_iter, "models");
        if (models && cJSON_IsArray(models)) {
            total += cJSON_GetArraySize(models);
        }
        prov_iter = prov_iter->next;
    }

    /* Allocate array (NULL-terminated) */
    model_entry_t **entries = calloc(total + 1, sizeof(model_entry_t *));
    if (!entries) {
        cJSON_Delete(root);
        return NULL;
    }

    /* Second pass: populate entries */
    int idx = 0;
    prov_iter = providers->child;
    while (prov_iter) {
        const char *prov_name = prov_iter->string;

        cJSON *base_url = cJSON_GetObjectItem(prov_iter, "baseUrl");
        cJSON *api_key  = cJSON_GetObjectItem(prov_iter, "apiKey");

        const char *url = (base_url && cJSON_IsString(base_url))
                          ? base_url->valuestring : "";
        const char *key = (api_key && cJSON_IsString(api_key))
                          ? api_key->valuestring : "";

        cJSON *models = cJSON_GetObjectItem(prov_iter, "models");
        if (models && cJSON_IsArray(models)) {
            int n = cJSON_GetArraySize(models);
            for (int i = 0; i < n; i++) {
                cJSON *m   = cJSON_GetArrayItem(models, i);
                cJSON *mid = cJSON_GetObjectItem(m, "id");
                cJSON *cw  = cJSON_GetObjectItem(m, "contextWindow");
                cJSON *mt  = cJSON_GetObjectItem(m, "maxTokens");

                if (!mid || !cJSON_IsString(mid)) continue;

                model_entry_t *entry = calloc(1, sizeof(model_entry_t));
                entry->provider       = strdup(prov_name);
                entry->base_url       = strdup(url);
                entry->api_key        = strdup(key);
                entry->model_id       = strdup(mid->valuestring);
                entry->context_window = (cw && cJSON_IsNumber(cw)) ? cw->valueint : 0;
                entry->max_tokens     = (mt && cJSON_IsNumber(mt)) ? mt->valueint : 0;

                entries[idx++] = entry;
            }
        }
        prov_iter = prov_iter->next;
    }

    entries[idx] = NULL;  /* NULL terminator */
    cJSON_Delete(root);
    return entries;
}

void models_config_free(model_entry_t **entries) {
    if (!entries) return;
    for (int i = 0; entries[i]; i++) {
        free(entries[i]->provider);
        free(entries[i]->base_url);
        free(entries[i]->api_key);
        free(entries[i]->model_id);
        free(entries[i]);
    }
    free(entries);
}

const model_entry_t *models_config_find(model_entry_t **entries, const char *model_id) {
    if (!entries || !model_id) return NULL;
    for (int i = 0; entries[i]; i++) {
        if (strcmp(entries[i]->model_id, model_id) == 0)
            return entries[i];
    }
    return NULL;
}

int models_config_count(model_entry_t **entries) {
    if (!entries) return 0;
    int n = 0;
    while (entries[n]) n++;
    return n;
}

const model_entry_t *models_config_load_saved(model_entry_t **entries) {
    if (!entries) return NULL;

    const char *home = getenv("HOME");
    if (!home) home = "/tmp";

    char path[512];
    snprintf(path, sizeof(path), "%s/.cop/model_set", home);

    FILE *f = fopen(path, "r");
    if (!f) return NULL;

    char model_id[256];
    const model_entry_t *result = NULL;
    if (fgets(model_id, sizeof(model_id), f)) {
        /* Strip trailing newline / carriage return */
        size_t len = strlen(model_id);
        while (len > 0 && (model_id[len-1] == '\n' || model_id[len-1] == '\r'))
            model_id[--len] = '\0';
        result = models_config_find(entries, model_id);
    }
    fclose(f);
    return result;
}

int models_config_save_model(const char *model_id) {
    if (!model_id) return -1;

    const char *home = getenv("HOME");
    if (!home) home = "/tmp";

    /* Ensure ~/.cop/ exists */
    char cop_dir[512];
    snprintf(cop_dir, sizeof(cop_dir), "%s/.cop", home);
    mkdir(cop_dir, 0755);

    char path[512];
    snprintf(path, sizeof(path), "%s/.cop/model_set", home);

    FILE *f = fopen(path, "w");
    if (!f) return -1;

    fprintf(f, "%s\n", model_id);
    fclose(f);
    return 0;
}
