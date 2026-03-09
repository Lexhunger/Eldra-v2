# Console Emotion Bridge

Registers emotion and eyes-related console commands.

Common commands:
- `emo_state_show` - print current emotion state and meters.
- `emo_set <meter> <value>` - set one meter for testing.
- `emo_set all <h sat e s f eld> [batt]` - set all meters.
- `emo_weight <target> <pct>` - set one affect weight.
- `emo_weight all <h sat e s f>` - set all affect weights.
- `eyes_sleep_force <off|light|heavy|status>` - force sleepy-eye overlay for testing.
- `eyes_dizzy ...` - tune IMU dizzy sensitivity.

## eyes_dizzy

Usage:
- `eyes_dizzy status`
- `eyes_dizzy preset <easy|normal|hard> [persist|temp]`
- `eyes_dizzy <gyro_thresh|spike|gdev|accum_ms|cooldown_ms> <value> [persist|temp]`
- `eyes_dizzy set <gyro_thresh_dps> <spike_dps> <gdev_thresh> <accum_ms> <cooldown_ms> [persist|temp]`

Field meaning:
- `gyro_thresh` - sustained angular-rate threshold in deg/sec.
- `spike` - instant angular-rate trigger in deg/sec (must be at least `gyro_thresh + 5`).
- `gdev` - accel magnitude deviation from 1g (unitless, e.g. `0.30`).
- `accum_ms` - time above `gyro_thresh` needed to trigger (ms).
- `cooldown_ms` - refractory time after trigger (ms).

Runtime clamp/range:
- `gyro_thresh`: `20.0..500.0`
- `spike`: `30.0..1000.0` (and auto-raised to `gyro_thresh + 5` if needed)
- `gdev`: `0.05..1.00`
- `accum_ms`: `50..5000`
- `cooldown_ms`: `250..60000`

Persistence behavior:
- Default mode is `persist` (saved to `/sdcard/config.json`).
- Use `temp` for runtime-only changes that do not save.

Examples:
- `eyes_dizzy status`
- `eyes_dizzy preset hard`
- `eyes_dizzy gyro_thresh 75`
- `eyes_dizzy cooldown_ms 7000 temp`
- `eyes_dizzy set 70 160 0.35 250 5000`

Tip:
- Use `config_show` to confirm persisted values after saving.
