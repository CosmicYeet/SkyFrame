#pragma once
#include "esp_err.h"
#include "skyframe_config.h"
esp_err_t sf_config_nvs_init(void);
esp_err_t sf_config_nvs_load(sf_config_t *out);
esp_err_t sf_config_nvs_save(const sf_config_t *cfg);
