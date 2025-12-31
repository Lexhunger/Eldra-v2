# Emotion Engine

Eldra's Emotion Engine keeps a set of bounded meters, resolves them into a named emotional state, and notifies the rest of the system via output hooks. It has no direct hardware knowledge and relies solely on events sent by other modules.

## Meters
- **happiness (0-100):** Overall mood tilt; rises with play/pet/voice, falls when needs unmet.
- **hunger (satiety 0-130):** Higher = fuller. Decays slowly (~24h from stuffed to hungry). Bands: ≤15 hangry, ≤30 hungry, >90 stuffed (energy drain), >100 food coma (sleepy until <98).
- **energy (0-100):** Drifts down over time; recovers faster while sleeping/forced sleep; drains on play and stuffed penalty.
- **social (0-100):** Decays over time; boosted by pet (small/big), play, voice.
- **fear (0-100):** Spikes on edge detection/shake; decays passively.
- **eldritch_charge (0-100):** Slow charge + shake boosts; gated by satiety/energy/battery before entering ELDRITCH.
- **battery_percent (0-100):** Passed in from sensors; used to gate sleepy/eldritch.
- Config knobs: `sleep.start_hour`/`sleep.end_hour` (default 22–8), `emotion.mood_log_interval_minutes` (default 20, 0=off).

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
1. **DIZZY** – dizziness timer active (shake intensity).
2. **SCARED** – fear above threshold or scare timer.
3. **SLEEPY (forced)** – quiet-hours idle >10m sets `sleep_forced`; any interaction wakes.
4. **ELDRITCH** – charge high AND satiety >30 AND energy/battery OK.
5. **HUNGRY / HANGRY** – satiety bands: ≤15 hangry, ≤30 hungry (SAD if also lonely/low happiness).
6. **SLEEPY (low energy/batt)** – energy ≤30 or battery low.
7. **LONELY / ISOLATED** – social ≤25 (≤10 skews SAD).
8. **SAD** – low happiness plus another need low.
9. **PLAYFUL** – happy + social high with recent play.
10. **HAPPY** – happiness high and no higher-priority need.
11. **NEUTRAL** – fallback.

## Needs Mask and Affect
- **Needs mask:** `emotion_get_needs()` returns bitflags (hungry, lonely, sleepy, scared, playful, eldritch-ready) for blending expressions without changing the hard state.
- **Affect:** `emotion_get_affect(&valence,&arousal)` derives neutral/happy/sad/angry/excited plus signed valence/arousal for finer visual blending.
- **Periodic logging:** meters + affect + needs are logged every `mood_log_interval_minutes` if enabled.

## Event API (hardware-agnostic)
These entry points are called by other modules; they adjust meters, track timestamps, clear forced-sleep, and re-select state.

- `emotion_on_tick(uint32_t now_ms)` – Passive drift, sleep-idle check (quiet hours), affect update.
- `emotion_on_feed(uint32_t food_type)` – 0=big (+60), 1=small (+30), 2+=snack (+5) satiety.
- `emotion_on_pet(uint32_t intensity)` – 0=small, 1=big (larger social/happy).
- `emotion_on_play(void)` – Boost happy/social, cost energy, raise hunger.
- `emotion_on_shake(int intensity)` – Boost fear + eldritch charge, starts dizzy timer.
- `emotion_on_edge_detected(void)` – Fear spike + scare timer.
- `emotion_on_hood_state_changed(bool hood_closed)` – Energy/social tweaks, wakes pet.
- `emotion_on_voice_command(uint32_t phrase_id)` – Happy/social boost, wakes pet.
- `emotion_set_battery_percent(uint8_t)` – External battery gate for sleepy/eldritch.

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
