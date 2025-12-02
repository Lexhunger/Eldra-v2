# QMI8658 IMU Notes

The QMI8658 is a 6-axis sensor combining a 3-axis accelerometer and 3-axis gyroscope with high-speed sampling and built-in filtering. It enables Eldra-V2 to react to tilting, sudden shakes, taps, being picked up, orientation changes, and idle/sleep/wake states.

---

## Key Features
### Accel Ranges
+/-2g to +/-16g

### Gyro Ranges
+/-125 deg/s to +/-2000 deg/s

### Output Data Rate
Up to 800 Hz

### Communication
- I2C (default)
- SPI mode is available but not planned

---

## Planned Usage in Eldra-V2
### Phase 2
- Tilt-driven eye direction
- Shake-driven surprise animation
- Pick-up-triggered wake-up animation

### Phase 3
- Movement-based mood changes
- Persistent orientation sensing
- Idle detection for sleep

---

## Notes
Waveshare SDK examples include basic drivers. Eldra-V2 will use a modular component so other modules do not talk directly to the IMU driver.
