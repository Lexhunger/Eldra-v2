# Config Store

Reads/writes `/sdcard/config.json`. If missing or malformed, it writes defaults and returns them.

The schema includes:
- boot/init flags
- Wi-Fi settings
- cloud/logging options
- sleep policy
- affect weights
- IMU dizzy tuning

## Persisted dizzy tuning keys

Stored under `emotion`:
- `dizzy_gyro_thresh_dps_x10`
- `dizzy_gyro_spike_dps_x10`
- `dizzy_gdev_x100`
- `dizzy_accum_ms`
- `dizzy_cooldown_ms`

Scale factors:
- `*_dps_x10` stores `dps * 10`
- `dizzy_gdev_x100` stores `gdev * 100`

Default dizzy values:
- `gyro_thresh=65.0 dps`
- `spike=145.0 dps`
- `gdev=0.30`
- `accum_ms=220`
- `cooldown_ms=4500`

## API
- `config_store_load(&cfg)` - loads (and auto-creates if missing).
- `config_store_save(&cfg)` - writes config to SD.
- `config_store_get_defaults(&cfg)` - returns defaults without I/O.

Caller must ensure SD is mounted before loading/saving.
