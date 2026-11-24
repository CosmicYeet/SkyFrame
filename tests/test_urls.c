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
 puts("PASS: API URLs, miles->nautical miles, bounds and buffer safety");return 0;
}
