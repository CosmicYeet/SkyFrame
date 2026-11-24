/* Portable part of the updater: version comparison and the release manifest. No ESP-IDF. */
#include "skyframe_update.h"
#include "cJSON.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void parts(const char *v, long out[3])
{
    out[0] = out[1] = out[2] = 0;
    if (!v) return;
    if (*v == 'v' || *v == 'V') v++;
    for (int i = 0; i < 3 && *v; i++) {
        char *end;
        out[i] = strtol(v, &end, 10);
        if (end == v) { out[i] = 0; return; }
        v = end;
        if (*v != '.') return;
        v++;
    }
}
int sf_version_compare(const char *a, const char *b)
{
    long x[3], y[3];
    parts(a, x);
    parts(b, y);
    for (int i = 0; i < 3; i++) if (x[i] != y[i]) return x[i] < y[i] ? -1 : 1;
    return 0;
}

static bool copy_string(cJSON *root, const char *key, char *dst, size_t cap, bool required)
{
    cJSON *v = cJSON_GetObjectItemCaseSensitive(root, key);
    if (!cJSON_IsString(v) || !v->valuestring || !v->valuestring[0] || strlen(v->valuestring) >= cap) {
        dst[0] = 0;
        return !required;
    }
    snprintf(dst, cap, "%s", v->valuestring);
    return true;
}
bool sf_update_parse_manifest(const char *json, size_t len, sf_update_manifest_t *out)
{
    if (!json || !out) return false;
    memset(out, 0, sizeof(*out));
    cJSON *root = cJSON_ParseWithLength(json, len);
    if (!root) return false;
    bool ok = copy_string(root, "version", out->version, sizeof(out->version), true) &&
              copy_string(root, "url", out->url, sizeof(out->url), true);
    copy_string(root, "notes", out->notes, sizeof(out->notes), false);
    cJSON_Delete(root);
    /* Only firmware over HTTPS, and a version that parses (not "0.0.0"). */
    if (ok && (strncmp(out->url, "https://", 8) || sf_version_compare(out->version, "0.0.0") <= 0)) ok = false;
    if (!ok) memset(out, 0, sizeof(*out));
    return ok;
}
