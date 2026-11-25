/* Flight routes from adsb.lol: see skyframe_routes.h. */
#include "skyframe_routes.h"
#include "cJSON.h"
#include <ctype.h>
#include <stdio.h>
#include <string.h>

bool sf_route_url(char *out, size_t cap, const char *callsign)
{
    char cs[9];
    size_t n = callsign ? strlen(callsign) : 0;
    if (!out || !cap || n < 3 || n > 8) return false;
    for (size_t i = 0; i < n; i++) {
        if (!isalnum((unsigned char)callsign[i])) return false;
        cs[i] = (char)toupper((unsigned char)callsign[i]);
    }
    cs[n] = 0;
    int k = snprintf(out, cap, "https://vrs-standing-data.adsb.lol/routes/%.2s/%s.json", cs, cs);
    return k > 0 && (size_t)k < cap;
}

static const char *str_of(const cJSON *o, const char *key)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    return cJSON_IsString(v) && v->valuestring ? v->valuestring : "";
}
static void airport_of(const cJSON *a, sf_airport_t *out)
{
    memset(out, 0, sizeof(*out));
    const char *iata = str_of(a, "iata");
    snprintf(out->code, sizeof(out->code), "%s", strlen(iata) == 3 ? iata : str_of(a, "icao"));
    snprintf(out->name, sizeof(out->name), "%s", str_of(a, "name"));
    snprintf(out->city, sizeof(out->city), "%s", str_of(a, "location"));
}

bool sf_parse_route(const char *json, size_t len, const char *home, int climb, sf_airport_t *from, sf_airport_t *to)
{
    if (!json || !from || !to) return false;
    cJSON *root = cJSON_ParseWithLength(json, len);
    if (!root) return false;
    const cJSON *stops = cJSON_GetObjectItemCaseSensitive(root, "_airports");
    int n = cJSON_IsArray(stops) ? cJSON_GetArraySize(stops) : 0;
    bool ok = false;
    if (n >= 2) {
        int first = 0, last = n - 1;
        for (int i = 0; home && home[0] && i < n; i++) {
            if (strcmp(str_of(cJSON_GetArrayItem(stops, i), "iata"), home)) continue;
            if (i == 0) { first = 0; last = 1; }                     /* starts here */
            else if (i == n - 1) { first = n - 2; last = n - 1; }    /* ends here */
            else if (climb > 0) { first = i; last = i + 1; }         /* just left */
            else { first = i - 1; last = i; }                        /* coming in */
            break;
        }
        airport_of(cJSON_GetArrayItem(stops, first), from);
        airport_of(cJSON_GetArrayItem(stops, last), to);
        ok = from->code[0] && to->code[0];
    }
    cJSON_Delete(root);
    return ok;
}
