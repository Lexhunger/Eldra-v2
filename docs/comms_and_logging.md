# Comms and Logging Backbone

The comms backbone normalizes external inputs into a single command queue and provides a unified logging path. BLE, IR, future RFID, and a Wi-Fi server are all producers; the Emotion Engine consumes commands via the main loop.

## pet_command_t
```c
typedef struct {
    pet_command_type_t type;  // Command category
    uint32_t arg0;            // Optional parameter (e.g., food type, state id)
    uint32_t arg1;            // Optional parameter (e.g., flag value)
} pet_command_t;
```

## Command Queue
- **Producers:** BLE packets, IR codes, future RFID tags, Wi-Fi server messages.
- **Consumer:** Main app loop -> Emotion Engine event handlers.
- **Shape:** Small ring buffer for now; pushes from ISRs/tasks, drained by the main loop.

## Console / Command Router
- UART0 console task feeds lines into a transport-agnostic command router that HTTP can reuse later.
- Default commands:
  - `LOGS [tag] [level] [limit]` – dump recent in-RAM logs (ring buffer of last 100 entries).
  - `SETTIME <epoch_ms|dd/mm/yyyy-HH:MM:SS>` – set RTC from epoch or human format.
  - `SETDATE dd/mm/yyyy` – set RTC date only.
  - `SETCLOCK HH:MM:SS` – set RTC time only.
  - `BAT` – show battery voltage/percent.
  - `STATE` – show current emotion state/meters (if context provided).
  - `FEED [type]`, `PET`, `PLAY`, `FORCESTATE <id>` – enqueue pet commands.
- HTTP endpoints can call `comms_commands_process_line()` with the same strings to mirror console behavior.

### Initial Command Types
- **FEED** (`arg0` = food type)
- **PET**
- **PLAY**
- **DEBUG_FORCE_STATE** (`arg0` = state id)
- **SET_FLAG** (`arg0` = flag id, `arg1` = value)
- **FUTURE:** `RFID_ITEM`, `SERVER_SCRIPTED_EVENT`, etc.

## Transport Notes
- **BLE:** Flipper Zero or phone app sends compact binary commands; pet returns telemetry/state snapshots.
- **IR:** IR codes map into the same `pet_command_t` fields so they share the queue and handlers.
- **Future RFID:** Reader decodes tag -> command (e.g., feed item, toy, ritual token).
- **Wi-Fi:** Pet pushes logs/state to a home server and polls for queued commands to enqueue locally.

## SD and Remote Logging
All modules call `log_event(level, tag, msg)` rather than raw logging macros. It keeps an in-RAM ring (last 100 entries) for quick console/HTTP retrieval via `LOGS`. Later it will:
- Write to SD card with timestamps.
- Optionally mirror to the home server alongside telemetry snapshots.

Centralizing logging allows the Emotion Engine, sensors, comms, and actuators to gain persistent logs without code changes when storage or streaming is added.
