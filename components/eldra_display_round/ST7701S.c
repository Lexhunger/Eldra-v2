// ST7701S driver: sets up reset/CS lines (via EXIO), sends init sequence,
// configures RGB panel, and provides a simple "Hello" bitmap renderer (legacy).
#include "ST7701S.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "esp_heap_caps.h"
#include "freertos/semphr.h"

#if LCD_RESET_VIA_EXIO || LCD_CS_VIA_EXIO
#include "TCA9554PWR.h"
#endif

#define Delay(ms) vTaskDelay((ms) / portTICK_PERIOD_MS)

static const char *LCD_TAG = "LCD";
static ST7701S_handle s_st7701s = NULL;
esp_lcd_panel_handle_t panel_handle = NULL;
#if CONFIG_EXAMPLE_AVOID_TEAR_EFFECT_WITH_SEM
SemaphoreHandle_t sem_vsync_end = NULL;
SemaphoreHandle_t sem_gui_ready = NULL;
#endif

typedef struct {
    char c;
    uint8_t rows[7];
} glyph_t;

static const glyph_t simple_font[] = {
    {' ', {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}},
    {'D', {0x1E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1E}},
    {'E', {0x1F, 0x10, 0x1E, 0x10, 0x1E, 0x10, 0x1F}},
    {'H', {0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}},
    {'L', {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F}},
    {'O', {0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}},
    {'R', {0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11}},
    {'W', {0x11, 0x11, 0x11, 0x15, 0x15, 0x1B, 0x11}},
};

static const glyph_t *find_glyph(char c)
{
    size_t count = sizeof(simple_font) / sizeof(simple_font[0]);
    for (size_t i = 0; i < count; i++) {
        if (simple_font[i].c == c) {
            return &simple_font[i];
        }
    }
    return &simple_font[0]; // space as fallback
}

/**
 * @brief Example Create an ST7701S object
 * @param SDA SDA pin
 * @param SCL SCL pin
 * @param CS  CS  pin
 * @param channel_select SPI channel selection
 * @param method_select  SPI_METHOD,IOEXPANDER_METHOD
 * @note
*/
ST7701S_handle ST7701S_newObject(int SDA, int SCL, int CS, char channel_select, char method_select)
{
    ST7701S_handle new_handle = heap_caps_calloc(1, sizeof(ST7701S), MALLOC_CAP_DEFAULT);
    if (!new_handle) {
        return NULL;
    }
    new_handle->method_select = method_select;
    
    if (method_select) {
        new_handle->spi_io_config_t.miso_io_num = -1;
        new_handle->spi_io_config_t.mosi_io_num = SDA;
        new_handle->spi_io_config_t.sclk_io_num = SCL;
        new_handle->spi_io_config_t.quadwp_io_num = -1;
        new_handle->spi_io_config_t.quadhd_io_num = -1;

        new_handle->spi_io_config_t.max_transfer_sz = SOC_SPI_MAXIMUM_BUFFER_SIZE;

        ESP_ERROR_CHECK(spi_bus_initialize(channel_select, &(new_handle->spi_io_config_t), SPI_DMA_CH_AUTO));

        new_handle->st7701s_protocol_config_t.command_bits = 1;
        new_handle->st7701s_protocol_config_t.address_bits = 8;
        new_handle->st7701s_protocol_config_t.clock_speed_hz = 4000000;
        new_handle->st7701s_protocol_config_t.mode = 0;
        new_handle->st7701s_protocol_config_t.spics_io_num = CS;
        new_handle->st7701s_protocol_config_t.queue_size = 1;

        ESP_ERROR_CHECK(spi_bus_add_device(channel_select, &(new_handle->st7701s_protocol_config_t),
                                           &(new_handle->spi_device)));
        
        return new_handle;
    } else {
        // IO expander write path is not used in this trimmed-down example.
        return NULL;
    }
}

