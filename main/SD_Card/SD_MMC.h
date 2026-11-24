#pragma once

#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"

#include "EXIO/TCA9554PWR.h"

#define CONFIG_EXAMPLE_PIN_CLK  2
#define CONFIG_EXAMPLE_PIN_CMD  1
#define CONFIG_EXAMPLE_PIN_D0   42
#define CONFIG_EXAMPLE_PIN_D1   -1
#define CONFIG_EXAMPLE_PIN_D2   -1
#define CONFIG_EXAMPLE_PIN_D3   -1  // Using EXIO

/* Initialized card, or NULL when no usable card is inserted. Not mounted. */
extern sdmmc_card_t *SD_Card;
void SD_Init(void);
