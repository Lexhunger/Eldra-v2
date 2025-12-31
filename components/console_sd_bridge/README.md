# Console SD Bridge

Registers SD console commands that enqueue work to `sd_driver` (non-blocking):
- `sd_init` – mount card.
- `sd_list [path]` – list directory (default `/sdcard`).
- `sd_read <path>` – print file to console.
- `sd_delete <path>` – delete file.
- `sd_log <text>` – append line to daily rotating log.
- `sd_wifi_save <ssid> <pass>` – save Wi‑Fi creds to `/sdcard/wifi/last_wifi.txt`.
- `config_show` – parse and print `/sdcard/config.json` (auto-created with defaults if missing).
- `sd_format yes` – destructive FAT format, remounts and recreates driver folders.

Requires the `console` component and `sd_driver` to be present. The worker task lives inside `sd_driver` so commands return immediately. *** End Patch
