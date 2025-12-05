# Eldra Eyes Component

Planned ESP-IDF component that owns Eldra-V2’s eye animation system. It will provide a small C API to create/destroy the eye context, update state, render into a framebuffer, switch between CHIBI (pixel-art) and ELDRITCH (cosmic) modes, adjust moods, and trigger the cowl-driven transformation clips.

## Planned API Surface (high level)
- `eldra_eyes_create` / `eldra_eyes_destroy`
- `eldra_eyes_update(dt_ms)` and `eldra_eyes_render(framebuffer, fb_width, fb_height)`
- Mode/mood control: `eldra_eyes_set_mode`, `eldra_eyes_set_mood`
- Transform triggers: `eldra_eyes_trigger_transform_chibi_to_eldritch`, `eldra_eyes_trigger_transform_eldritch_to_chibi`

See `docs/eldra_eyes_animation.md` for the full design: modes, moods, runes, renderers, transformation flow, assets, and roadmap.
