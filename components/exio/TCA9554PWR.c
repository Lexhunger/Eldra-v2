// TCA9554PWR IO expander driver with serialized access and cached output state.
#include <stdbool.h>
#include <stdio.h>
#include "esp_check.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "TCA9554PWR.h"
#include "eldra_logging.h"

static const char *TAG = "exio";
static const TickType_t k_exio_lock_wait_ticks = pdMS_TO_TICKS(20);
static const TickType_t k_exio_cmd_wait_ticks = pdMS_TO_TICKS(50);

static bool s_i2c_initialized = false;
static bool s_expander_configured = false;
static SemaphoreHandle_t s_exio_lock = NULL;
static uint8_t s_cached_output = 0x7F; // EXIO1/3 high, EXIO8 low
static bool s_cached_output_valid = false;
static uint8_t s_cached_config = 0x00; // all outputs
static bool s_cached_config_valid = false;
static const uint8_t k_lcd_guard_mask = (1U << (TCA9554_EXIO1 - 1)) | (1U << (TCA9554_EXIO3 - 1));
static bool s_lcd_control_active = false;

static esp_err_t exio_reg_write(uint8_t reg, uint8_t data);

static bool exio_lock_take(void)
{
    // Before scheduler start there is no concurrent task access; proceed unlocked.
    if (!s_exio_lock || xTaskGetSchedulerState() == taskSCHEDULER_NOT_STARTED) {
        return true;
    }
    return (xSemaphoreTake(s_exio_lock, k_exio_lock_wait_ticks) == pdTRUE);
}

static void exio_lock_give(bool locked)
{
    if (locked && s_exio_lock && xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED) {
        xSemaphoreGive(s_exio_lock);
    }
}

void EXIO_SetLCDControlActive(bool active)
{
    bool locked = exio_lock_take();
    if (!locked) {
        // Keep this operation non-blocking; best effort update.
        EL_LOGW(TAG, "EXIO lock timeout setting LCD control active=%d", active ? 1 : 0);
    }
    s_lcd_control_active = active;
    exio_lock_give(locked);
}

void EXIO_ForceLCDIdle(void)
{
    if (!s_i2c_initialized || !s_expander_configured) {
        return;
    }
    bool locked = exio_lock_take();
    if (!locked) {
        return;
    }
    if (!s_lcd_control_active) {
        uint8_t out = s_cached_output_valid ? s_cached_output : 0x7F;
        out |= k_lcd_guard_mask;
        (void)exio_reg_write(TCA9554_OUTPUT_REG, out);
    }
    exio_lock_give(locked);
}

// Bring up the I2C bus for the expander (idempotent).
static esp_err_t exio_i2c_init(void)
{
    if (s_i2c_initialized) {
        return ESP_OK;
    }

    i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = I2C_MASTER_SDA_IO,
        .scl_io_num = I2C_MASTER_SCL_IO,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = I2C_MASTER_FREQ_HZ,
    };

    esp_err_t ret = i2c_param_config(I2C_MASTER_NUM, &conf);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = i2c_driver_install(I2C_MASTER_NUM, conf.mode, 0, 0, 0);
    if (ret == ESP_ERR_INVALID_STATE) {
        ret = ESP_OK; // already installed
    }
    if (ret == ESP_OK) {
        s_i2c_initialized = true;
        if (!s_exio_lock) {
            s_exio_lock = xSemaphoreCreateMutex();
        }
    }
    return ret;
}

static esp_err_t exio_reg_read(uint8_t reg, uint8_t *out)
{
    if (!out) {
        return ESP_ERR_INVALID_ARG;
    }
    ESP_RETURN_ON_ERROR(exio_i2c_init(), TAG, "i2c init failed");

    uint8_t value = 0;
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    if (!cmd) {
        return ESP_ERR_NO_MEM;
    }

    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (TCA9554_ADDRESS << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, reg, true);
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (TCA9554_ADDRESS << 1) | I2C_MASTER_READ, true);
    i2c_master_read_byte(cmd, &value, I2C_MASTER_NACK);
    i2c_master_stop(cmd);
    esp_err_t ret = i2c_master_cmd_begin(I2C_MASTER_NUM, cmd, k_exio_cmd_wait_ticks);
    i2c_cmd_link_delete(cmd);
    if (ret == ESP_OK) {
        *out = value;
    }
    return ret;
}

