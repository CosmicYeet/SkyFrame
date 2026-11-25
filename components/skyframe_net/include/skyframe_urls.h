#pragma once
#include <stdbool.h>
#include <stddef.h>
/* Pure, host-testable URL construction; no device-private coordinates in code. */
bool sf_aircraft_url(char *out, size_t cap, double lat, double lon, double miles);
bool sf_weather_url(char *out, size_t cap, double lat, double lon);
/* The radar's satellite backdrop: a size x size image (USGS National Map imagery, public
   domain, US only) centered on lat/lon, `half_miles` from the center to each edge, in Web
   Mercator scaled so distances match the radar's. */
bool sf_satellite_url(char *out, size_t cap, double lat, double lon, double half_miles, int size);
