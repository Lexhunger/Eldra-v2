# Eldra-V2 Eye Animation System – Design Overview

## Goals
- Drive the 480×480 RGB round display via `esp_lcd_new_rgb_panel` with ~60 FPS updates in typical use.
- Use 11×11 pixel-art eyes (upscaled) for the "chibi horror" look.
- Render a full-screen eldritch horror eye for the cosmic mode.
- Handle moods, runes, and sensor-driven behaviors (reed switch, NFC/IR from Flipper Zero, etc.).
- Keep the system modular, data-driven, and efficient with CPU/RAM, leaning on SD card for heavy assets.

## High-Level Behavior
Eldra’s face is owned by a dedicated eye engine when in "pet mode." It swaps between two renderers:
- **CHIBI mode**: two small pixel-art eyes, cute/stylized.
- **ELDRITCH mode**: a single large cosmic eye with iris, sclera, and pulsing veins.

A cowl/hat with a magnet + reed switch controls the main transformation:
- Hat on → CHIBI mode (default).
- Hat off → animated transformation to ELDRITCH mode.

Runes (via NFC/IR/other) temporarily alter mood or visuals. Sensors (edge detection, sound, possible camera/face detection) influence mood and animation choice, not raw rendering. Debug/configuration happens on a separate debug/info screen (not on the pet face).

## Modes and Moods
### Modes
Top-level eye modes:
- `CHIBI`: two eyes, pixel-art, small region updates.
- `ELDRITCH`: full-screen cosmic horror eye, high detail.

The current mode selects the renderer and asset sets.

### Moods
Within a mode, the engine tracks a **mood** (Neutral, Happy, Sad, Angry, Sleepy, Hungry, Bored, Eldritch / Rune state). Moods affect:
- Idle animation selection and weighting.
- Eye color / palette.
- Pupil dilation and jitter.
- Reaction intensity to sensor input.

Runes act as mood modifiers that temporarily override or augment the base mood.

## Animation Engine
The engine centers on a context struct (e.g., `eldra_eyes_context_t`) holding:
- Current mode and mood.
- Active animation clip(s).
- Transform state (CHIBI ↔ ELDRITCH).
- Timing data (per-clip and per-transform).
- References to canvases / frame buffers.

Animation clips are named sequences (e.g., `IDLE_BLINK_SLOW`, `IDLE_SIDE_GLANCE`, `IDLE_LOOK_AROUND`, `IDLE_MICRO_TWITCH`, `TRANSFORM_CHIBI_TO_ELDRITCH`, `TRANSFORM_ELDRITCH_TO_CHIBI`) with frames, durations, and parameters.

A future "brain" layer will choose clips based on mode, mood, runes, and sensor inputs, maintain idle pools (6+ variants), and avoid obvious repetition.

Minimal public API (conceptual):
- `eldra_eyes_create / destroy`
- `eldra_eyes_set_mode`
- `eldra_eyes_set_mood`
- `eldra_eyes_trigger_transform_chibi_to_eldritch`
- `eldra_eyes_trigger_transform_eldritch_to_chibi`
- `eldra_eyes_update(dt_ms)`
- `eldra_eyes_render(framebuffer, fb_width, fb_height)`

`update()` advances timers and internal state; `render()` draws into a framebuffer region that is ultimately pushed to the panel.

## Rendering Paths
### CHIBI Rendering (Pixel-Art Eyes)
- Uses tiny 11×11 pixel-art sprites, upscaled (integer scaling) into small eye canvases (e.g., 96×96 or 128×128 per eye).
- Each eye has a position on the panel, scale factor, and pupil variant (size/shape).
- Sprites are ideally palette-indexed: sprite stores indices; palette maps indices to RGB565; palette swapping enables mood-based recoloring (happy = warmer colors, angry = reds, eldritch runes = unnatural hues).
- Only the eye regions are redrawn each frame (dirty rectangles) to maintain 60 FPS.

### ELDRITCH Rendering (Cosmic Eye)
- Uses full-screen (or tiled) RGB565 textures for base iris + pupil, sclera, and veins (overlays).
- Large textures live on the SD card and are loaded/streamed into PSRAM when needed.
- Typical render: mostly static base with procedural animation—pupil dilation/contraction, iris brightness shifts, vein pulsing, and subtle jitter.

## CHIBI ↔ ELDRITCH Transformation
The transformation is a real animation clip, not a hard cut. Example flow (CHIBI → ELDRITCH when the hat is removed):
1. **Phase A – Chibi reaction**: eyes widen, pupils dilate, slight color shift.
2. **Phase B – Merge**: two chibi eyes move toward center and scale up; pupil shape morphs toward a goat-like slit (using alternate sprites).
3. **Phase C – Reveal cosmic iris**: a high-res eldritch iris texture is revealed via a growing circular mask; chibi eyes under that region are overwritten/faded.
4. **Phase D – Background & veins**: dark background dissolves into bright sclera (radial or noise-based dissolve); veins overlay appears and starts pulsing.