static esp_err_t exio_reg_write(uint8_t reg, uint8_t data)
{
    ESP_RETURN_ON_ERROR(exio_i2c_init(), TAG, "i2c init failed");

    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    if (!cmd) {
        return ESP_ERR_NO_MEM;
    }

    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (TCA9554_ADDRESS << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, reg, true);
    i2c_master_write_byte(cmd, data, true);
    i2c_master_stop(cmd);
    esp_err_t ret = i2c_master_cmd_begin(I2C_MASTER_NUM, cmd, k_exio_cmd_wait_ticks);
    i2c_cmd_link_delete(cmd);

    if (ret == ESP_OK) {
        if (reg == TCA9554_OUTPUT_REG) {
            s_cached_output = data;
            s_cached_output_valid = true;
        } else if (reg == TCA9554_CONFIG_REG) {
            s_cached_config = data;
            s_cached_config_valid = true;
        }
    }
    return ret;
}

uint8_t Read_REG(uint8_t REG)
{
    bool locked = exio_lock_take();
    if (!locked) {
        if (REG == TCA9554_OUTPUT_REG && s_cached_output_valid) {
            return s_cached_output;
        }
        if (REG == TCA9554_CONFIG_REG && s_cached_config_valid) {
            return s_cached_config;
        }
        EL_LOGW(TAG, "Read_REG(0x%02X) lock timeout", REG);
        return 0;
    }
    uint8_t v = 0;
    esp_err_t ret = exio_reg_read(REG, &v);
    exio_lock_give(locked);

    if (ret != ESP_OK) {
        if (REG == TCA9554_OUTPUT_REG && s_cached_output_valid) {
            return s_cached_output;
        }
        if (REG == TCA9554_CONFIG_REG && s_cached_config_valid) {
            return s_cached_config;
        }
        EL_LOGW(TAG, "Read_REG(0x%02X) failed: %s", REG, esp_err_to_name(ret));
        return 0;
    }
    return v;
}

void Write_REG(uint8_t REG, uint8_t Data)
{
    bool locked = exio_lock_take();
    if (!locked) {
        EL_LOGW(TAG, "Write_REG(0x%02X,0x%02X) lock timeout", REG, Data);
        return;
    }
    esp_err_t ret = exio_reg_write(REG, Data);
    exio_lock_give(locked);
    if (ret != ESP_OK) {
        EL_LOGW(TAG, "Write_REG(0x%02X,0x%02X) failed: %s", REG, Data, esp_err_to_name(ret));
    }
}

void Mode_EXIO(uint8_t Pin, uint8_t State)
{
    if (Pin < 1 || Pin > 8) {
        return;
    }

    bool locked = exio_lock_take();
    if (!locked) {
        EL_LOGW(TAG, "Mode_EXIO lock timeout pin=%u", Pin);
        return;
    }
    uint8_t cfg = 0;
    if (s_cached_config_valid) {
        cfg = s_cached_config;
    } else if (exio_reg_read(TCA9554_CONFIG_REG, &cfg) != ESP_OK) {
        cfg = 0x00;
    }

    uint8_t bit = (uint8_t)(1U << (Pin - 1));
    if (State) {
        cfg |= bit;   // input mode
    } else {
        cfg &= (uint8_t)~bit; // output mode
    }

    esp_err_t ret = exio_reg_write(TCA9554_CONFIG_REG, cfg);
    exio_lock_give(locked);
    if (ret != ESP_OK) {
        EL_LOGW(TAG, "Mode_EXIO failed pin=%u state=%u: %s", Pin, State, esp_err_to_name(ret));
    }
}

void Mode_EXIOS(uint8_t PinState)
{
    bool locked = exio_lock_take();
    if (!locked) {
        EL_LOGW(TAG, "Mode_EXIOS lock timeout");
        return;
    }
    esp_err_t ret = exio_reg_write(TCA9554_CONFIG_REG, PinState);
    exio_lock_give(locked);
    if (ret != ESP_OK) {
        EL_LOGW(TAG, "Mode_EXIOS failed: %s", esp_err_to_name(ret));
    }
}

uint8_t Read_EXIO(uint8_t Pin)
{
    if (Pin < 1 || Pin > 8) {
        return 0;
    }
    uint8_t bits = Read_REG(TCA9554_INPUT_REG);
    return (bits >> (Pin - 1)) & 0x01;
}

uint8_t Read_EXIOS(void)
{
    return Read_REG(TCA9554_INPUT_REG);
}

