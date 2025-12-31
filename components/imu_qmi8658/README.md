# QMI8658 IMU Driver (ESP32-S3-Touch-LCD-2.8C)

Minimal 6‑axis driver for the on-board QMI8658 accelerometer/gyro. Uses the shared I2C0 bus (SDA=15, SCL=7) already brought up by the EXIO component; safe to call init multiple times.

## Wiring
- I2C0: SDA=GPIO15, SCL=GPIO7 (same as TCA9554 expander).
- IMU address: tries 0x6B first, then 0x6A (WHO_AM_I verified).

## Defaults
- Accel: ±4 g, 250 Hz ODR, LPF enabled (mode 3).
- Gyro: ±256 dps, 250 Hz ODR, LPF enabled (mode 3).

## API
- `qmi8658_init()` – probes address, configures CTRL1/2/3/5/7; idempotent.
- `qmi8658_read_sample(&qmi8658_sample_t)` – reads accel/gyro, returns g and dps.

## Auto-init
If `config.json` has `"auto_init":{"imu":true}` (default), `app_main` calls `qmi8658_init()` at boot and logs success/failure to SD.