/**
 * @brief Screen initialization
 * @param St7701S_handle 
 * @param type 
 * @note
*/
void ST7701S_screen_init(ST7701S_handle St7701S_handle, unsigned char type)
{
    (void)St7701S_handle;
    // Local short-hands to keep the init table readable.
    #define SPI_WriteComm(cmd) ST7701S_WriteCommand(St7701S_handle, cmd)
    #define SPI_WriteData(data) ST7701S_WriteData(St7701S_handle, data)
    if (type == 1){
    // 2.8inch
    SPI_WriteComm(0xFF);     
    SPI_WriteData(0x77);   
    SPI_WriteData(0x01);   
    SPI_WriteData(0x00);   
    SPI_WriteData(0x00);   
    SPI_WriteData(0x13);   

    SPI_WriteComm(0xEF);     
    SPI_WriteData(0x08);   

    SPI_WriteComm(0xFF);     
    SPI_WriteData(0x77);   
    SPI_WriteData(0x01);   
    SPI_WriteData(0x00);   
    SPI_WriteData(0x00);   
    SPI_WriteData(0x10);   

    SPI_WriteComm(0xC0);     
    SPI_WriteData(0x3B);   
    SPI_WriteData(0x00);   

    SPI_WriteComm(0xC1);     
    SPI_WriteData(0x10);   
    SPI_WriteData(0x0C);   

    SPI_WriteComm(0xC2);     
    SPI_WriteData(0x07);   
    SPI_WriteData(0x0A);   

    SPI_WriteComm(0xC7);     
    SPI_WriteData(0x00);           

    SPI_WriteComm(0xCC);     
    SPI_WriteData(0x10);   

    SPI_WriteComm(0xCD);     
    SPI_WriteData(0x08); 

    SPI_WriteComm(0xB0);     
    SPI_WriteData(0x05);   
    SPI_WriteData(0x12);   
    SPI_WriteData(0x98);   
    SPI_WriteData(0x0E);   
    SPI_WriteData(0x0F);   
    SPI_WriteData(0x07);   
    SPI_WriteData(0x07);   
    SPI_WriteData(0x09);   
    SPI_WriteData(0x09);   
    SPI_WriteData(0x23);   
    SPI_WriteData(0x05);   
    SPI_WriteData(0x52);   
    SPI_WriteData(0x0F);   
    SPI_WriteData(0x67);   
    SPI_WriteData(0x2C);   
    SPI_WriteData(0x11);   

    SPI_WriteComm(0xB1);     
    SPI_WriteData(0x0B);   
    SPI_WriteData(0x11);   
    SPI_WriteData(0x97);   
    SPI_WriteData(0x0C);   
    SPI_WriteData(0x12);   
    SPI_WriteData(0x06);   
    SPI_WriteData(0x06);   
    SPI_WriteData(0x08);   
    SPI_WriteData(0x08);   
    SPI_WriteData(0x22);   
    SPI_WriteData(0x03);   
    SPI_WriteData(0x51);   
    SPI_WriteData(0x11);   
    SPI_WriteData(0x66);   
    SPI_WriteData(0x2B);   
    SPI_WriteData(0x0F);   

    SPI_WriteComm(0xFF);     
    SPI_WriteData(0x77);   
    SPI_WriteData(0x01);   
    SPI_WriteData(0x00);   
    SPI_WriteData(0x00);   
    SPI_WriteData(0x11);   

    SPI_WriteComm(0xB0);     
    SPI_WriteData(0x5D);   

    SPI_WriteComm(0xB1);     
    SPI_WriteData(0x3E);   

    SPI_WriteComm(0xB2);     
    SPI_WriteData(0x81);   

    SPI_WriteComm(0xB3);     
    SPI_WriteData(0x80);   

    SPI_WriteComm(0xB5);     
    SPI_WriteData(0x4E);   

    SPI_WriteComm(0xB7);     
    SPI_WriteData(0x85);   

    SPI_WriteComm(0xB8);     
    SPI_WriteData(0x20);   

    SPI_WriteComm(0xC1);     
    SPI_WriteData(0x78);   

    SPI_WriteComm(0xC2);     
    SPI_WriteData(0x78);   

    SPI_WriteComm(0xD0);     
    SPI_WriteData(0x88);   

    SPI_WriteComm(0xE0);     
    SPI_WriteData(0x00);   
    SPI_WriteData(0x00);   
    SPI_WriteData(0x02);   

    SPI_WriteComm(0xE1);     
    SPI_WriteData(0x06);   
    SPI_WriteData(0x30);   
    SPI_WriteData(0x08);   
    SPI_WriteData(0x30);   
    SPI_WriteData(0x05);   
    SPI_WriteData(0x30);   
    SPI_WriteData(0x07);   
    SPI_WriteData(0x30);   
    SPI_WriteData(0x00);   
    SPI_WriteData(0x33);   
    SPI_WriteData(0x33);   

    SPI_WriteComm(0xE2);     
    SPI_WriteData(0x11);   
    SPI_WriteData(0x11);   
    SPI_WriteData(0x33);   
    SPI_WriteData(0x33);   
    SPI_WriteData(0xF4);   
    SPI_WriteData(0x00);   
    SPI_WriteData(0x00);   
    SPI_WriteData(0x00);   
    SPI_WriteData(0xF4);   
    SPI_WriteData(0x00);   
    SPI_WriteData(0x00);   
    SPI_WriteData(0x00);   

    SPI_WriteComm(0xE3);     
    SPI_WriteData(0x00);   
    SPI_WriteData(0x00);   
    SPI_WriteData(0x11);   
    SPI_WriteData(0x11);   

    SPI_WriteComm(0xE4);     
    SPI_WriteData(0x44);   
    SPI_WriteData(0x44);   

    SPI_WriteComm(0xE5);     
    SPI_WriteData(0x0D);   
    SPI_WriteData(0xF5);   
    SPI_WriteData(0x30);   
    SPI_WriteData(0xF0);   
    SPI_WriteData(0x0F);   
    SPI_WriteData(0xF7);   
    SPI_WriteData(0x30);   
    SPI_WriteData(0xF0);   
    SPI_WriteData(0x09);   
    SPI_WriteData(0xF1);   
    SPI_WriteData(0x30);   
    SPI_WriteData(0xF0);   
    SPI_WriteData(0x0B);   
    SPI_WriteData(0xF3);   
    SPI_WriteData(0x30);   
    SPI_WriteData(0xF0);   

    SPI_WriteComm(0xE6);     
    SPI_WriteData(0x00);   
    SPI_WriteData(0x00);   
    SPI_WriteData(0x11);   
    SPI_WriteData(0x11);   

    SPI_WriteComm(0xE7);     
    SPI_WriteData(0x44);   
    SPI_WriteData(0x44);   

    SPI_WriteComm(0xE8);     
    SPI_WriteData(0x0C);   
    SPI_WriteData(0xF4);   
    SPI_WriteData(0x30);   
    SPI_WriteData(0xF0);   
    SPI_WriteData(0x0E);   
    SPI_WriteData(0xF6);   
    SPI_WriteData(0x30);   
    SPI_WriteData(0xF0);   
    SPI_WriteData(0x08);   
    SPI_WriteData(0xF0);   
    SPI_WriteData(0x30);   
    SPI_WriteData(0xF0);   
    SPI_WriteData(0x0A);   
    SPI_WriteData(0xF2);   
    SPI_WriteData(0x30);   
    SPI_WriteData(0xF0);   

    SPI_WriteComm(0xE9);     
    SPI_WriteData(0x36);   
    SPI_WriteData(0x01);   

    SPI_WriteComm(0xEB);     
    SPI_WriteData(0x00);   
    SPI_WriteData(0x01);   
    SPI_WriteData(0xE4);   
    SPI_WriteData(0xE4);   
    SPI_WriteData(0x44);   
    SPI_WriteData(0x88);   
    SPI_WriteData(0x40);   

    SPI_WriteComm(0xED);     
    SPI_WriteData(0xFF);   
    SPI_WriteData(0x10);   
    SPI_WriteData(0xAF);   
    SPI_WriteData(0x76);   
    SPI_WriteData(0x54);   
    SPI_WriteData(0x2B);   
    SPI_WriteData(0xCF);   
    SPI_WriteData(0xFF);   
    SPI_WriteData(0xFF);   
    SPI_WriteData(0xFC);   
    SPI_WriteData(0xB2);   
    SPI_WriteData(0x45);   
    SPI_WriteData(0x67);   
    SPI_WriteData(0xFA);   
    SPI_WriteData(0x01);   
    SPI_WriteData(0xFF);   

    SPI_WriteComm(0xEF);     
    SPI_WriteData(0x08);   
    SPI_WriteData(0x08);   
    SPI_WriteData(0x08);   
    SPI_WriteData(0x45);   
    SPI_WriteData(0x3F);   
    SPI_WriteData(0x54);   

    SPI_WriteComm(0xFF);     
    SPI_WriteData(0x77);   
    SPI_WriteData(0x01);   
    SPI_WriteData(0x00);   
    SPI_WriteData(0x00);   
    SPI_WriteData(0x00);   

    SPI_WriteComm(0x11);     
    Delay(120);                //ms

    SPI_WriteComm(0x3A);    
    SPI_WriteData(0x66);       // 0x66  /  0x77

    SPI_WriteComm(0x36);     
    SPI_WriteData(0x00);   

    SPI_WriteComm(0x35);     
    SPI_WriteData(0x00);   

    SPI_WriteComm(0x29);
    }
    #undef SPI_WriteComm
    #undef SPI_WriteData
}

