#pragma once
#include <stdbool.h>
#include <stddef.h>
/* Pure, host-testable URL construction; no device-private coordinates in code. */
bool sf_aircraft_url(char *out, size_t cap, double lat, double lon, double miles);
bool sf_weather_url(char *out, size_t cap, double lat, double lon);
