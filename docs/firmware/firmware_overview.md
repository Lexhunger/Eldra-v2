# Firmware Overview (Eldra-V2)

This document explains how the firmware pieces fit together today and what is planned next. It complements `docs/eldra_eyes_animation.md`, which covers the eye engine design in depth.

## High-Level Loop
- `app_main` (see `main/main.c`) initializes sensors, then the display/LVGL stack, and currently launches the vendor LVGL demo as a temporary debug surface.
- A simple loop calls `lv_timer_handler()` every ~10 ms. Later this loop will hand control to the Eldra eyes renderer instead of the vendor demo UI.

## Components
- `components/eldra_display_round/`
  - Wraps vendor panel + LVGL bring-up (`eldra_display_round_init`), backlight control, and exposes panel dimensions. Keeps the rest of the firmware insulated from panel specifics.
- `components/eldra_sensors/`
  - Wraps vendor sensor/peripheral initialization for I2C, IO expander, battery, RTC, IMU, SD, wireless, buttons. Spawns a background polling task (`driver_loop`) that keeps the IMU/RTC/battery drivers active.
- `components/eldra_eyes/`
  - New eye engine skeleton. Public API is stubbed (create/destroy, set mode/mood, trigger transforms, update, render) and will grow into the full system described in `docs/eldra_eyes_animation.md`.

## Current Data Flow
1) Sensors init and start a background polling task.
2) Display init brings up the RGB panel and LVGL.
3) LVGL demo owns the screen (temporary).
4) Main loop services LVGL; no other tasks touch the display yet.

## Near-Term Evolution
- Replace the vendor LVGL demo with the Eldra eyes renderer.
- Extend `eldra_eyes_update/render` to manage animation clips, transformation timing, and drawing into a framebuffer that the display component pushes.
- Add hooks for sensor/rune events to feed mode/mood and animation selection.
- Add a lightweight debug/info UI (separate screen) to inspect mode/mood/rune/sensor states without interfering with the pet face.

## References
- Eye system design: `docs/eldra_eyes_animation.md`
- Display wrapper: `components/eldra_display_round/`
- Sensor wrapper: `components/eldra_sensors/`
- Entry point: `main/main.c`
- Docs and reference assets (PNGs, markdown) are not part of the build; CMake only compiles code under `main/` and `components/`, and third-party deps under `managed_components/`.
