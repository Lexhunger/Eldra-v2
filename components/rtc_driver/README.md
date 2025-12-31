# RTC Driver (software RTC)

Lightweight wrapper around `settimeofday`/`gettimeofday` to set/read system time in a friendly AM/PM format.

API
- `rtc_driver_set_datetime("MM/DD/YYYY HH:MM:SS AM")`
- `rtc_driver_set_date("MM/DD/YYYY")`
- `rtc_driver_set_time("HH:MM:SS AM")`
- `rtc_driver_format_now(buf, len)` -> `MM/DD/YYYY HH:MM:SS AM`
- `rtc_driver_get_time(struct tm*)`

Notes
- Uses internal software RTC; loses time across power cycles unless SNTP or an external RTC is used to restore it.
- Strict parsing; keep quoted strings when containing spaces.