/**
 * @brief Example Delete the ST7701S object
 * @param St7701S_handle 
*/
void ST7701S_delObject(ST7701S_handle St7701S_handle)
{
    if (St7701S_handle == NULL) {
        return;
    }
    free(St7701S_handle);
}

/**
 * @brief SPI write instruction
 * @param St7701S_handle 
 * @param cmd instruction
*/
void ST7701S_WriteCommand(ST7701S_handle St7701S_handle, uint8_t cmd)
{
    if(St7701S_handle->method_select){
        spi_transaction_t spi_tran = {
            .rxlength = 0,
            .length = 0,
            .cmd = 0,
            .addr = cmd,
        };
        spi_device_transmit(St7701S_handle->spi_device, &spi_tran);
    }else{
        // IO expander path not used in this simplified example
    }
}

/**
 * @brief SPI write data
 * @param St7701S_handle
 * @param data 
*/
void ST7701S_WriteData(ST7701S_handle St7701S_handle, uint8_t data)
{
    if(St7701S_handle->method_select){
        spi_transaction_t spi_tran = {
            .rxlength = 0,
            .length = 0,
            .cmd = 1,
            .addr = data,
        };
        spi_device_transmit(St7701S_handle->spi_device, &spi_tran);
    }else{
        // IO expander path not used in this simplified example
    }
}