void Set_EXIO(uint8_t Pin, uint8_t State)
{
    if (Pin < 1 || Pin > 8 || State > 1) {
        printf("Parameter error, please enter the correct parameter!\r\n");
        return;
    }

    bool locked = exio_lock_take();
    if (!locked) {
        EL_LOGW(TAG, "Set_EXIO lock timeout pin=%u state=%u", Pin, State);
        return;
    }
    uint8_t out = 0;
    if (s_cached_output_valid) {
        out = s_cached_output;
    } else if (exio_reg_read(TCA9554_OUTPUT_REG, &out) != ESP_OK) {
        out = 0x7F; // safe fallback: keep reset/CS high, buzzer low
    }

    uint8_t bit = (uint8_t)(1U << (Pin - 1));
    if (State) {
        out |= bit;
    } else {
        out &= (uint8_t)~bit;
    }
    // Guard LCD reset/CS high for non-LCD writes so unrelated EXIO toggles
    // cannot accidentally shift panel state, but never override lines while
    // the LCD driver is actively holding RESET/CS for command/reset windows.
    if (!s_lcd_control_active && Pin != TCA9554_EXIO1 && Pin != TCA9554_EXIO3) {
        out |= k_lcd_guard_mask;
    }

    esp_err_t ret = exio_reg_write(TCA9554_OUTPUT_REG, out);
    exio_lock_give(locked);
    if (ret != ESP_OK) {
        EL_LOGW(TAG, "Set_EXIO failed pin=%u state=%u: %s", Pin, State, esp_err_to_name(ret));
    }
}

void Set_EXIOS(uint8_t PinState)
{
    // Bulk writes should never pull LCD reset/CS low, except while the LCD
    // driver is actively controlling those lines.
    if (!s_lcd_control_active) {
        PinState |= k_lcd_guard_mask;
    }
    bool locked = exio_lock_take();
    if (!locked) {
        EL_LOGW(TAG, "Set_EXIOS lock timeout");
        return;
    }
    esp_err_t ret = exio_reg_write(TCA9554_OUTPUT_REG, PinState);
    exio_lock_give(locked);
    if (ret != ESP_OK) {
        EL_LOGW(TAG, "Set_EXIOS failed: %s", esp_err_to_name(ret));
    }
}

void Set_Toggle(uint8_t Pin)
{
    if (Pin < 1 || Pin > 8) {
        return;
    }

    bool locked = exio_lock_take();
    if (!locked) {
        EL_LOGW(TAG, "Set_Toggle lock timeout pin=%u", Pin);
        return;
    }
    uint8_t out = 0;
    if (s_cached_output_valid) {
        out = s_cached_output;
    } else if (exio_reg_read(TCA9554_OUTPUT_REG, &out) != ESP_OK) {
        out = 0x7F;
    }

    out ^= (uint8_t)(1U << (Pin - 1));
    if (!s_lcd_control_active && Pin != TCA9554_EXIO1 && Pin != TCA9554_EXIO3) {
        out |= k_lcd_guard_mask;
    }
    esp_err_t ret = exio_reg_write(TCA9554_OUTPUT_REG, out);
    exio_lock_give(locked);
    if (ret != ESP_OK) {
        EL_LOGW(TAG, "Set_Toggle failed pin=%u: %s", Pin, esp_err_to_name(ret));
    }
}

void TCA9554PWR_Init(uint8_t PinState)
{
    Mode_EXIOS(PinState);
}

esp_err_t EXIO_Init(void)
{
    ESP_RETURN_ON_ERROR(exio_i2c_init(), TAG, "i2c init failed");
    if (s_expander_configured) {
        return ESP_OK;
    }
    bool locked = exio_lock_take();
    if (!locked) {
        return ESP_ERR_TIMEOUT;
    }

    // Demo-aligned deterministic defaults:
    // - all EXIO as outputs
    // - no polarity inversion
    // - output 0x7F (EXIO1 reset high, EXIO3 CS high, EXIO8 buzzer low)
    esp_err_t ret = exio_reg_write(TCA9554_CONFIG_REG, 0x00);
    if (ret == ESP_OK) {
        ret = exio_reg_write(TCA9554_Polarity_REG, 0x00);
    }
    const uint8_t out_expected = 0x7F;
    if (ret == ESP_OK) {
        ret = exio_reg_write(TCA9554_OUTPUT_REG, out_expected);
    }

    if (ret == ESP_OK) {
        uint8_t out_readback = 0;
        esp_err_t rr = exio_reg_read(TCA9554_OUTPUT_REG, &out_readback);
        if (rr == ESP_OK && out_readback != out_expected) {
            EL_LOGW(TAG, "EXIO output mismatch (exp=0x%02X got=0x%02X)", out_expected, out_readback);
        }
        s_cached_output = out_expected;
        s_cached_output_valid = true;
        s_cached_config = 0x00;
        s_cached_config_valid = true;
        s_expander_configured = true;
    }

    exio_lock_give(locked);
    return ret;
}

