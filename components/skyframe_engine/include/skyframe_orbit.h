/* Space station passes: the ISS's published orbit (a two-line element set, TLE) propagated
   with SGP4 (near-Earth), and the times it can be seen from a point on the ground: above the
   horizon, in sunlight, while the sky there is dark. Portable C, host-tested. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

typedef struct {
    /* from the TLE */
    double epoch_unix;          /* seconds since 1970 (UTC) */
    double bstar, inclo, nodeo, ecco, argpo, mo, no_kozai;   /* radians, rad/min */
    /* SGP4 state (sgp4init) */
    int isimp;
    double no, ao, con41, x1mth2, x7thm1, cc1, cc4, cc5, d2, d3, d4, delmo, eta, argpdot, omgcof,
           sinmao, t2cof, t3cof, t4cof, t5cof, xlcof, aycof, mdot, nodecf, nodedot, xmcof;
    bool valid;
} sf_orbit_t;

/* Parses the two element lines (line 1 starts "1 ", line 2 "2 "; checksums verified) and
   initialises SGP4. False for a malformed set or a deep-space orbit (period over 225 min). */
bool sf_orbit_parse(const char *line1, const char *line2, sf_orbit_t *out);
/* Parses a CelesTrak TLE reply (name line optional), e.g. "ISS (ZARYA)\n1 25544U ...\n2 ...". */
bool sf_orbit_parse_text(const char *text, size_t len, sf_orbit_t *out);
/* Position in the TEME frame (km) at a Unix time. False if the orbit has decayed. */
bool sf_orbit_position(const sf_orbit_t *o, double unix_time, double r_km[3]);

/* Where it is in the sky from (lat, lon in degrees) at a Unix time. */
typedef struct { double az_deg, el_deg, range_km; bool sunlit; double sun_el_deg; } sf_look_t;
bool sf_orbit_look(const sf_orbit_t *o, double lat, double lon, double unix_time, sf_look_t *out);

/* A visible pass: from when it appears (above MIN_EL, sunlit, dark sky) to when it fades or
   sets. Directions are compass degrees (0 = north, clockwise). */
#define SF_PASS_MIN_EL 10.0          /* below this, trees and houses hide it */
#define SF_PASS_DARK_SUN_EL -6.0     /* civil twilight: dark enough to see it */
typedef struct {
    time_t start, end;               /* Unix time (UTC) */
    double max_el_deg;
    double start_az_deg, end_az_deg;
    bool valid;
} sf_pass_t;
/* The first visible pass that starts after `from` (or is in progress), within `hours`, whose
   highest point is at least `min_peak_deg`. Steps of 10 s. */
bool sf_orbit_next_pass(const sf_orbit_t *o, double lat, double lon, time_t from, int hours,
                        double min_peak_deg, sf_pass_t *out);

/* "north", "northeast", ... for a compass bearing. */
const char *sf_compass_word(double az_deg);
