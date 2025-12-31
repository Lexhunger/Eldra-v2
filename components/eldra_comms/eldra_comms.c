#include "eldra_comms.h"

#include <string.h>

#include "eldra_logging.h"

/**
 * @brief Simple ring buffer for incoming commands.
 */
#define COMMS_QUEUE_LENGTH 16U

typedef struct {
    pet_command_t buffer[COMMS_QUEUE_LENGTH];
    size_t head;
    size_t tail;
    size_t count;
} command_queue_t;

static const char *TAG = "comms";
static command_queue_t g_queue;

static bool queue_full(const command_queue_t *queue) {
    return queue && queue->count >= COMMS_QUEUE_LENGTH;
}

static bool queue_empty(const command_queue_t *queue) {
    return (!queue) || (queue->count == 0U);
}

void comms_init(void) {
    memset(&g_queue, 0, sizeof(g_queue));
}

bool comms_enqueue_command(const pet_command_t *cmd) {
    if (!cmd) {
        log_event(LOG_LEVEL_WARN, TAG, "Rejecting NULL command");
        return false;
    }
    if (queue_full(&g_queue)) {
        log_event(LOG_LEVEL_WARN, TAG, "Command queue full, dropping type=%d", cmd->type);
        return false;
    }
    g_queue.buffer[g_queue.tail] = *cmd;
    g_queue.tail = (g_queue.tail + 1U) % COMMS_QUEUE_LENGTH;
    g_queue.count++;
    return true;
}

bool comms_dequeue_command(pet_command_t *out_cmd) {
    if (!out_cmd || queue_empty(&g_queue)) {
        return false;
    }
    *out_cmd = g_queue.buffer[g_queue.head];
    g_queue.head = (g_queue.head + 1U) % COMMS_QUEUE_LENGTH;
    g_queue.count--;
    return true;
}

static void handle_command(emotion_context_t *emotion, const pet_command_t *cmd, uint32_t now_ms) {
    if (!cmd) {
        return;
    }
    if (!emotion) {
        log_event(LOG_LEVEL_WARN, TAG, "No emotion context; cannot handle cmd=%d", cmd->type);
        return;
    }

    switch (cmd->type) {
        case CMD_FEED:
        emotion_on_feed(emotion, cmd->arg0, now_ms);
        break;
    case CMD_PET:
        emotion_on_pet(emotion, 0, now_ms);
        break;
        case CMD_PLAY:
            emotion_on_play(emotion, now_ms);
            break;
        case CMD_DEBUG_FORCE_STATE: {
            emotion_state_t forced = (emotion_state_t)cmd->arg0;
            emotion_state_t prev = emotion->current_state;
            emotion->previous_state = prev;
            emotion->current_state = forced;
            emotion->last_state_change_ms = now_ms;
            emotion_notify_state_change(prev, forced);
            break;
        }
        case CMD_SET_FLAG:
            // Flags will be defined in a later phase (e.g., debug toggles, behavior masks).
            log_event(LOG_LEVEL_INFO, TAG, "CMD_SET_FLAG id=%lu value=%lu", (unsigned long)cmd->arg0,
                      (unsigned long)cmd->arg1);
            break;
        case CMD_RESERVED_RFID:
        case CMD_RESERVED_SERVER:
        default:
            // Reserved for future command types (RFID tag actions or server-scripted events).
            log_event(LOG_LEVEL_DEBUG, TAG, "Reserved command type=%d ignored", cmd->type);
            break;
    }
}

void comms_process_all_pending(emotion_context_t *emotion, uint32_t now_ms) {
    pet_command_t cmd;
    while (comms_dequeue_command(&cmd)) {
        handle_command(emotion, &cmd, now_ms);
    }
}

void comms_on_ble_packet(const uint8_t *data, size_t len) {
    (void)data;
    (void)len;
    // TODO: Parse BLE payload into pet_command_t instances and enqueue them.
    log_event(LOG_LEVEL_DEBUG, TAG, "BLE packet stub len=%lu", (unsigned long)len);
}

void comms_on_ir_code(uint32_t code) {
    // TODO: Map IR codes to commands (e.g., button presses, feed/play/pet).
    log_event(LOG_LEVEL_DEBUG, TAG, "IR code stub code=0x%lx", (unsigned long)code);
}

void comms_on_rfid_tag(uint32_t tag_id) {
    // TODO: Map RFID tag IDs to feed/play/debug commands.
    log_event(LOG_LEVEL_DEBUG, TAG, "RFID tag stub id=%lu", (unsigned long)tag_id);
}

void comms_on_server_command(const pet_command_t *cmd) {
    // TODO: Server polling will validate and enqueue commands here.
    comms_enqueue_command(cmd);
}
