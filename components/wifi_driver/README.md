# Wi-Fi Driver (STA helper)

Purpose: keep Wi‑Fi logic independent and expose a simple API for scans, best-BSSID connect, roaming, status, and ping.

Features
- `wifi_driver_init_sta()` – init/start STA.
- `wifi_driver_scan_and_log()` – scan, dedupe SSIDs, log strongest RSSI.
- `wifi_driver_connect_best_async(ssid, pass)` – non-blocking connect to strongest BSSID for SSID; status via `wifi_driver_get_status`.
- `wifi_driver_set_roaming(bool)` – background task rescans every 30s and roams to stronger AP (>10 dB delta).
- `wifi_driver_ping(host, count, timeout_ms)` – raw ICMP ping (no esp_ping dependency).

Notes
- All worker tasks run on core1, priority 3 to keep console/UI responsive.
- Logs for Wi‑Fi/PHY are reduced to WARN inside `wifi_driver_init_sta` to avoid flooding the console.
