#pragma once
// Simple job queue scaffold to keep console handlers non-blocking.

#include "esp_err.h"
#include <stdint.h>

typedef enum {
    JOB_SAY,
    JOB_CLEAR,
} job_type_t;

typedef struct {
    job_type_t type;
    uint16_t color;   // Used by JOB_CLEAR (RGB565)
    char text[64];    // Used by JOB_SAY
} job_t;

esp_err_t job_queue_init(void);
esp_err_t job_queue_enqueue(const job_t *job);
