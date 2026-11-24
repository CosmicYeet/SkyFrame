#include "I2C_Driver.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define I2C_MAX_DEVICES                8

static const char *I2C_TAG = "I2C";

static i2c_master_bus_handle_t i2c_bus = NULL;
static SemaphoreHandle_t i2c_dev_lock = NULL;
static struct {
    uint8_t addr;
    i2c_master_dev_handle_t handle;
} i2c_devs[I2C_MAX_DEVICES];
static int i2c_dev_count = 0;

/**
 * @brief i2c master initialization
 */
static esp_err_t i2c_master_init(void)
{
    i2c_master_bus_config_t bus_config = {
        .i2c_port = I2C_MASTER_NUM,
        .sda_io_num = I2C_Touch_SDA_IO,
        .scl_io_num = I2C_Touch_SCL_IO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };

    i2c_dev_lock = xSemaphoreCreateMutex();
    if (i2c_dev_lock == NULL) {
        return ESP_ERR_NO_MEM;
    }
    return i2c_new_master_bus(&bus_config, &i2c_bus);
}
void I2C_Init(void)
{
    /********************* I2C *********************/
    ESP_ERROR_CHECK(i2c_master_init());
    ESP_LOGI(I2C_TAG, "I2C initialized successfully");
}

i2c_master_bus_handle_t I2C_Get_Bus(void)
{
    return i2c_bus;
}

// Look up (or add on first use) the device handle for a 7-bit address
static esp_err_t i2c_get_device(uint8_t Driver_addr, i2c_master_dev_handle_t *dev)
{
    esp_err_t ret = ESP_OK;

    if (i2c_bus == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(i2c_dev_lock, portMAX_DELAY);
    for (int i = 0; i < i2c_dev_count; i++) {
        if (i2c_devs[i].addr == Driver_addr) {
            *dev = i2c_devs[i].handle;
            xSemaphoreGive(i2c_dev_lock);
            return ESP_OK;
        }
    }
    if (i2c_dev_count >= I2C_MAX_DEVICES) {
        ret = ESP_ERR_NO_MEM;
    } else {
        i2c_device_config_t dev_config = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = Driver_addr,
            .scl_speed_hz = I2C_MASTER_FREQ_HZ,
        };
        ret = i2c_master_bus_add_device(i2c_bus, &dev_config, dev);
        if (ret == ESP_OK) {
            i2c_devs[i2c_dev_count].addr = Driver_addr;
            i2c_devs[i2c_dev_count].handle = *dev;
            i2c_dev_count++;
        }
    }
    xSemaphoreGive(i2c_dev_lock);
    return ret;
}


// Reg addr is 8 bit
esp_err_t I2C_Write(uint8_t Driver_addr, uint8_t Reg_addr, const uint8_t *Reg_data, uint32_t Length)
{
    i2c_master_dev_handle_t dev;
    esp_err_t ret = i2c_get_device(Driver_addr, &dev);
    if (ret != ESP_OK) {
        return ret;
    }

    uint8_t buf[Length+1];

    buf[0] = Reg_addr;
    // Copy Reg_data to buf starting at buf[1]
    memcpy(&buf[1], Reg_data, Length);
    return i2c_master_transmit(dev, buf, Length+1, I2C_MASTER_TIMEOUT_MS);
}



esp_err_t I2C_Read(uint8_t Driver_addr, uint8_t Reg_addr, uint8_t *Reg_data, uint32_t Length)
{
    i2c_master_dev_handle_t dev;
    esp_err_t ret = i2c_get_device(Driver_addr, &dev);
    if (ret != ESP_OK) {
        return ret;
    }
    return i2c_master_transmit_receive(dev, &Reg_addr, 1, Reg_data, Length, I2C_MASTER_TIMEOUT_MS);
}
