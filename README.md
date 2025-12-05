# Eldra-V2
A cosmic-horror-cute desktop pet powered by ESP-IDF 5.5.1 and the Waveshare ESP32-S3 2.8" Round LCD board.

## What is Eldra-V2?
Eldra-V2 is the next-generation rewrite of the Eldra desktop pet. The Waveshare board provides:
- 480x480 RGB parallel IPS panel (no SPI bottleneck)
- ESP32-S3 dual-core performance with 8 MB PSRAM and 16 MB flash
- Onboard QMI8658 6-axis IMU, RTC, battery charging, SD card slot, and USB-C
- A platform tuned for real-time animation and interaction

Goal: a cute-but-cosmic companion with expressive pixel-art eyes, procedural moods, subtle animation, ambient effects, and eventual light physical motion.

## Eye Animation System (Eldra-V2)
Eldra’s face is driven by a modular eye engine that swaps between two renderers: CHIBI mode uses tiny 11×11 pixel-art eyes (scaled up and redrawn in small regions), while ELDRITCH mode is a full-screen cosmic eye with layered textures and procedural pulses. The system is built to sustain 60 FPS on the round 480×480 RGB panel.

Moods and rune modifiers steer idle clip selection, palette swaps, and how strongly the eyes react to sensors. A cowl-mounted magnet + reed switch controls the signature transformation: hat on keeps Eldra in CHIBI, hat off triggers an animated merge and reveal into ELDRITCH, with a mirrored sequence on return.

Full design details—modes, moods, renderers, transform flow, assets, and roadmap—are documented in `docs/eldra_eyes_animation.md`.

## Project Goals
**Phase 1 - Core Display + Eyes**
- Render 11x11 pixel-art eyes with an 8-color RGB565 palette
- Center and scale the eyes on screen
- Animate idle, blink, look-left/right, and mood transitions (blue + red + cosmic glow)

**Phase 2 - Interactivity**
- Use IMU data for tilt reactions and "wake up" from movement
- Add touchless interaction via movement intensity
- Add mood logic (calm, curious, annoyed, eldritch)

**Phase 3 - Physical Motion**
- 16-channel servo driver for tentacle movement
- Optional vibration motor
- Optional Hall/ToF/sound sensors

**Phase 4 - Personality Engine**
- Lightweight behavior tree
- Mood decay/rise, idle expressions, and event triggers (shake, pick up, pet via accelerometer)

## Hardware
**Primary board**
- Waveshare ESP32-S3 Round LCD (2.8", 480x480)  
  Link: https://www.amazon.ca/gp/product/B0DX6TF5BN
- ESP32-S3 dual-core Xtensa LX7 @ 240 MHz
- 512 KB SRAM + 8 MB PSRAM, 16 MB flash
- RGB round display, battery charging, QMI8658 IMU, TF/SD slot, USB-C

**External hardware (future phases)**
- PCA9685 (or similar) 16-channel servo driver
- Micro 9g servos
- Coin cell RTC battery
- 32 GB SD card
- Optional sensors: VL53L0X ToF, vibration motor, Hall sensors, addressable LEDs

## Pixel-Art Eye System
- Base eye: 11x11 grid with an 8-color palette chosen for clarity, smooth shading, easy mood transitions, and efficient memory.

| Index | Hex     | RGB565 | Description                  |
|------:|---------|--------|------------------------------|
| 0     | #000000 | 0x0000 | Black                        |
| 1     | #001020 | 0x0821 | Deep blue shadow             |
| 2     | #002060 | 0x1042 | Cool mid-blue                |
| 3     | #003A9A | 0x1D53 | Bright blue                  |
| 4     | #005CC0 | 0x2E7B | Light blue                   |
| 5     | #0A82D0 | 0x50FF | Cyan-blue highlight          |
| 6     | #67C4F1 | 0x7FFF | Pale highlight               |
| 7     | #FFFFFF | 0xFFFF | Spark highlight (optional)   |

The palette can be recolored for red, purple, green, or cosmic variants.

### Eye Animation Strategy
- **Option A - Prebaked frames:** store 11x11 palette-index arrays; very fast and compact for idle/blink/look animations.
- **Option B - Procedural shading:** compute colors based on distance to center; easy mood shifts.
- **Option C - Hybrid (recommended):** prerender base frames and apply light procedural noise or color shifts for a "cute cosmic horror" feel.

## Software Architecture
```
Eldra_V2/
|-- main/
|   |-- main.c                   # App entry, loop, mode state machine
|   `-- CMakeLists.txt
|-- components/
|   |-- eldra_display_round/     # Wrapper for esp_lcd RGB panel
|   |   |-- eldra_display_round.c
|   |   |-- include/
|   |   |   `-- eldra_display_round.h
|   |   `-- CMakeLists.txt
|   |-- eldra_eyes/              # Pixel eye renderer + animations
|   |   |-- eldra_eyes.c
|   |   |-- include/
|   |   |   `-- eldra_eyes.h
|   |   `-- CMakeLists.txt
|   |-- eldra_sensors/           # IMU + inputs (future)
|   |-- eldra_mood/              # Mood logic engine (future)
|   `-- eldra_motion/            # Servo control (future)
|-- sdkconfig.defaults
|-- partitions.csv
`-- README.md
```

## Current TODO Highlights
**Phase 1 - Display & Eye Bootstrapping**
- Implement `eldra_display_round.c` with `esp_lcd_new_rgb_panel`
- Screen init test (solid colors)
- Draw centered 11x11 test grid
- Convert final blue-eye 11x11 grid into an embedded array
- Map 8-color palette to RGB565
- Render a static eye at 1x scale, then scale up to fill the screen
- Blink animation (3 frames) and idle micro-motion (wiggle/shimmer)
- 60 FPS task loop

**Phase 2 - Sensors + Interaction**
- QMI8658 IMU driver wrapper, shake/tilt detection, wake/sleep reactions

**Phase 3 - Mood Engine**
- Mood struct `{calm, curious, agitated, cosmic}`
- Smooth palette transitions, timers, movement triggers, idle personality cycles

**Phase 4 - Physical Body**
- Servo driver integration, tentacle movement tests, touch/shake reactions, timed animations

## Setup Prerequisites
- Install ESP-IDF 5.5.1 and ensure its tools are on PATH.
- Activate the ESP-IDF environment (PowerShell helper): `idf55`
- Set the build target once per build directory: `idf.py set-target esp32s3`

## Build & Flash
Activate the ESP-IDF 5.5.1 environment:
```
idf55
```

Build:
```
idf.py build
```

Flash and monitor:
```
idf.py flash monitor
```

## License
MIT License
