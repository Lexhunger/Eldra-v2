# Battery & Power Notes

Eldra-V2 supports portable use via the Waveshare board's onboard battery system.

---

## Supported Batteries
- Single-cell LiPo / Li-Ion (3.7 V nominal)
- Recommended: 500-1500 mAh
- JST-PH 2.0 connector

---

## Charging
- Charge via USB-C
- Onboard charger handles CC/CV, protection, and charge-state monitoring

---

## Power Draw (projected)
- LCD at full brightness: 120-150 mA
- ESP32-S3 running animations: 60-80 mA
- IMU: 1-3 mA
- Idle (screen dimmed): ~30 mA

---

## Future Enhancements
- Dynamic dimming based on mood
- Low-battery "tired" behavior
- Sleep mode when untouched
