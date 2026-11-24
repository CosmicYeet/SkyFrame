#pragma once

#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "lvgl.h"
/* SkyFrame: LVGL demos are intentionally not compiled. */

#include "ST7701S.h"
#include "GT911.h"

#define EXAMPLE_LVGL_TICK_PERIOD_MS    2

extern lv_disp_draw_buf_t disp_buf; // contains internal graphic buffer(s) called draw buffer(s)
extern lv_disp_drv_t disp_drv;      // contains callback functions
extern lv_disp_t *disp;
void example_lvgl_flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_map);
void example_increase_lvgl_tick(void *arg);
/*Read the touchpad*/
void example_touchpad_read( lv_indev_drv_t * drv, lv_indev_data_t * data );

/* Frame timing, for the serial log: a frame's UI work (timers, animations, drawing; from
   lvgl_render_begin, called before lv_timer_handler, to its last area handed to the panel)
   and, separately, the wait for the panel's refresh. Read and reset by lvgl_frame_stats_take. */
typedef struct {
    uint32_t frames;          /* frames sent to the panel */
    uint32_t measured;        /* of which timed */
    uint64_t work_us;         /* their UI work in all */
    uint32_t work_max_us;
    uint64_t wait_us;         /* waiting for the panel's refresh */
} lvgl_frame_stats_t;
void lvgl_render_begin(void);
void lvgl_frame_stats_take(lvgl_frame_stats_t *out);

void LVGL_Init(void);
/* The panel's frame buffer (480x480 RGB565): exactly what is on the glass. */
const void *LCD_Frame_Buffer(void);