// Reset line handling (EXIO or direct GPIO depending on board wiring)
esp_err_t ST7701S_reset(void)
{
#if LCD_RESET_VIA_EXIO
    Set_EXIO(TCA9554_EXIO1, false);
#else
    if (LCD_RESET_GPIO < 0) {
        return ESP_ERR_INVALID_ARG;
    }
    gpio_set_level(LCD_RESET_GPIO, 0);
#endif
    vTaskDelay(pdMS_TO_TICKS(10));
#if LCD_RESET_VIA_EXIO
    Set_EXIO(TCA9554_EXIO1, true);
#else
    gpio_set_level(LCD_RESET_GPIO, 1);
#endif
    vTaskDelay(pdMS_TO_TICKS(10));
    return ESP_OK;
}

// Manual chip-select handling (EXIO or direct GPIO)
esp_err_t ST7701S_CS_EN(void)
{
#if LCD_CS_VIA_EXIO
    Set_EXIO(TCA9554_EXIO3,false);
#else
    if (LCD_CS_GPIO < 0) {
        return ESP_ERR_INVALID_ARG;
    }
    gpio_set_level(LCD_CS_GPIO, 0);
#endif
    vTaskDelay(pdMS_TO_TICKS(10));
    return ESP_OK;
}
esp_err_t ST7701S_CS_Dis(void)
{
#if LCD_CS_VIA_EXIO
    Set_EXIO(TCA9554_EXIO3,true);
#else
    if (LCD_CS_GPIO < 0) {
        return ESP_ERR_INVALID_ARG;
    }
    gpio_set_level(LCD_CS_GPIO, 1);
#endif
    vTaskDelay(pdMS_TO_TICKS(10));
    return ESP_OK;
}

