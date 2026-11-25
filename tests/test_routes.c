#include "skyframe_routes.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static const char *TWO =
    "{\"callsign\":\"SWA2140\",\"_airport_codes_iata\":\"AUS-BWI\",\"_airports\":["
    "{\"name\":\"Austin Bergstrom International Airport\",\"icao\":\"KAUS\",\"iata\":\"AUS\",\"location\":\"Austin\"},"
    "{\"name\":\"Baltimore/Washington International Thurgood Marshal Airport\",\"icao\":\"KBWI\",\"iata\":\"BWI\",\"location\":\"Baltimore\"}]}";
static const char *THREE =
    "{\"callsign\":\"SWA1234\",\"_airports\":["
    "{\"name\":\"Dallas Love Field\",\"icao\":\"KDAL\",\"iata\":\"DAL\",\"location\":\"Dallas\"},"
    "{\"name\":\"Baltimore/Washington\",\"icao\":\"KBWI\",\"iata\":\"BWI\",\"location\":\"Baltimore\"},"
    "{\"name\":\"Las Vegas Harry Reid\",\"icao\":\"KLAS\",\"iata\":\"LAS\",\"location\":\"Las Vegas\"}]}";

int main(void)
{
    char url[120];
    assert(sf_route_url(url, sizeof(url), "swa2140"));
    assert(!strcmp(url, "https://vrs-standing-data.adsb.lol/routes/SW/SWA2140.json"));
    assert(!sf_route_url(url, sizeof(url), "AB"));            /* too short */
    assert(!sf_route_url(url, sizeof(url), "N12 3AB"));       /* not a callsign */
    assert(!sf_route_url(url, 20, "SWA2140"));

    sf_airport_t a, b;
    assert(sf_parse_route(TWO, strlen(TWO), "BWI", -1, &a, &b));
    assert(!strcmp(a.code, "AUS") && !strcmp(b.code, "BWI") && !strcmp(a.city, "Austin") && !strcmp(b.city, "Baltimore"));
    /* Multi-stop through BWI: the leg depends on whether the plane is leaving or arriving. */
    assert(sf_parse_route(THREE, strlen(THREE), "BWI", +1, &a, &b) && !strcmp(a.code, "BWI") && !strcmp(b.code, "LAS"));
    assert(sf_parse_route(THREE, strlen(THREE), "BWI", -1, &a, &b) && !strcmp(a.code, "DAL") && !strcmp(b.code, "BWI"));
    assert(sf_parse_route(THREE, strlen(THREE), "BWI", 0, &a, &b) && !strcmp(a.code, "DAL") && !strcmp(b.code, "BWI"));
    /* Not through the home airport: first stop to last. */
    assert(sf_parse_route(THREE, strlen(THREE), "MDW", +1, &a, &b) && !strcmp(a.code, "DAL") && !strcmp(b.code, "LAS"));
    /* Home at an end: that leg, whatever the climb. */
    assert(sf_parse_route(THREE, strlen(THREE), "DAL", -1, &a, &b) && !strcmp(a.code, "DAL") && !strcmp(b.code, "BWI"));
    /* Broken or empty replies. */
    assert(!sf_parse_route("{\"_airports\":[]}", 16, "BWI", 0, &a, &b));
    assert(!sf_parse_route("not json", 8, "BWI", 0, &a, &b));
    puts("PASS: route files: address, legs through the home airport, refusals");
    return 0;
}
