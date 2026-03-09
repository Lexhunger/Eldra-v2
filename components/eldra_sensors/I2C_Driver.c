#include "I2C_Driver.h"
#include "esp_check.h"
#include "freertos/FreeRTOS.h"
#include "eldra_logging.h"


static const char *I2C_TAG = "I2C";

static i2c_master_bus_handle_t s_i2c_bus = NULL;
/**
 * @brief i2c master initialization
 */
static esp_err_t i2c_master_init(void)
{
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = I2C_MASTER_NUM,
        .scl_io_num = I2C_Touch_SCL_IO,
        .sda_io_num = I2C_Touch_SDA_IO,
        .glitch_ignore_cnt = 7,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .flags.enable_internal_pullup = true,
    };
    return i2c_new_master_bus(&bus_cfg, &s_i2c_bus);
}
void I2C_Init(void)
{
    /********************* I2C *********************/
    ESP_ERROR_CHECK(i2c_master_init());
    EL_LOGI(I2C_TAG, "I2C initialized successfully");  
}


// Reg addr is 8 bit
esp_err_t I2C_Write(uint8_t Driver_addr, uint8_t Reg_addr, const uint8_t *Reg_data, uint32_t Length)
{
    if (!s_i2c_bus) {
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t buf[Length+1];
    
    buf[0] = Reg_addr;
    // Copy Reg_data to buf starting at buf[1]
    memcpy(&buf[1], Reg_data, Length);

    i2c_master_dev_handle_t dev;
    i2c_device_config_t dev_cfg = {
        .device_address = Driver_addr,
        .scl_speed_hz = I2C_MASTER_FREQ_HZ,
        .dev_addr_length = I2C_ADDR_BIT_7,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(s_i2c_bus, &dev_cfg, &dev), I2C_TAG, "add device failed");
    esp_err_t ret = i2c_master_transmit(dev, buf, Length + 1, I2C_MASTER_TIMEOUT_MS / portTICK_PERIOD_MS);
    i2c_master_bus_rm_device(dev);
    return ret;
}



esp_err_t I2C_Read(uint8_t Driver_addr, uint8_t Reg_addr, uint8_t *Reg_data, uint32_t Length)
{
    if (!s_i2c_bus) {
        return ESP_ERR_INVALID_STATE;
    }

    i2c_master_dev_handle_t dev;
    i2c_device_config_t dev_cfg = {
        .device_address = Driver_addr,
        .scl_speed_hz = I2C_MASTER_FREQ_HZ,
        .dev_addr_length = I2C_ADDR_BIT_7,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(s_i2c_bus, &dev_cfg, &dev), I2C_TAG, "add device failed");
    esp_err_t ret = i2c_master_transmit_receive(dev, &Reg_addr, 1, Reg_data, Length, I2C_MASTER_TIMEOUT_MS / portTICK_PERIOD_MS);
    i2c_master_bus_rm_device(dev);
    return ret;
}

