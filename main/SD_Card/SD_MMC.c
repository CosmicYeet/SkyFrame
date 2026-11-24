#include "SD_MMC.h"
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

static const char *SD_TAG = "SD";

sdmmc_card_t *SD_Card = NULL;

esp_err_t SD_Card_D3_EN(void)
{
    Set_EXIO(TCA9554_EXIO4,true);
    vTaskDelay(pdMS_TO_TICKS(10));
    return ESP_OK;
}

/* Bring up the SDMMC host (1-bit; D3 is driven through the TCA9554 expander) and
   identify the card. The FAT filesystem is mounted later by skyframe_storage, which
   also hands the card to a USB host for file transfer. */
void SD_Init(void)
{
    ESP_LOGI(SD_TAG, "Initializing SD card (SDMMC, 1-bit)");
    SD_Card_D3_EN();

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
    slot_config.width = 1;
    slot_config.clk = CONFIG_EXAMPLE_PIN_CLK;
    slot_config.cmd = CONFIG_EXAMPLE_PIN_CMD;
    slot_config.d0 = CONFIG_EXAMPLE_PIN_D0;
    slot_config.d1 = CONFIG_EXAMPLE_PIN_D1;
    slot_config.d2 = CONFIG_EXAMPLE_PIN_D2;
    slot_config.d3 = CONFIG_EXAMPLE_PIN_D3;
    // Internal pull-ups only supplement the board's external pull-ups.
    slot_config.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    esp_err_t ret = sdmmc_host_init();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(SD_TAG, "SDMMC host init failed (%s)", esp_err_to_name(ret));
        return;
    }
    ret = sdmmc_host_init_slot(host.slot, &slot_config);
    if (ret != ESP_OK) {
        ESP_LOGE(SD_TAG, "SDMMC slot init failed (%s)", esp_err_to_name(ret));
        sdmmc_host_deinit();
        return;
    }
    sdmmc_card_t *card = calloc(1, sizeof(*card));
    if (!card) {
        sdmmc_host_deinit();
        return;
    }
    ret = sdmmc_card_init(&host, card);
    if (ret != ESP_OK) {
        ESP_LOGW(SD_TAG, "No usable microSD card (%s)", esp_err_to_name(ret));
        free(card);
        sdmmc_host_deinit();
        return;
    }
    sdmmc_card_print_info(stdout, card);
    SD_Card = card;
}
