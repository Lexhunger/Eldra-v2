#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    ELDRA_CMD_TYPE_FEED,
    ELDRA_CMD_TYPE_PET,
    ELDRA_CMD_TYPE_PLAY,
    ELDRA_CMD_TYPE_DEBUG_FORCE_STATE,
    ELDRA_CMD_TYPE_SET_FLAG,
    ELDRA_CMD_TYPE_RFID_ITEM,
    ELDRA_CMD_TYPE_SERVER_SCRIPTED_EVENT,
    ELDRA_CMD_TYPE_UNKNOWN
} eldra_command_type_t;

typedef struct {
    char id[64];                 // UUID from server
    eldra_command_type_t type;
    int arg0;
    int arg1;
} eldra_command_t;

typedef struct {
    const char *emotion_state;
    int happiness;
    int hunger;
    int energy;
    int social;
    int fear;
    int eldritch_charge;
    int battery_percentage;
    int battery_voltage_mv;
    bool battery_is_charging;
    bool flag_asleep;
    bool flag_low_power;
    bool flag_debug_mode;
} eldra_state_t;

typedef void (*eldra_command_handler_t)(const eldra_command_t *cmd);

/**
 * @brief Initialize Eldra cloud client and start background task.
 *
 * @param base_url   Base URL for Eldra Web UI (e.g. "http://sn-llm-core.local:8030").
 * @param auth_token Authorization bearer token (e.g. "the-old-ones").
 */
void eldra_cloud_init(const char *base_url, const char *auth_token);

/**
 * @brief Update network connectivity flag.
 *
 * @param online true when network is available, false when offline.
 */
void eldra_cloud_set_online(bool online);

/**
 * @brief Register a command handler callback.
 *
 * @param handler Callback invoked when a new command is fetched.
 */
void eldra_cloud_register_command_handler(eldra_command_handler_t handler);

/**
 * @brief Queue an acknowledgement for the last command.
 *
 * @param cmd     Command being acknowledged.
 * @param ok      true for OK, false for FAILED.
 * @param details Optional description string (may be NULL).
 */
void eldra_cloud_ack_command(const eldra_command_t *cmd, bool ok, const char *details);

/**
 * @brief Update cached state snapshot for periodic uploads.
 *
 * @param state Pointer to state structure to copy.
 */
void eldra_cloud_set_state(const eldra_state_t *state);

/**
 * @brief Log locally and enqueue a log line for later upload.
 *
 * @param level Log level string ("DEBUG", "INFO", "WARN", "ERROR").
 * @param tag   Log tag (optional, defaults to "eldra_cloud" when NULL or empty).
 * @param msg   Message string.
 */
void eldra_cloud_log(const char *level, const char *tag, const char *msg);

/**
 * @brief Perform a health check (GET /api/health) using current base_url/token.
 * @return true on success/200, false otherwise.
 */
bool eldra_cloud_health_check(void);

#ifdef __cplusplus
}
#endif
