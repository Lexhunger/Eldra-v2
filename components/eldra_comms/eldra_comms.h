#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "eldra_emotion.h"

/**
 * @file eldra_comms.h
 * @brief Command backbone that normalizes BLE/IR/RFID/Wi-Fi inputs into a shared queue.
 */

/**
 * @brief Command categories that can be enqueued for the Emotion Engine.
 */
typedef enum {
    CMD_FEED = 0,
    CMD_PET,
    CMD_PLAY,
    CMD_DEBUG_FORCE_STATE,
    CMD_SET_FLAG,
    CMD_RESERVED_RFID,
    CMD_RESERVED_SERVER,
} pet_command_type_t;

/**
 * @brief Generic command envelope shared by all comms sources.
 */
typedef struct {
    pet_command_type_t type;
    uint32_t arg0;
    uint32_t arg1;
} pet_command_t;

/**
 * @brief Initialize the comms queue and reset producer stubs.
 */
void comms_init(void);

/**
 * @brief Enqueue a command from any producer (BLE/IR/RFID/Wi-Fi).
 * @param cmd Command to enqueue.
 * @return True if the command was accepted; false if the queue is full or input invalid.
 */
bool comms_enqueue_command(const pet_command_t *cmd);

/**
 * @brief Dequeue the next pending command.
 * @param out_cmd Destination for the command.
 * @return True if a command was returned; false if the queue is empty or output null.
 */
bool comms_dequeue_command(pet_command_t *out_cmd);

/**
 * @brief Drain all pending commands and dispatch them into the Emotion Engine.
 * @param emotion Emotion context to pass to handlers.
 * @param now_ms Current monotonic time in milliseconds.
 */
void comms_process_all_pending(emotion_context_t *emotion, uint32_t now_ms);

/**
 * @brief Stub: BLE packet handler should parse data and enqueue commands.
 */
void comms_on_ble_packet(const uint8_t *data, size_t len);

/**
 * @brief Stub: IR receiver handler should map codes to commands.
 */
void comms_on_ir_code(uint32_t code);

/**
 * @brief Stub: Future RFID packet handler.
 */
void comms_on_rfid_tag(uint32_t tag_id);

/**
 * @brief Stub: Wi-Fi server handler for pushed commands or polled queue.
 */
void comms_on_server_command(const pet_command_t *cmd);
