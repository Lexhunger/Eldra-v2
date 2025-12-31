#pragma once

#include "esp_err.h"
#include "eldra_emotion.h"

// Register console commands to trigger emotion actions (feed/pet/play/state).
// Optionally provide an emotion context for status reporting; may be NULL.
esp_err_t ConsoleEmotion_Init(emotion_context_t *ctx);
