# Eldra-V2 Pin Maps

## Display (RGB Panel)

| Signal | GPIO        |
|--------|-------------|
| HSYNC  | 39          |
| VSYNC  | 40          |
| DE     | 41          |
| PCLK   | 42          |
| Backlight | 2        |
| B0-B4  | 15, 7, 6, 5, 4 |
| G0-G5  | 9, 46, 3, 8, 16, 1 |
| R0-R4  | 14, 21, 47, 48, 45 |

---

## IMU (QMI8658)
Uses I2C:

- SDA = TBD (varies slightly by board revision)
- SCL = TBD
- TODO: confirm pins against the exact Waveshare revision in use.

---

## User Buttons
- Boot button = GPIO 0
- Additional keys = TBD

---

## External Servo Driver (I2C)
- SDA = choose free pin
- SCL = choose free pin

---

## SD Card Interface
Handled internally by Waveshare wiring; available via ESP-IDF `sdmmc` or `sdspi` drivers.
