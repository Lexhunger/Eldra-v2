# Console SD Bridge

Registers SD/config console commands that enqueue work to `sd_driver` (non-blocking):
- `sd_init` - mount card
- `sd_list [path]` - list directory (default `/sdcard`)
- `sd_read <path>` - print file to console
- `sd_delete <path>` - delete file
- `sd_log <text>` - append a line to the daily rotating log
- `sd_wifi_save <ssid> <pass>` - save Wi-Fi creds to `/sdcard/wifi/last_wifi.txt`
- `config_show` - parse and print `/sdcard/config.json` (auto-created if missing)
- `config_clear_all yes` - reset config to defaults and apply runtime defaults
- `sd_format yes` - destructive FAT format, remounts, recreates driver folders

`config_show` includes persisted dizzy IMU settings:
- `gyro_thresh`
- `spike`
- `gdev`
- `accum_ms`
- `cooldown_ms`

Requires `console` and `sd_driver`. The worker task lives inside `sd_driver`, so commands return immediately.
