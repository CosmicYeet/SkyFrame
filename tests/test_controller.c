#include "skyframe_controller.h"
#include <assert.h>
#include <stdio.h>
int main(void){
    sf_controller_t c;sf_controller_init(&c,0);
    sf_aircraft_result_t data={0};data.api_ok=true;
    sf_controller_aircraft(&c,&data,0);
    assert(sf_controller_tick(&c,1000)==SF_SCREEN_RADAR);
    assert(sf_controller_tick(&c,31001)==SF_SCREEN_WEATHER);
    assert(sf_controller_tick(&c,61001)==SF_SCREEN_PHOTOS);
    assert(sf_controller_tick(&c,121001)==SF_SCREEN_RADAR);
    /* With traffic, radar must time out after 120 seconds. */
    sf_controller_init(&c,0);data.count=1;data.planes[0].distance_miles=1;
    sf_controller_aircraft(&c,&data,0);
    assert(sf_controller_tick(&c,119999)==SF_SCREEN_RADAR);
    assert(sf_controller_tick(&c,120000)==SF_SCREEN_WEATHER);
    /* No recent feed must not turn into false empty sky. */
    sf_controller_init(&c,0);sf_controller_aircraft(&c,&data,0);
    assert(sf_controller_tick(&c,50000)==SF_SCREEN_RADAR);
    puts("controller tests passed");return 0;
}
