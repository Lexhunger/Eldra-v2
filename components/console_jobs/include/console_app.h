#pragma once
// Simple console integration: USB-SERIAL-JTAG REPL with a basic "say" echo command.

#include "esp_err.h"

// Bring up a console REPL on USB-SERIAL-JTAG and register a "say" command.
esp_err_t Console_Init(void);