static void draw_glyph(uint16_t *buffer, int buf_w, int x, int y, const glyph_t *glyph, int scale, uint16_t fg, uint16_t bg)
{
    const int glyph_w = 5;
    const int glyph_h = 7;
    for (int row = 0; row < glyph_h; row++) {
        for (int col = 0; col < glyph_w; col++) {
            bool on = (glyph->rows[row] >> (glyph_w - 1 - col)) & 0x01;
            for (int sy = 0; sy < scale; sy++) {
                for (int sx = 0; sx < scale; sx++) {
                    int px = x + col * scale + sx;
                    int py = y + row * scale + sy;
                    buffer[py * buf_w + px] = on ? fg : bg;
                }
            }
        }
    }
}

// Bring up control lines: initialize EXIO if needed and set GPIO directions.
static esp_err_t lcd_control_lines_init(void)
{
#if LCD_RESET_VIA_EXIO || LCD_CS_VIA_EXIO
    ESP_LOGI(LCD_TAG, "Init EXIO for reset/CS");
    esp_err_t ret = EXIO_Init();
    if (ret != ESP_OK) {
        return ret;
    }
#endif

#if !LCD_RESET_VIA_EXIO
    if (LCD_RESET_GPIO < 0) {
        return ESP_ERR_INVALID_ARG;
    }
    gpio_reset_pin(LCD_RESET_GPIO);
    gpio_set_direction(LCD_RESET_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(LCD_RESET_GPIO, 1);
#endif
#if !LCD_CS_VIA_EXIO
    if (LCD_CS_GPIO >= 0) {
        gpio_reset_pin(LCD_CS_GPIO);
        gpio_set_direction(LCD_CS_GPIO, GPIO_MODE_OUTPUT);
        gpio_set_level(LCD_CS_GPIO, 1);
    }
#endif
    return ESP_OK;
}

esp_err_t LCD_Init(void)
{
    if (panel_handle) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t ret = lcd_control_lines_init();
    if (ret != ESP_OK) {
        return ret;
    }

    ST7701S_reset();
    ST7701S_CS_EN();
    vTaskDelay(pdMS_TO_TICKS(100));
    s_st7701s = ST7701S_newObject(LCD_MOSI, LCD_SCLK, LCD_CS, SPI2_HOST, SPI_METHOD);
    if (!s_st7701s) {
        return ESP_ERR_NO_MEM;
    }
    
    ST7701S_screen_init(s_st7701s, 1);
#if CONFIG_EXAMPLE_AVOID_TEAR_EFFECT_WITH_SEM
    ESP_LOGI(LCD_TAG, "Create semaphores");
    sem_vsync_end = xSemaphoreCreateBinary();
    assert(sem_vsync_end);
    sem_gui_ready = xSemaphoreCreateBinary();
    assert(sem_gui_ready);
#endif

    ESP_LOGI(LCD_TAG, "Install RGB LCD panel driver");
    esp_lcd_rgb_panel_config_t panel_config = {
        .data_width = 16, // RGB565 in parallel mode, thus 16bit in width
        .psram_trans_align = 64,
        .num_fbs = EXAMPLE_LCD_NUM_FB,
#if CONFIG_EXAMPLE_USE_BOUNCE_BUFFER
        .bounce_buffer_size_px = 10 * EXAMPLE_LCD_H_RES,
#endif
        .clk_src = LCD_CLK_SRC_DEFAULT,
        .disp_gpio_num = EXAMPLE_PIN_NUM_DISP_EN,
        .pclk_gpio_num = EXAMPLE_PIN_NUM_PCLK,
        .vsync_gpio_num = EXAMPLE_PIN_NUM_VSYNC,
        .hsync_gpio_num = EXAMPLE_PIN_NUM_HSYNC,
        .de_gpio_num = EXAMPLE_PIN_NUM_DE,
        .data_gpio_nums = {
            EXAMPLE_PIN_NUM_DATA0,
            EXAMPLE_PIN_NUM_DATA1,
            EXAMPLE_PIN_NUM_DATA2,
            EXAMPLE_PIN_NUM_DATA3,
            EXAMPLE_PIN_NUM_DATA4,
            EXAMPLE_PIN_NUM_DATA5,
            EXAMPLE_PIN_NUM_DATA6,
            EXAMPLE_PIN_NUM_DATA7,
            EXAMPLE_PIN_NUM_DATA8,
            EXAMPLE_PIN_NUM_DATA9,
            EXAMPLE_PIN_NUM_DATA10,
            EXAMPLE_PIN_NUM_DATA11,
            EXAMPLE_PIN_NUM_DATA12,
            EXAMPLE_PIN_NUM_DATA13,
            EXAMPLE_PIN_NUM_DATA14,
            EXAMPLE_PIN_NUM_DATA15,
        },
        .timings = {
            .pclk_hz = EXAMPLE_LCD_PIXEL_CLOCK_HZ,
            .h_res = EXAMPLE_LCD_H_RES,
            .v_res = EXAMPLE_LCD_V_RES, 
            .hsync_back_porch = 10,
            .hsync_front_porch = 50,
            .hsync_pulse_width = 8,
            .vsync_back_porch = 18,
            .vsync_front_porch = 8,
            .vsync_pulse_width = 2,
            .flags.pclk_active_neg = false,
        },
        .flags.fb_in_psram = true, // allocate frame buffer in PSRAM
    };
    ret = esp_lcd_new_rgb_panel(&panel_config, &panel_handle);
    if (ret != ESP_OK) {
        return ret;
    }

    ESP_LOGI(LCD_TAG, "Initialize RGB LCD panel");
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel_handle));
    ST7701S_CS_Dis();
    Backlight_Init();
    return ESP_OK;
}

