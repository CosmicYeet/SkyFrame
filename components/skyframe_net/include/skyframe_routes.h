/* Flight routes from adsb.lol's route files (vrs-standing-data.adsb.lol, community-kept, by
   callsign), which list every stop of a multi-stop flight: "DAL-PHX-LAS". Portable and
   host-tested. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include "skyframe_engine.h"

/* "https://vrs-standing-data.adsb.lol/routes/SW/SWA2140.json"; false for an unusable callsign. */
bool sf_route_url(char *out, size_t cap, const char *callsign);
/* The leg being flown. When `home` (an IATA code, "BWI") is a stop, the leg through it: a plane
   climbing (climb > 0) has just left it, one descending (climb < 0) is coming in; unknown (0)
   counts as arriving. Otherwise the whole route, first stop to last. */
bool sf_parse_route(const char *json, size_t len, const char *home, int climb, sf_airport_t *from, sf_airport_t *to);
