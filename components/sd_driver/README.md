# SD Driver (FatFS over SDMMC 1-bit)

Minimal, async SD card support for ESP32-S3-LCD-2.8C using the board’s 1-bit SDMMC pins (CLK=2, CMD=1, D0=42, D3 enabled via EXIO4). Mount point: `/sdcard`.

Features
- Async worker task (core1) so console/UI stay responsive.
- `sd_driver_init()` mounts card and ensures `/sdcard/logs` and `/sdcard/wifi` exist.
- `sd_driver_list_async(path)` list directory.
- `sd_driver_read_async(path)` print file contents to console.
- `sd_driver_delete_async(path)` delete file.
- `sd_driver_log_async(line)` append to daily log `/sdcard/logs/YYYYMMDD.log`, rotates to keep last 5 days.
- `sd_driver_save_wifi_credentials(ssid, pass)` writes `/sdcard/wifi/last_wifi.txt`.
- `sd_driver_format()` destructive FAT format, remounts, recreates dirs.

Dependencies
- `fatfs`, `sdmmc`, `esp_driver_sdmmc`, `esp_driver_gpio`, `freertos`, `esp_timer`, `exio`.

Notes
- Uses internal pull-ups; board requires external 10k pull-ups on SD lines (as per schematic).
- Rotation is best-effort and runs after each log append.
