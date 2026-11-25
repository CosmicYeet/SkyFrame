#include "skyframe_orbit.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

/* An ISS element set (CelesTrak, 24 Nov 2026) and its positions from the reference SGP4
   (python-sgp4 2.27, WGS-72), minutes after the epoch -> TEME km. */
static const char *L1 = "1 25544U 98067A   26328.42887670  .00005116  00000+0  98982-4 0  9995";
static const char *L2 = "2 25544  51.6321 227.0974 0007395  60.4202 299.7523 15.49573206596592";
static const double REF[][4] = {
    {0.0, -4625.287085, -4976.951542, 0.002383},
    {45.0, 4902.385636, 4685.091543, 483.902580},
    {92.5, -4728.094813, -4877.967135, -132.491457},
    {360.0, -5503.909015, -1424.953912, -3732.907187},
    {1440.0, 4912.216124, 4695.513146, -234.188571},
    {4320.0, 5434.944542, 4034.308889, -648.184347},
    {-720.0, 3141.116923, -2809.000383, 5317.417431},
};

int main(void)
{
    sf_orbit_t o;
    assert(sf_orbit_parse(L1, L2, &o));
    double worst = 0;
    for (size_t i = 0; i < sizeof(REF) / sizeof(REF[0]); i++) {
        double r[3];
        assert(sf_orbit_position(&o, o.epoch_unix + REF[i][0] * 60.0, r));
        double d = sqrt(pow(r[0] - REF[i][1], 2) + pow(r[1] - REF[i][2], 2) + pow(r[2] - REF[i][3], 2));
        if (d > worst) worst = d;
        if (d > 0.05) { fprintf(stderr, "t=%+.1f min: off by %.3f km\n", REF[i][0], d); assert(0); }
    }

    /* The CelesTrak reply, with its name line and trailing spaces; broken sets are refused. */
    char text[256];
    snprintf(text, sizeof(text), "ISS (ZARYA)             \r\n%s\r\n%s\r\n", L1, L2);
    sf_orbit_t t;
    assert(sf_orbit_parse_text(text, strlen(text), &t) && t.epoch_unix == o.epoch_unix);
    char bad[80];
    snprintf(bad, sizeof(bad), "%s", L2);
    bad[20] = bad[20] == '1' ? '2' : '1';                   /* checksum no longer matches */
    assert(!sf_orbit_parse(L1, bad, &t));
    assert(!sf_orbit_parse_text("<html>", 6, &t));

    /* Passes from the sample point (39.15, -76.70) over three days: each one starts and ends
       visible (above 10 deg, sunlit, dark sky) and peaks as reported. */
    time_t from = (time_t)o.epoch_unix;
    int found = 0;
    for (int k = 0; k < 8; k++) {
        sf_pass_t p;
        if (!sf_orbit_next_pass(&o, 39.15, -76.70, from, 72, 10.0, &p)) break;
        assert(p.valid && p.end >= p.start && p.max_el_deg >= 10.0 && p.max_el_deg <= 90.0);
        sf_look_t a, b;
        assert(sf_orbit_look(&o, 39.15, -76.70, (double)p.start, &a) && sf_orbit_look(&o, 39.15, -76.70, (double)p.end, &b));
        assert(a.el_deg >= SF_PASS_MIN_EL && a.sunlit && a.sun_el_deg <= SF_PASS_DARK_SUN_EL);
        assert(b.el_deg >= SF_PASS_MIN_EL && b.sunlit && b.sun_el_deg <= SF_PASS_DARK_SUN_EL);
        assert(p.end - p.start < 15 * 60);                    /* an ISS pass lasts minutes */
        found++;
        from = p.end + 60;
    }
    assert(found > 0);                                         /* 39 N sees the ISS most weeks */

    assert(!strcmp(sf_compass_word(0), "north") && !strcmp(sf_compass_word(359), "north"));
    assert(!strcmp(sf_compass_word(44), "northeast") && !strcmp(sf_compass_word(315), "northwest"));
    assert(!strcmp(sf_compass_word(-90), "west") && !strcmp(sf_compass_word(180), "south"));
    printf("PASS: SGP4 matches the reference within %.1f m; TLE parsing; %d visible passes in 3 days\n", worst * 1000.0, found);
    return 0;
}
