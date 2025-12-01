# Eldra-v2

ESP-IDF 5.3.1 project for the Waveshare ESP32-S3 2.8" Round Display.

## Features

- RGB panel display driver using `esp_lcd_new_rgb_panel`
- 11×11 pixel art eye with 8-color RGB565 palette
- Scalable eye rendering with center positioning
- Clean modular architecture

## Requirements

- ESP-IDF v5.3.1 or later
- Waveshare ESP32-S3 2.8" Round Display
- USB-C cable for programming

## Dependencies

This project uses standard ESP-IDF components:
- `esp_lcd` - LCD panel driver
- `driver` - GPIO driver
- `nvs_flash` - Non-volatile storage
- `freertos` - FreeRTOS kernel

## File Layout

```
Eldra-v2/
├── CMakeLists.txt              # Project CMake configuration
├── README.md                   # This file
├── sdkconfig.defaults          # Default SDK configuration
├── partitions.csv              # Partition table
├── main/
│   ├── CMakeLists.txt          # Main component CMake
│   └── main.c                  # Application entry point
└── components/
    ├── eldra_display_round/
    │   ├── CMakeLists.txt
    │   ├── eldra_display_round.c
    │   └── include/
    │       └── eldra_display_round.h
    └── eldra_eyes/
        ├── CMakeLists.txt
        ├── eldra_eyes.c
        └── include/
            └── eldra_eyes.h
```

## Build Steps

1. **Install ESP-IDF v5.3.1**
   ```bash
   mkdir -p ~/esp
   cd ~/esp
   git clone -b v5.3.1 --recursive https://github.com/espressif/esp-idf.git
   cd esp-idf
   ./install.sh esp32s3
   ```

2. **Set up environment**
   ```bash
   . ~/esp/esp-idf/export.sh
   ```

3. **Clone and build**
   ```bash
   git clone https://github.com/Lexhunger/Eldra-v2.git
   cd Eldra-v2
   idf.py set-target esp32s3
   idf.py build
   ```

4. **Flash and monitor**
   ```bash
   idf.py -p /dev/ttyUSB0 flash monitor
   ```

## Wiring

The Waveshare ESP32-S3 2.8" Round Display is an integrated board with the display already connected. No external wiring is required.

### Pin Mapping (Reference)

| Signal    | GPIO | Description          |
|-----------|------|----------------------|
| HSYNC     | 39   | Horizontal sync      |
| VSYNC     | 40   | Vertical sync        |
| DE        | 41   | Data enable          |
| PCLK      | 42   | Pixel clock          |
| Backlight | 2    | LCD backlight        |
| B0-B4     | 15,7,6,5,4     | Blue data    |
| G0-G5     | 9,46,3,8,16,1  | Green data   |
| R0-R4     | 14,21,47,48,45 | Red data     |

## API Reference

### Display Module (`eldra_display_round.h`)

```c
esp_err_t eldra_display_init(void);           // Initialize display
esp_err_t eldra_display_clear(uint16_t color); // Clear with color
esp_err_t eldra_display_draw_pixel(int x, int y, uint16_t color);
esp_err_t eldra_display_fill_rect(int x, int y, int w, int h, uint16_t color);
esp_err_t eldra_display_refresh(void);        // Update display
int eldra_display_get_width(void);            // Get width (480)
int eldra_display_get_height(void);           // Get height (480)
```

### Eyes Module (`eldra_eyes.h`)

```c
esp_err_t eldra_eyes_init(void);              // Initialize module
esp_err_t eldra_eyes_draw(const eldra_eye_config_t *config);
esp_err_t eldra_eyes_draw_centered(int scale); // Draw centered eye
uint16_t eldra_eyes_get_palette_color(eldra_eye_color_t index);
```

### RGB565 Color Palette

| Index | Color   | RGB565  |
|-------|---------|---------|
| 0     | Black   | 0x0000  |
| 1     | White   | 0xFFFF  |
| 2     | Blue    | 0x001F  |
| 3     | Red     | 0xF800  |
| 4     | Green   | 0x07E0  |
| 5     | Yellow  | 0xFFE0  |
| 6     | Cyan    | 0x07FF  |
| 7     | Magenta | 0xF81F  |

## License

MIT License