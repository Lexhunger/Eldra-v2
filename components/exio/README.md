# TCA9554PWR IO Expander Driver

Provides minimal control of the TCA9554PWR IO expander (EXIO). Used for LCD reset/CS and buzzer on this board.

## Wiring (board default)
- I2C SDA: GPIO15
- I2C SCL: GPIO7
- IO1: LCD reset
- IO3: LCD CS
- IO8: Buzzer (active high)

## Dependencies
- ESP-IDF `driver/i2c`, `esp_err`

## Usage
```c
EXIO_Init();             // init I2C + expander; applies safe defaults for LCD reset/CS and buzzer
Set_EXIO(pin, level);    // set a single pin (1-8)
Read_EXIO(pin);          // read a single pin
```

Notes:
- Init defaults all pins to outputs and then applies safe line states:
  - IO1 (LCD reset): high (released)
  - IO3 (LCD CS): high (deasserted)
  - IO8 (buzzer): low (off)
- Adjust `I2C_MASTER_SDA_IO/SCL_IO` in `TCA9554PWR.h` for other boards.
