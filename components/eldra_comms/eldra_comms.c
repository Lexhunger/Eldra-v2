#include "eldra_comms.h"

#include <string.h>

#include "eldra_logging.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

/**
 * @brief Simple ring buffer for incoming commands.
 */
#define COMMS_QUEUE_LENGTH 16U

typedef struct {
    struct {
        pet_command_t cmd;
        uint32_t enqueue_ms;
    } buffer[COMMS_QUEUE_LENGTH];
    size_t head;
    size_t tail;
    size_t count;
} command_queue_t;

static const char *TAG = "comms";
static command_queue_t g_queue;
static SemaphoreHandle_t g_queue_lock = NULL;
static comms_stats_t g_stats;
static uint64_t g_dequeue_latency_sum_ms = 0;
static uint32_t g_dequeue_latency_samples = 0;
static const TickType_t k_queue_lock_wait = pdMS_TO_TICKS(2);

static inline bool queue_lock_take(void)
{
    if (!g_queue_lock) {
        return false;
    }
    return (xSemaphoreTake(g_queue_lock, k_queue_lock_wait) == pdTRUE);
}

static inline void queue_lock_give(void)
{
    if (g_queue_lock) {
        xSemaphoreGive(g_queue_lock);
    }
}

static bool queue_full(const command_queue_t *queue) {
    return queue && queue->count >= COMMS_QUEUE_LENGTH;
}

static bool queue_empty(const command_queue_t *queue) {
    return (!queue) || (queue->count == 0U);
}

void comms_init(void) {
    if (g_queue_lock == NULL) {
        g_queue_lock = xSemaphoreCreateMutex();
    }
    memset(&g_queue, 0, sizeof(g_queue));
    memset(&g_stats, 0, sizeof(g_stats));
    g_dequeue_latency_sum_ms = 0;
    g_dequeue_latency_samples = 0;
}

bool comms_enqueue_command(const pet_command_t *cmd) {
    if (!cmd) {
        log_event(LOG_LEVEL_WARN, TAG, "Rejecting NULL command");
        return false;
    }

    if (!queue_lock_take()) {
        g_stats.enqueue_drop_lock++;
        log_event(LOG_LEVEL_WARN, TAG, "Command queue lock timeout, dropping type=%d", cmd->type);
        return false;
    }

    if (queue_full(&g_queue)) {
        g_stats.enqueue_drop_full++;
        queue_lock_give();
        log_event(LOG_LEVEL_WARN, TAG, "Command queue full, dropping type=%d", cmd->type);
        return false;
    }

    g_queue.buffer[g_queue.tail].cmd = *cmd;
    g_queue.buffer[g_queue.tail].enqueue_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
    g_queue.tail = (g_queue.tail + 1U) % COMMS_QUEUE_LENGTH;
    g_queue.count++;
    g_stats.enqueue_ok++;
    g_stats.queue_depth = (uint32_t)g_queue.count;
    if (g_stats.queue_depth > g_stats.queue_max_depth) {
        g_stats.queue_max_depth = g_stats.queue_depth;
    }
    queue_lock_give();
    return true;
}

bool comms_dequeue_command(pet_command_t *out_cmd) {
    if (!out_cmd) {
        return false;
    }

    if (!queue_lock_take()) {
        return false;
    }
    if (queue_empty(&g_queue)) {
        g_stats.queue_depth = (uint32_t)g_queue.count;
        queue_lock_give();
        return false;
    }

    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
    uint32_t enq_ms = g_queue.buffer[g_queue.head].enqueue_ms;
    uint32_t lat_ms = now_ms - enq_ms; // wrap-safe
    *out_cmd = g_queue.buffer[g_queue.head].cmd;
    g_queue.head = (g_queue.head + 1U) % COMMS_QUEUE_LENGTH;
    g_queue.count--;
    g_stats.dequeue_ok++;
    g_stats.dequeue_latency_last_ms = lat_ms;
    if (lat_ms > g_stats.dequeue_latency_max_ms) {
        g_stats.dequeue_latency_max_ms = lat_ms;
    }
    g_dequeue_latency_sum_ms += (uint64_t)lat_ms;
    g_dequeue_latency_samples++;
    if (g_dequeue_latency_samples > 0U) {
        g_stats.dequeue_latency_avg_ms = (uint32_t)(g_dequeue_latency_sum_ms / g_dequeue_latency_samples);
    }
    g_stats.queue_depth = (uint32_t)g_queue.count;
    queue_lock_give();
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
        case CMD_SHAKE:
            emotion_on_shake(emotion, (int)cmd->arg0, now_ms);
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

void comms_get_stats(comms_stats_t *out_stats)
{
    if (!out_stats) {
        return;
    }
    if (!queue_lock_take()) {
        memset(out_stats, 0, sizeof(*out_stats));
        return;
    }
    *out_stats = g_stats;
    out_stats->queue_depth = (uint32_t)g_queue.count;
    queue_lock_give();
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
