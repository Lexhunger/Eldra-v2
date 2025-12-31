# IMU Console Bridge

Registers two console commands (USB-Serial/JTAG REPL):
- `imu_init` – initialize the QMI8658 IMU on I2C0 (idempotent).
- `imu_read` – read one sample; prints accel (g) and gyro (dps) XYZ.

Depends on `imu_qmi8658`, `console`, `freertos`. Non-blocking; runs in the console task.