At the end, the engine switches `mode = ELDRITCH` and continues with normal eldritch idle animations. The reverse uses its own clip with the same mechanism.

## Assets and Storage
- **Internal flash**: chibi sprites (small, pixel-art); core code and basic palettes.
- **PSRAM**: eye canvases for chibi rendering; eldritch textures/tiles loaded from SD.
- **SD card**: large eldritch eye textures/overlays; additional animation packs (future alt eyes, seasonal themes, etc.).

Art can be updated or expanded via SD card without reflashing firmware.

## Integration and Debugging
- Implemented as an ESP-IDF component at `components/eldra_eyes/`.
- Exposes a small API for mode/mood changes, triggering transformations, and later sensor/rune events.
- Debug/info lives on a separate screen (LVGL or custom text), outside the eye engine.

## Chibi Eye Reference Sprite (Palette-Indexed)
Reference PNG lives in `docs/` for artists only (not part of the build). Below is the canonical 8-color palette and 11×11 grid to embed later as palette-indexed data. Default behavior is to mirror for left/right eyes, but the engine will keep independent per-eye control (offsets, lid openness, brow tilt) so we can animate one eye differently (e.g., raise a brow, squint, wink).

### Palette (index → hex → use)
| Index | Hex     | Use                               |
|------:|---------|-----------------------------------|
| 0     | #000000 | Background / deepest dark         |
| 1     | #000220 | Very dark navy / outer edge shadow|
| 2     | #00134A | Deep blue shadow                  |
| 3     | #00358C | Mid blue / darker iris            |
| 4     | #0068B3 | Main iris blue                    |
| 5     | #0887D2 | Bright mid highlight              |
| 6     | #0D82CA | Softer highlight                  |
| 7     | #67C4F1 | Brightest specular / shine        |

### 11×11 Grid (palette indices)
```
row 1: {0, 0, 0, 0, 1, 1, 1, 0, 0, 0, 0}
row 2: {0, 0, 1, 2, 3, 3, 3, 2, 1, 0, 0}
row 3: {0, 1, 2, 3, 4, 4, 4, 3, 2, 1, 0}
row 4: {0, 2, 3, 6, 2, 1, 2, 5, 3, 2, 1}
row 5: {1, 3, 4, 2, 3, 0, 7, 2, 6, 3, 1}
row 6: {1, 3, 4, 3, 1, 0, 0, 3, 6, 3, 1}
row 7: {1, 3, 6, 3, 1, 0, 0, 3, 6, 3, 1}
row 8: {1, 3, 4, 4, 3, 1, 3, 4, 4, 3, 1}
row 9: {0, 2, 3, 5, 5, 4, 6, 4, 3, 2, 0}
row10: {0, 0, 1, 2, 3, 3, 3, 2, 1, 0, 0}
row11: {0, 0, 0, 1, 1, 2, 1, 1, 0, 0, 0}
```

### Notes for Implementation (later)
- Keep palette-indexed storage for compact flash usage; map to RGB565 at runtime.
- Store a single 11×11 sprite and mirror for the opposite eye, but expose per-eye transforms:
  - Position (x, y) offsets; optional scale overrides.
  - Lid openness / blink per eye.
  - Brow tilt/skew per eye for expressions (raised brow, squint).
  - Pupil size/offset per eye.
- Consider a tiny per-eye mask for eyebrow accents (1-bit overlay) to avoid baking eyebrows into the base sprite.

## Roadmap / Implementation Phases
1. **Component skeleton**: `eldra_eyes.h` with enums, context type, public API; `eldra_eyes.c` with basic context and stubbed functions.
2. **Transform state & timing**: add a `transform_state` struct for CHIBI ↔ ELDRITCH; implement simple timing in `update()` (no rendering yet).
3. **CHIBI rendering MVP**: add eye canvases, render colored test blocks; render a simple 11×11 test sprite with upscaling into canvases.
4. **Basic idle & blink clips** (CHIBI only).
5. **ELDRITCH rendering MVP**: load a single full-screen test texture (flash or SD) and display as the eldritch base.
6. **Transformation clip**: implement the CHIBI → ELDRITCH sequence with basic effects.
7. **Moods, runes, and sensor integration**: drive clip selection/parameters from mood/rune/sensor state.
8. **Polish & expansion**: more idle variants, moods, rune visuals, complex dissolves/noise masks/pulsing effects.
