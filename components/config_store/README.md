# Config Store

Reads/writes `/sdcard/config.json`. If missing or malformed, writes defaults and returns them.

Defaults
```json
{
  "auto_init": { "sd": true, "wifi": true, "rtc": true, "imu": true },
  "wifi": { "ssid": "", "pass": "", "roam": true },
  "logs": { "to_sd": true }
}
```

API
- `config_store_load(&cfg)` – loads (and auto-creates file if missing).
- `config_store_save(&cfg)` – writes file.
- `config_store_get_defaults(&cfg)` – returns defaults without I/O.

Caller must ensure SD is mounted before loading/saving.
