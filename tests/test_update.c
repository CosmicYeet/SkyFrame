#include "skyframe_update.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void) {
    /* Versions: numeric per part, so 0.5.10 is newer than 0.5.9. */
    assert(sf_version_compare("0.5.0", "0.5.0") == 0);
    assert(sf_version_compare("0.5.10", "0.5.9") > 0 && sf_version_compare("0.5.9", "0.5.10") < 0);
    assert(sf_version_compare("1.0.0", "0.99.99") > 0);
    assert(sf_version_compare("v0.6", "0.6.0") == 0 && sf_version_compare("0.6", "0.5.9") > 0);
    assert(sf_version_compare("junk", "0.0.0") == 0 && sf_version_compare("", "0.0.1") < 0);

    /* Manifest: version and an HTTPS url are required; notes are optional. */
    sf_update_manifest_t m;
    const char *good = "{\"version\":\"0.5.1\",\"url\":\"https://github.com/CosmicYeet/SkyFrame/releases/download/v0.5.1/SkyFrame.bin\","
                       "\"notes\":\"Smoother radar.\"}";
    assert(sf_update_parse_manifest(good, strlen(good), &m));
    assert(!strcmp(m.version, "0.5.1") && !strncmp(m.url, "https://github.com/", 19) && !strcmp(m.notes, "Smoother radar."));
    const char *no_notes = "{\"version\":\"0.5.2\",\"url\":\"https://example.com/SkyFrame.bin\"}";
    assert(sf_update_parse_manifest(no_notes, strlen(no_notes), &m) && !m.notes[0]);
    const char *http = "{\"version\":\"0.5.2\",\"url\":\"http://example.com/SkyFrame.bin\"}";
    assert(!sf_update_parse_manifest(http, strlen(http), &m) && !m.version[0]);
    const char *no_version = "{\"url\":\"https://example.com/SkyFrame.bin\"}";
    assert(!sf_update_parse_manifest(no_version, strlen(no_version), &m));
    const char *zero = "{\"version\":\"0.0.0\",\"url\":\"https://example.com/SkyFrame.bin\"}";
    assert(!sf_update_parse_manifest(zero, strlen(zero), &m));
    assert(!sf_update_parse_manifest("not json", 8, &m) && !sf_update_parse_manifest("{}", 2, &m));
    char long_version[64];
    snprintf(long_version, sizeof(long_version), "{\"version\":\"1.2.3456789012345678\",\"url\":\"https://e.com/x\"}");
    assert(!sf_update_parse_manifest(long_version, strlen(long_version), &m));   /* doesn't fit: refused, not cut */
    puts("PASS: update versions and release manifest");
    return 0;
}
