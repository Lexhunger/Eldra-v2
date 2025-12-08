# Emotion Engine

Eldra's Emotion Engine keeps a set of bounded meters, resolves them into a named emotional state, and notifies the rest of the system via output hooks. It has no direct hardware knowledge and relies solely on events sent by other modules.

## Meters (0-100)
- **happiness:** General mood; drops if other needs stay unmet.
- **hunger:** Higher = more hungry; rises over time and after energy use.
- **energy:** Readiness to act; drains with play/motion, recovers slowly when calm.
- **social:** Desire for interaction; decays without engagement.
- **fear:** Threat/unease; spikes on edge detection, harsh motion, or loud inputs.
- **eldritch_charge:** Builds toward the cosmic/eldritch reveal; rises slowly or via special triggers.

## Named States
- **NEUTRAL**
- **HAPPY**
- **SAD**
- **LONELY**
- **SLEEPY**
- **HUNGRY**
- **PLAYFUL**
- **ELDRITCH**
- **SCARED**
- **DIZZY**

## State Priority Rules
1. **DIZZY** - Active while a dizziness timer is running (e.g., after strong shake/spin).
2. **SCARED** - `fear` above threshold or an active scare timer.
3. **ELDRITCH** - `eldritch_charge` high enough to trigger the reveal posture.
4. **HUNGRY** - `hunger` above threshold for a sustained period.
5. **SLEEPY** - `energy` low (optionally battery low or late night once hardware hooks exist).
6. **LONELY** - `social` meter low due to a lack of interaction.
7. **SAD** - `happiness` low plus another need (hunger/social/energy) also low.
8. **PLAYFUL** - `happiness` and `social` high with a recent play timestamp.
9. **HAPPY** - `happiness` high and no higher-priority need active.
10. **NEUTRAL** - Default fallback when no rule triggers.

## Event API (hardware-agnostic)
These entry points are called by other modules. They adjust meters, track timestamps, and immediately re-select the active state.

- `emotion_on_tick(uint32_t now_ms)` - Periodic update; decays/raises meters and refreshes timers.
- `emotion_on_feed(uint32_t food_type)`
- `emotion_on_pet(void)`
- `emotion_on_play(void)`
- `emotion_on_shake(int intensity)`
- `emotion_on_edge_detected(void)`
- `emotion_on_hood_state_changed(bool hood_closed)`
- `emotion_on_voice_command(uint32_t phrase_id)`

## Output Hooks (implemented elsewhere)
The Emotion Engine only signals intent; display, haptics, and audio live in other modules.

- `emotion_output_set_state(emotion_state_t state)`
- Optional per-domain hooks such as `emotion_output_set_animation(...)` or `emotion_output_set_haptics(...)`

The default stub implementation simply logs state changes; the real render/haptics/audio layers will override these hooks later.

## Data Flow
```
inputs (events) -> meter updates -> state selection -> output hooks
```

Events flow in from sensors (IMU, ToF, hood switch, voice triggers) and virtual commands (BLE/IR/RFID/Wi-Fi). The Emotion Engine adjusts meters, evaluates the priority rules above, and emits the active state to the output hooks so the UI, servos, haptics, and audio layers can respond.
