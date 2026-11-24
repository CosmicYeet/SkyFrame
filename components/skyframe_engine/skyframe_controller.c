#include "skyframe_controller.h"
#include <string.h>
#define SF_FEED_STALE_MS 45000u
void sf_controller_init(sf_controller_t *c,uint32_t now_ms){
    if (!c) return;
    memset(c,0,sizeof(*c));
    sf_schedule_init(&c->scheduler,now_ms);
    c->last_screen=SF_SCREEN_RADAR;c->screen_changed=true;c->activation_miles=SF_ACTIVATION_MILES;
}
void sf_controller_aircraft(sf_controller_t *c,const sf_aircraft_result_t *r,uint32_t now_ms){
    if(!c||!r)return;
    c->aircraft=*r;c->have_aircraft_snapshot=true;c->aircraft_updated_ms=now_ms;
}
void sf_controller_details(sf_controller_t *c,bool open){if(c)c->details_open=open;}
sf_screen_t sf_controller_tick(sf_controller_t *c,uint32_t now_ms){
    if(!c)return SF_SCREEN_RADAR;
    bool valid=c->have_aircraft_snapshot&&c->aircraft.api_ok&&
        ((uint32_t)(now_ms-c->aircraft_updated_ms)<=SF_FEED_STALE_MS);
    sf_screen_t next=sf_schedule_tick(&c->scheduler,now_ms,valid,
        valid&&sf_has_active_traffic(&c->aircraft,c->activation_miles),c->details_open);
    c->screen_changed=(next!=c->last_screen);
    c->last_screen=next;
    return next;
}

void sf_controller_set_activation(sf_controller_t *c,float miles){if(c&&miles>=1&&miles<=100)c->activation_miles=miles;}