void LCD_Clear(uint16_t color)
{
    if (!panel_handle) {
        return;
    }

    const int chunk_lines = 40;
    size_t buffer_pixels = EXAMPLE_LCD_H_RES * chunk_lines;
    uint16_t *line_buffer = heap_caps_malloc(buffer_pixels * sizeof(uint16_t), MALLOC_CAP_8BIT | MALLOC_CAP_DMA);
    if (!line_buffer) {
        ESP_LOGE(LCD_TAG, "Failed to allocate clear buffer");
        return;
    }

    for (size_t i = 0; i < buffer_pixels; i++) {
        line_buffer[i] = color;
    }

    for (int y = 0; y < EXAMPLE_LCD_V_RES; y += chunk_lines) {
        int lines = chunk_lines;
        if (y + lines > EXAMPLE_LCD_V_RES) {
            lines = EXAMPLE_LCD_V_RES - y;
        }
        esp_lcd_panel_draw_bitmap(panel_handle, 0, y, EXAMPLE_LCD_H_RES, y + lines, line_buffer);
    }

    free(line_buffer);
}

void LCD_DrawHelloWorld(void)
{
    if (!panel_handle) {
        return;
    }

    const char *text = "HELLO WORLD";
    const int scale = 4;
    const int glyph_w = 5 * scale;
    const int glyph_h = 7 * scale;
    const int spacing = 1 * scale;
    int len = strlen(text);
    int buffer_w = len * (glyph_w + spacing) - spacing;
    int buffer_h = glyph_h;
    uint16_t fg = 0xFFFF; // white
    uint16_t bg = 0x0000; // black

    uint16_t *text_buffer = heap_caps_malloc(buffer_w * buffer_h * sizeof(uint16_t), MALLOC_CAP_8BIT | MALLOC_CAP_DMA);
    if (!text_buffer) {
        ESP_LOGE(LCD_TAG, "Failed to allocate text buffer");
        return;
    }
    for (int i = 0; i < buffer_w * buffer_h; i++) {
        text_buffer[i] = bg;
    }

    int cursor_x = 0;
    for (int i = 0; i < len; i++) {
        const glyph_t *glyph = find_glyph(text[i]);
        draw_glyph(text_buffer, buffer_w, cursor_x, 0, glyph, scale, fg, bg);
        cursor_x += glyph_w + spacing;
    }

    int start_x = (EXAMPLE_LCD_H_RES - buffer_w) / 2;
    int start_y = (EXAMPLE_LCD_V_RES - buffer_h) / 2;
    esp_lcd_panel_draw_bitmap(panel_handle, start_x, start_y, start_x + buffer_w, start_y + buffer_h, text_buffer);

    free(text_buffer);
}

