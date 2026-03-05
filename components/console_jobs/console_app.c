#include "console_app.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_check.h"
#include "esp_console.h"
#include "esp_log.h"
#include "linenoise/linenoise.h"
#include "eldra_sensors.h"
#include "eldra_logging.h"

static const char *TAG = "console";

static int cmd_say(int argc, char **argv)
{
    if (argc < 2) {
        printf("Usage: say <text>\n");
        return 0;
    }
    size_t len = 0;
    for (int i = 1; i < argc; i++) {
        len += strlen(argv[i]) + 1;
    }
    char *buf = malloc(len);
    if (!buf) {
        printf("Allocation failed\n");
        return 1;
    }
    buf[0] = '\0';
    for (int i = 1; i < argc; i++) {
        strcat(buf, argv[i]);
        if (i != argc - 1) {
            strcat(buf, " ");
        }
    }
    printf("%s\n", buf);
    free(buf);
    return 0;
}

static int cmd_battery(int argc, char **argv)
{
    (void)argc; (void)argv;
    float volts = eldra_sensors_get_battery_voltage();
    uint8_t pct = eldra_sensors_get_battery_percent();
    printf("Battery: %.2f V (%u%%)\n", volts, (unsigned)pct);
    return 0;
}

esp_err_t Console_Init(void)
{
    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t repl_cfg = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_cfg.prompt = "eldra> ";
    repl_cfg.max_history_len = 16;
    repl_cfg.task_stack_size = 4096;
    repl_cfg.task_priority = 4;
    repl_cfg.task_core_id = 0;

    esp_console_dev_usb_serial_jtag_config_t dev_cfg = ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();

    ESP_RETURN_ON_ERROR(esp_console_new_repl_usb_serial_jtag(&dev_cfg, &repl_cfg, &repl),
                        TAG, "create repl failed");

    // Notify logger about the prompt so it can reprint after async logs.
    log_set_prompt(repl_cfg.prompt);

    const esp_console_cmd_t say_cmd = {
        .command = "say",
        .help = "Echo text back on the console. Usage: say <text>",
        .hint = NULL,
        .func = &cmd_say,
    };
    ESP_RETURN_ON_ERROR(esp_console_cmd_register(&say_cmd), TAG, "register say failed");

    const esp_console_cmd_t batt_cmd = {
        .command = "battery",
        .help = "Show battery voltage and percentage.",
        .hint = NULL,
        .func = &cmd_battery,
    };
    ESP_RETURN_ON_ERROR(esp_console_cmd_register(&batt_cmd), TAG, "register battery failed");

    ESP_RETURN_ON_ERROR(esp_console_start_repl(repl), TAG, "start repl failed");
    EL_LOGI(TAG, "Console ready. Type 'help' for commands.");
    return ESP_OK;
}

