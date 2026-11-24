#pragma once
#include "skyframe_engine.h"
#include <stdbool.h>
#include <stdint.h>
/* A simple main-thread controller. Networking worker must pass snapshot copies here
   (or deliver them through a queue) rather than writing these fields concurrently. */
typedef struct {
    sf_scheduler_t scheduler;
    sf_aircraft_result_t aircraft;
    bool have_aircraft_snapshot;
    bool details_open;
    sf_screen_t last_screen;
    bool screen_changed;
    uint32_t aircraft_updated_ms;
    float activation_miles;
} sf_controller_t;
void sf_controller_init(sf_controller_t *c, uint32_t now_ms);
void sf_controller_aircraft(sf_controller_t *c, const sf_aircraft_result_t *r, uint32_t now_ms);
void sf_controller_details(sf_controller_t *c, bool open);
sf_screen_t sf_controller_tick(sf_controller_t *c, uint32_t now_ms);

void sf_controller_set_activation(sf_controller_t *c,float miles);
