# Console + LVGL Quickstart (ESP32-S3-Touch-LCD-2.8C)

This project exposes a non-blocking console over the board’s USB-Serial/JTAG port, wires one example command (`say`), and renders text on the LCD via LVGL. The console runs in its own task so the main app keeps running.

## Hardware / Ports
- Board: ESP32-S3-Touch-LCD-2.8C (ST7701S RGB panel, TCA9554 IO expander, 8 MB PSRAM).
- USB-C ports:  
  - UART bridge (CH343): flash on COM9 (in this setup).  
  - USB-Serial/JTAG: console/monitor on COM10 (VID:PID 303A:1001).  
- LCD control wiring (fixed in code): reset = TCA9554 IO1, CS = TCA9554 IO3, backlight = ESP32 GPIO6.

## Software requirements
- IDF 5.5.x.
- Components: `console`, `lvgl`, `esp_timer`, `esp_lcd`, `driver` (declared in `main/CMakeLists.txt`).
- sdkconfig: console routed to USB-Serial/JTAG (checked in). UART console disabled.

## Build / Flash / Monitor
```bash
idf.py -p COM9 flash         # use the UART bridge to flash
idf.py -p COM10 monitor      # use the USB-Serial/JTAG port to interact
```
Use a terminal that supports ANSI escapes (idf.py monitor, minicom, screen). PuTTY works, but basic terminals may show “escape sequences not supported” and disable line editing/history.

## Current console behavior
- Runs `esp_console_new_repl_usb_serial_jtag`, starts a REPL task on its own stack.
- Registered commands:
  - `say <text>`: updates the LVGL label on screen.
  - `help`: built-in.
- LVGL render loop runs in `app_main` (calls `LVGL_ProcessTextQueue` + `lv_timer_handler`).
- Text updates from other tasks can call `LVGL_SendText` (queued to LVGL task).

## Non-blocking pattern (recommended)
Implemented:
1) Console commands are lightweight and enqueue jobs (`job_queue.c`).
2) A worker task drains the queue and executes actions (e.g., `LVGL_SendText`, `LCD_Clear`).
3) Add more job types/commands without blocking the REPL or main loop.

## Files of interest
- `main/main.c`: app entry; LCD init, LVGL init, console init, LVGL loop.
- `main/LCD_Driver/ST7701S.c/.h`: panel bring-up (RGB) and backlight.
- `main/LVGL_Driver/LVGL_Driver.c/.h`: LVGL glue, text queue, demo label.
- `main/console_app.c/.h`: console setup, `say` command.
- `main/job_queue.c/.h`: FreeRTOS queue + worker for non-blocking command handling.
- `main/EXIO/TCA9554PWR.c/.h`: IO expander; EXIO pins default to outputs driven low (silences buzzer on EXIO8).

## Porting checklist to another project
1) Copy `LVGL_Driver`, `console_app`, and add `REQUIRES console esp_timer esp_lcd driver lvgl` to your component.
2) Adjust pin macros in `ST7701S.h` to match your wiring (reset/CS via expander or GPIO).
3) Ensure sdkconfig routes console to your chosen interface (USB-Serial/JTAG recommended).
4) Provide a main loop that calls `LVGL_ProcessTextQueue()` and `lv_timer_handler()` periodically.
5) Keep/extend `job_queue` and add your own job types/commands; keep handlers non-blocking.

## Common pitfalls
- Port contention: only one monitor/terminal can hold the USB-Serial/JTAG port at a time.
- Terminal without ANSI: you’ll get “escape sequences not supported” and lose history/editing; switch to idf.py monitor/minicom/screen.
- Wrong port: console is on USB-Serial/JTAG (COM10 here), flashing usually on UART bridge (COM9 here).
