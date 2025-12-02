# Contributing to Eldra-V2

Welcome, squishy human (and hello, AI assistant). This project is built with a human + AI pairing model in mind: humans own intent, architecture, and final review; AI helps with execution, scaffolding, and refactors. Keep the repo readable, well-documented, and consistent.

---

## 1. Project Overview
Eldra-V2 targets ESP-IDF 5.5.1 on the Waveshare ESP32-S3 2.8" Round Display. Goals:
- Render 11x11 pixel-art eyes (scaled up) in a cosmic-cute style.
- Drive the 480x480 RGB panel via `esp_lcd_new_rgb_panel`.
- Build modular components for display, eyes/animation, sensors, mood engine, and physical motion.
- Treat `docs/` as a source of truth for hardware, design, and behavior.

---

## 2. Branch & PR Workflow
- `main` must stay buildable and flashable.
- Use small, focused feature branches (e.g., `feature/display-pipeline`, `feature/eyes-v1`, `feature/imu-integration`).
- PRs should include:
  - short description of the change
  - mention of updated docs (`docs/...`)
  - notes about pin maps, timing, or rendering behavior if touched

---

## 3. Code Style & Structure
### Language / Tooling
- C with ESP-IDF 5.5.1 and CMake (per ESP-IDF conventions). Avoid sneaking in C++ or alternative build systems.

### Style
- Favor clear, explicit C over clever tricks.
- Keep functions small and behavior-driven in naming.
- Comment non-obvious logic (bit fiddling, color packing, timing hacks, hardware errata/workarounds).

### File Layout
```
main/                     # boot, app_main, top-level init
components/
  eldra_display_round/    # display driver (RGB panel, init, drawing)
  eldra_eyes/             # eye renderer + animation state
  eldra_sensors/          # IMU, buttons, etc. (future)
  eldra_mood/             # mood engine (future)
  eldra_motion/           # servo/tentacle motion (future)
docs/
  hardware/               # board, pin maps, modules
  firmware/               # display pipeline, eye renderer, animation
  reference/              # RGB565, setup notes, file layout
  roadmap.md              # project phases and TODOs
```

---

## 4. Using AI Assistants (ChatGPT / Codex / Copilot)
AI is a tool, not the owner of the code.

### Rules
- Always read diffs; never blindly accept generated code.
- No single-PR "rewrite the entire project" changes.
- AI must not:
  - change hardware pin mappings without explicit intent
  - downgrade ESP-IDF or alter the build system
  - rewrite working modules outside the scope of the change

### Preferred Prompt Style
- Do: "Given `eldra_display_round.c`, add a function to draw a filled circle without breaking current APIs."
- Do: "Using `docs/firmware/eye_renderer_11x11.md`, generate a C function that converts a palette grid into RGB565 and draws it centered."
- Avoid: "Rewrite the entire project to be object-oriented" or "Convert everything to C++ and LVGL in one go."

### AI Must Respect Project Conventions
- Keep function names and public APIs stable unless intentionally changing them (and document it).
- Use existing helpers (e.g., `eldra_display_fill_rect`).
- Honor `SCREEN_W/SCREEN_H` and centralization logic; avoid hard-coding dimensions.

---

## 5. Display & Eyes - Special Rules
These modules are core to the project.

### Display
- `eldra_display_round` is the only place that should talk directly to `esp_lcd`.
- Other modules must use the display component API (e.g., `eldra_display_init`, `eldra_display_clear`, `eldra_display_fill_rect`).
- Do not scatter `esp_lcd_panel_draw_bitmap` calls across modules.

### Eye Renderer
- The 11x11 grid and 8-color palette are canonical.
- Rendering should use palette indices, not raw RGB everywhere.
- Keep it scaleable (nearest-neighbor scaling) and data-driven.
- Do not hard-code screen coordinates in multiple places.
- Keep rendering logic in `eldra_eyes`, not `main.c`.

---

## 6. Documentation Requirements
- Update docs when behavior changes:
  - Pin changes: `docs/reference/pin_maps.md`
  - RGB565 palette: `docs/firmware/palette_rgb565.md` (when it exists)
  - Eye layout/animation: `docs/firmware/eye_renderer_11x11.md` or `animation_strategy.md`
  - New module: mention in `docs/reference/file_structure_overview.md` and, if relevant, in `README.md`
- Prefer short, structured sections over walls of text. Treat docs as source of truth.

---

## 7. Commit Message Conventions
- Use short, descriptive messages:
  - `feat: add basic 11x11 eye renderer`
  - `fix: correct RGB565 palette index mapping`
  - `refactor: move display init into eldra_display_round`
  - `docs: add pin map and palette docs`
- Avoid vague messages like `misc changes` or `AI did this`.

---

## 8. Checklists
### Before Opening a PR
- `idf.py build` passes
- No new warnings unless documented
- Code is formatted and commented where non-obvious
- Docs updated if needed
- Changes are logically grouped and scoped

### AI-Assisted Change Checklist
- Reviewed the entire diff manually
- No unexpected hardware pin changes
- ESP-IDF version/target/build system unchanged
- No modules rewritten outside the intended scope
- Constants (screen dimensions, palette indices, etc.) remain correct
- Behavior confirmed on hardware or logically inspected for correctness
- Large generated sections include inline comments or explanation

---

## 9. Respect the Hardware
- Avoid busy-loops that spike CPU usage without reason.
- Do not hammer display updates in tight loops when unnecessary.
- Consider power consumption when designing animations and background tasks.
- Eldra should feel alive, not overheated.
