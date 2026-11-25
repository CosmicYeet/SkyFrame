#include "skyframe_urls.h"
#include <assert.h>
#include <string.h>
#include <stdio.h>
int main(void){
 char b[512];
 assert(sf_aircraft_url(b,sizeof(b),39.1,-76.6,10.0));
 assert(strstr(b,"/dist/8.7")!=NULL);
 assert(strstr(b,"/lat/39.10000/lon/-76.60000/")!=NULL);
 assert(sf_weather_url(b,sizeof(b),39.1,-76.6));
 assert(strstr(b,"temperature_unit=fahrenheit")!=NULL);
 assert(strstr(b,"timezone=auto")!=NULL);
 assert(!sf_aircraft_url(b,15,39,-76,10));
 assert(!sf_aircraft_url(b,sizeof(b),100,-76,10));
 assert(!sf_aircraft_url(b,sizeof(b),39,-76,0));
 assert(!sf_weather_url(b,sizeof(b),39,200));
 /* Satellite backdrop: 39.15,-76.70, 11.76 mi to each edge (the 10 mi radar's 480 px). */
 assert(sf_satellite_url(b,sizeof(b),39.15,-76.70,10.0*240/204,480));
 assert(strstr(b,"https://basemap.nationalmap.gov/arcgis/rest/services/USGSImageryOnly/MapServer/export?")==b);
 assert(strstr(b,"bbox=-8562620,4718766,-8513790,4767595&")!=NULL);
 assert(strstr(b,"&size=480,480&format=jpg&f=image")!=NULL);
 assert(!sf_satellite_url(b,sizeof(b),85,0,10,480));      /* Mercator breaks down near the poles */
 assert(!sf_satellite_url(b,sizeof(b),39,-76,0,480));
 assert(!sf_satellite_url(b,40,39,-76,10,480));
 puts("PASS: API URLs, miles->nautical miles, bounds and buffer safety");return 0;
}
