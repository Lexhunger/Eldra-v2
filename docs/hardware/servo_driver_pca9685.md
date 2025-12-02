# PCA9685 Servo Driver Notes

To support tentacle movement or physical Eldra expressions, Eldra-V2 may use a PCA9685 16-channel PWM servo driver module.

---

## Features
- 16 independent PWM outputs
- 12-bit resolution (4096 steps)
- I2C control
- Supports ~50-330 Hz servo frequency
- Suitable for micro servos, eyelid motors, tentacle articulation, and idle twitches

---

## Wiring to ESP32-S3 Board

| PCA9685 | ESP32-S3 Board |
|---------|----------------|
| VCC     | 3.3V           |
| GND     | GND            |
| SDA     | GPIOxx (TBD)   |
| SCL     | GPIOxx (TBD)   |

Servo rail is powered by an external 5V supply.

---

## Planned Component
- Provide a simple API to set servo angles or raw PWM values
- Keep I2C pin assignments configurable per build/board
- Avoid blocking calls; integrate with the main loop cadence
