/* Simulator stand-ins for the Waveshare board drivers. */
#pragma once
#include <stdint.h>
#include "sim_esp.h"
void I2C_Init(void);
void EXIO_Init(void);
void LCD_Init(void);
void Touch_Init(void);
void SD_Init(void);
void LVGL_Init(void);              /* opens the window, starts LVGL */
void Set_Backlight(uint8_t light); /* 0-100: dims the window */
uint32_t LCD_Vsync_Count(void);    /* panel refreshes: 60 per second */
const void *LCD_Frame_Buffer(void); /* the window's 480x480 RGB565 picture */
extern sdmmc_card_t *SD_Card;
