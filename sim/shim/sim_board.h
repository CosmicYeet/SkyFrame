/* Simulator stand-ins for the Waveshare board drivers. */
#pragma once
#include <stdint.h>
#include "sim_esp.h"
void I2C_Init(void);
void EXIO_Init(void);
void LCD_Init(void);
void Touch_Init(void);
void SD_Init(void);
#include <stdint.h>
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
void LVGL_Init(void);              /* opens the window, starts LVGL */
void Set_Backlight(uint8_t light); /* 0-100: dims the window */
uint32_t LCD_Vsync_Count(void);    /* panel refreshes: 60 per second */
void LCD_Set_Vsync_Task(TaskHandle_t task);
const void *LCD_Frame_Buffer(void); /* the window's 480x480 RGB565 picture */
extern sdmmc_card_t *SD_Card;
