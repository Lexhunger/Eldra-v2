# Eldra-V2
A cosmic-horror-cute desktop pet powered by ESP-IDF 5.5.1 and the Waveshare ESP32-S3 2.8" Round LCD board.

## Project Overview
Eldra is a small eldritch-cute desktop pet built on an ESP32-S3 with a round TFT display, sensors, haptics, and servos. It will juggle emotional states, react to touchless inputs, and express itself through animated eyes, vibration, audio chirps, and light motion while logging its inner life for later replay.

## Current Status (Dec 2025)
- Eyes: Centering tools (test pattern, `disp_center`, `eyes_offset`), newline/prompt fixes, battery console command, and SD/Wi-Fi/RTC/IMU console bridges merged from the driver demo.
- Emotion Engine: Satiety bands (hungry/hangry/stuffed/food-coma), time-based decay, quiet-hours auto-sleep (10p–8a idle 10m), affect + needs mask for blended expressions, gated eldritch (only when fed/energized).
- Config: `sleep.start_hour/end_hour`, `emotion.mood_log_interval_minutes` (0=off) now persisted on SD.
- Cloud: Client pointed to `http://sn-llm-core.local:8030` by default, bearer token required (e.g., `the-old-ones`); push state/logs, poll commands. Set via `cloud_set <base_url> <token>`.
- Console controls: `emo_feed 0|1|2` (big/small/snack), `emo_pet [small|big]`, `emo_play`, `emo_state`, `emo_state_show` / `emo_needs`, `emo_weight`, `eyes_sleep_force`, `eyes_dizzy` (status/preset/per-field/full-set), battery readout, eyes test/offset commands.
- Sensors: IMU shake → dizzy/fear; edge trigger; battery smoothing with hysteresis; EXIO/wifi/sd bridges online.
- Build: ESP-IDF 5.5.1, `idf55` helper to enter the env; `idf.py build/flash/monitor`.

## High-Level Architecture
- **Emotion Engine (brain):** Tracks meters (hunger, happiness, energy, social, fear, eldritch charge) and selects a named state that drives animations and haptics.
- **Comms & Logging Backbone:** BLE + IR inputs (RFID future), Wi-Fi server sync, and SD logging funnel events into a common command queue while exporting telemetry.
- **Sensors:** IMU for motion/tilt/shake, ToF for proximity, reed switch for hood, hall sensor for magnets, plus other environmental sensors as they are added.
- **Actuators:** Round display (eyes/expressions), buzzer for chirps, vibro motor for haptics, and PCA9685-driven servos for small motions and poses.

### Runtime Pipelines (non-blocking)
- **Render pipe:** `app_main` composes a full framebuffer each tick (`compose_frame`), clears it, draws eyes, then glyphs, and blits once. No network/SD inside the render path.
- **Eyes engine:** Pure CPU/pixel code (`eldra_eyes_render`). State is set via setters (console/bridge/cloud), never via blocking calls.
- **Glyphs layer:** Optional forehead runes drawn after eyes; assets are pre-decoded and cached. Offsets/scale are set through console/sleep logic.
- **Console REPL:** Runs in its own task. Commands talk to modules through bridges (wifi/sd/rtc/imu/emotion/glyph) so UI never blocks rendering.
- **Cloud task:** Polls/pushes on its own intervals; does not touch eyes directly. All module mutations go through bridge handlers.
- **Logging:** Ring buffer + SD writer; console prompt is reprinted after async logs to keep the REPL responsive.

Rule of thumb: modules never block each other; cross-module actions go through bridge commands/queues, render path is pull-only from shared state.

### Scaffold + Silos Model
- **Scaffold:** event queue/bridge code + console command bridge + `app_main` orchestration/runtime loops.
- **Silos:** render/eyes/glyphs, sleep, cloud, wifi, sensors, storage, emotion.
- **Contract:** silos do not call each other in blocking paths; scaffold carries intents/events and status updates.

### Component Tagging (Debug Isolation)
- Every module logs with a stable tag through `EL_LOG*` wrappers.
- Use `logtags` to list discovered tags and current console routing rules.
- Use `logfocus` to isolate a subsystem quickly:
  - `logfocus display`
  - `logfocus cloud`
  - `logfocus sleep`
  - `logfocus scaffold`
  - `logfocus off` (clear focus rules)

## Development Roadmap
1. **Emotion Engine Core:** Define emotion meters, state selection, and event-driven reactions with output hooks for animation and haptics.
2. **Comms & Logging Backbone:** Build a shared command queue for BLE/IR/RFID/Wi-Fi inputs and a unified logging path to SD/server.
3. **Virtual Inputs / Dev Controls:** Add developer controls to inject commands and simulate sensors for fast iteration.
4. **Physical Sensors:** Integrate IMU, ToF, reed switch, hall sensors, and other inputs that feed the Emotion Engine.
5. **Motion & Haptics:** Drive vibration motor and servos via PCA9685 for gestures, idle motion, and reactions.
6. **Audio & Mic / Phrase Recognition:** Add buzzer chirps, expand audio playback, and experiment with phrase detection hooks.
7. **Personality & Feature Modules:** Layer behaviors, routines, and contextual reactions on top of the Emotion Engine.
8. **Internal Frame Prototype:** Build the internal mechanical frame and mounting points for electronics.
9. **Body Shell Prototype:** Prototype the exterior shell for the “eldritch-cute” look and feel.
10. **Hood Prototype:** Refine the magnetized hood/cowl mechanism with reed switch feedback and supporting animations.

## What is Eldra-V2?
Eldra-V2 is the next-generation rewrite of the Eldra desktop pet. The Waveshare board provides:
- 480x480 RGB parallel IPS panel (no SPI bottleneck)
- ESP32-S3 dual-core performance with 8 MB PSRAM and 16 MB flash
- Onboard QMI8658 6-axis IMU, RTC, battery charging, SD card slot, and USB-C
- A platform tuned for real-time animation and interaction

Goal: a cute-but-cosmic companion with expressive pixel-art eyes, procedural moods, subtle animation, ambient effects, and eventual light physical motion.

## Eye Animation System (Eldra-V2)
Eldra's face is driven by a modular eye engine that swaps between two renderers: CHIBI mode uses tiny 11x11 pixel-art eyes (scaled up and redrawn in small regions), while ELDRITCH mode is a full-screen cosmic eye with layered textures and procedural pulses. The system is built to sustain 60 FPS on the round 480x480 RGB panel.

Moods and rune modifiers steer idle clip selection, palette swaps, and how strongly the eyes react to sensors. A cowl-mounted magnet + reed switch controls the signature transformation: hat on keeps Eldra in CHIBI, hat off triggers an animated merge and reveal into ELDRITCH, with a mirrored sequence on return.

Full design details - modes, moods, renderers, transform flow, assets, and roadmap - are documented in `docs/eldra_eyes_animation.md`.

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
  - `idf55` sets `IDF_PATH=C:\Users\bmpor\esp\v5.5.1\esp-idf` and activates the bundled Python/toolchain environment for this project.
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



