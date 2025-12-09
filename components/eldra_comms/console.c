#include "eldra_comms.h"

#include <string.h>

#include "driver/uart.h"
#include "eldra_logging.h"

/**
 * @file console.c
 * @brief UART0 console task that feeds lines into the shared command router.
 */

#define CONSOLE_UART UART_NUM_0
#define CONSOLE_RX_BUF 256
#define CONSOLE_TASK_STACK 4096
#define CONSOLE_TASK_PRIO 4

static const char *TAG = "console";

static void console_task(void *param) {
    (void)param;
    char buf[CONSOLE_RX_BUF] = {0};
    size_t pos = 0;
    while (1) {
        uint8_t ch;
        int len = uart_read_bytes(CONSOLE_UART, &ch, 1, pdMS_TO_TICKS(50));
        if (len > 0) {
            if (ch == '\n' || ch == '\r') {
                buf[pos] = '\0';
                comms_commands_process_line(buf);
                pos = 0;
                buf[0] = '\0';
            } else if (pos < sizeof(buf) - 1) {
                buf[pos++] = (char)ch;
            }
        }
    }
}

void comms_console_start(void) {
    // Install UART driver if not already present.
    uart_driver_install(CONSOLE_UART, CONSOLE_RX_BUF, 0, 0, NULL, 0);
    xTaskCreatePinnedToCore(console_task, "console_task", CONSOLE_TASK_STACK, NULL, CONSOLE_TASK_PRIO, NULL, 0);
    log_event(LOG_LEVEL_INFO, TAG,
              "Console ready. Commands: LOGS [tag] [level] [limit], SETTIME <epoch>, BAT, FEED, PET, PLAY, FORCESTATE, STATE");
}
