# Console RTC Bridge

Registers console commands for the RTC driver:
- `rtc_get` – show current time (`MM/DD/YYYY HH:MM:SS AM`).
- `rtc_set_datetime "MM/DD/YYYY" "HH:MM:SS AM"`
- `rtc_set_date MM/DD/YYYY`
- `rtc_set_time "HH:MM:SS AM"`

Commands are synchronous but fast; they call the `rtc_driver` helpers. Requires `console` and `rtc_driver`.