/********************* Reinit helper *********************/
esp_err_t ST7701S_reinit_sequence(void)
{
    if (!s_st7701s) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = ST7701S_reset();
    if (err != ESP_OK) {
        return err;
    }
    ST7701S_CS_EN();
    vTaskDelay(pdMS_TO_TICKS(100));
    ST7701S_screen_init(s_st7701s, 1);
    return ESP_OK;
}

/********************* BackLight *********************/
static void example_ledc_init(void)
{
    // Prepare and then apply the LEDC PWM timer configuration
    ledc_timer_config_t ledc_timer = {
        .speed_mode       = LEDC_MODE,
        .timer_num        = LEDC_TIMER,
        .duty_resolution  = LEDC_DUTY_RES,
        .freq_hz          = LEDC_FREQUENCY,  // Set output frequency at 4 kHz
        .clk_cfg          = LEDC_AUTO_CLK
    };
    ESP_ERROR_CHECK(ledc_timer_config(&ledc_timer));

    // Prepare and then apply the LEDC PWM channel configuration
    ledc_channel_config_t ledc_channel = {
        .speed_mode     = LEDC_MODE,
        .channel        = LEDC_CHANNEL,
        .timer_sel      = LEDC_TIMER,
        .intr_type      = LEDC_INTR_DISABLE,
        .gpio_num       = LEDC_OUTPUT_IO,
        .duty           = 0, // Set duty to 0%
        .hpoint         = 0
    };
    ESP_ERROR_CHECK(ledc_channel_config(&ledc_channel));
}

// Vendor default backlight level at init (will be overridden by app_main).
uint8_t LCD_Backlight = 70;
void Backlight_Init(void)
{
    example_ledc_init();
    Set_Backlight(LCD_Backlight);
}

void Set_Backlight(uint8_t Light)
{
    if(Light > Backlight_MAX) Light = Backlight_MAX;
    ESP_ERROR_CHECK(ledc_set_duty(LEDC_MODE, LEDC_CHANNEL, Light*(8192/100)));    // Set duty
    ESP_ERROR_CHECK(ledc_update_duty(LEDC_MODE, LEDC_CHANNEL));                 // Update duty to apply the new value
}
