# Waveshare ESP32-S3 2.8" Round LCD Development Board

Hardware foundation of Eldra-V2. It includes a powerful ESP32-S3 MCU, a 480x480 IPS display, multiple sensors, an SD card slot, and onboard power management/charging.

---

## Board Overview

### MCU
- ESP32-S3 (Xtensa LX7 dual-core @ 240 MHz)
- 512 KB SRAM, 384 KB ROM
- 16 MB flash, 8 MB PSRAM
- Wi-Fi 2.4 GHz + BLE 5

### Display
- 2.8" round IPS LCD
- 480x480 resolution
- RGB parallel interface (not SPI)
- High brightness and deep blacks; LVGL capable

### Onboard Peripherals
- QMI8658 6-axis IMU (acc + gyro)
- RTC clock + battery backup holder
- SD/TF card slot
- Battery charging management
- USB-C port for flashing and power
- User button(s)

---

## Pin Mapping (RGB Panel)

| Signal    | GPIO | Purpose            |
|-----------|------|--------------------|
| HSYNC     | 39   | Horizontal sync    |
| VSYNC     | 40   | Vertical sync      |
| DE        | 41   | Data Enable        |
| PCLK      | 42   | Pixel Clock        |
| Backlight | 2    | LCD backlight enable |
| B0-B4     | 15, 7, 6, 5, 4 | Blue channel |
| G0-G5     | 9, 46, 3, 8, 16, 1 | Green channel |
| R0-R4     | 14, 21, 47, 48, 45 | Red channel |

---

## Power

### Battery
- Supports single-cell LiPo / Li-Ion
- JST-PH 2.0 connector
- Onboard charger handles USB-C charging

### USB-C
- Used for flashing, serial console, and power

### Voltage
- Board runs at 3.3 V internally; no external regulation needed for basic operation

---

## Advantages for Eldra-V2
- RGB parallel bus allows full-speed 480x480 animations
- Integrated IMU enables interaction and personality
- SD card for asset storage (future support)
- Plenty of flash + PSRAM for large animations
- Onboard RTC allows timed behaviors
- Round screen matches the "eye" aesthetic
