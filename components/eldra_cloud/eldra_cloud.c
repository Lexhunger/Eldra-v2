#include "eldra_cloud.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define TAG "eldra_cloud"

#define ELDRA_CLOUD_TASK_DELAY_MS           1000
#define ELDRA_CLOUD_COMMAND_POLL_INTERVAL_MS 40000
#define ELDRA_CLOUD_STATE_PUSH_INTERVAL_MS   60000
#define ELDRA_CLOUD_LOG_FLUSH_INTERVAL_MS    30000
#define ELDRA_CLOUD_LOG_FLUSH_BATCH          16
#define ELDRA_CLOUD_HTTP_TIMEOUT_MS          5000
#define ELDRA_CLOUD_RESP_BUFFER_SIZE         2048

#define ELDRA_CLOUD_LOG_BUFFER_SIZE 64
#define ELDRA_CLOUD_HEALTH_URL      "/api/health"
#define ELDRA_CLOUD_CMD_NEXT_URL    "/api/command/next"
#define ELDRA_CLOUD_CMD_ACK_URL     "/api/command/ack"
#define ELDRA_CLOUD_STATE_URL       "/api/state"
#define ELDRA_CLOUD_LOGS_URL        "/api/logs/upload/json"

typedef struct {
    char ts[32];
    char level[8];
    char tag[16];
    char msg[80];
} eldra_cloud_log_entry_t;

typedef struct {
    char *buf;
    size_t len;
    size_t cap;
} http_buffer_t;

static char s_base_url[128];
static char s_auth_token[64];
static bool s_online = false;
static bool s_initialized = false;

static eldra_command_handler_t s_cmd_handler = NULL;

static eldra_state_t s_state_cache = {0};
static char s_state_emotion[32];
static bool s_state_valid = false;

static bool s_ack_pending = false;
static char s_ack_id[64];
static bool s_ack_ok = false;
static char s_ack_details[128];

static eldra_cloud_log_entry_t s_log_buffer[ELDRA_CLOUD_LOG_BUFFER_SIZE];
static size_t s_log_head = 0;
static size_t s_log_count = 0;

static TaskHandle_t s_task_handle = NULL;
static SemaphoreHandle_t s_mutex = NULL;

static bool str_ieq(const char *a, const char *b);
static eldra_command_type_t map_command_type(const char *type_str);
static esp_err_t http_event_handler(esp_http_client_event_t *evt);
static void set_common_headers(esp_http_client_handle_t client, bool json, bool accept_json);
static bool http_get_json(const char *url, char *resp, size_t resp_size);
static bool http_post_json(const char *url, const char *body, char *resp, size_t resp_size);
static bool fetch_and_dispatch_command(void);
static bool send_pending_ack(void);
static bool push_state_snapshot(void);
static bool flush_logs(void);
static void log_enqueue(const char *level, const char *tag, const char *msg);
static void eldra_cloud_task(void *arg);
static bool should_run_now(void);
static bool check_health(void);

static bool str_ieq(const char *a, const char *b)
{
    if (!a || !b) {
        return false;
    }
    while (*a && *b) {
        if (toupper((unsigned char)*a) != toupper((unsigned char)*b)) {
            return false;
        }
        ++a;
        ++b;
    }
    return (*a == '\0') && (*b == '\0');
}

static const char *normalize_level(const char *level)
{
    if (str_ieq(level, "DEBUG")) {
        return "DEBUG";
    }
    if (str_ieq(level, "WARN") || str_ieq(level, "WARNING")) {
        return "WARN";
    }
    if (str_ieq(level, "ERROR")) {
        return "ERROR";
    }
    return "INFO";
}

static eldra_command_type_t map_command_type(const char *type_str)
{
    if (str_ieq(type_str, "FEED")) {
        return ELDRA_CMD_TYPE_FEED;
    }
    if (str_ieq(type_str, "PET")) {
        return ELDRA_CMD_TYPE_PET;
    }
    if (str_ieq(type_str, "PLAY")) {
        return ELDRA_CMD_TYPE_PLAY;
    }
    if (str_ieq(type_str, "DEBUG_FORCE_STATE")) {
        return ELDRA_CMD_TYPE_DEBUG_FORCE_STATE;
    }
    if (str_ieq(type_str, "SET_FLAG")) {
        return ELDRA_CMD_TYPE_SET_FLAG;
    }
    if (str_ieq(type_str, "RFID_ITEM")) {
        return ELDRA_CMD_TYPE_RFID_ITEM;
    }
    if (str_ieq(type_str, "SERVER_SCRIPTED_EVENT")) {
        return ELDRA_CMD_TYPE_SERVER_SCRIPTED_EVENT;
    }
    return ELDRA_CMD_TYPE_UNKNOWN;
}

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    http_buffer_t *buf = (http_buffer_t *)evt->user_data;
    switch (evt->event_id) {
        case HTTP_EVENT_ON_DATA:
            if (buf && buf->buf && buf->cap > 1) {
                size_t available = (buf->cap > buf->len + 1) ? buf->cap - buf->len - 1 : 0;
                size_t to_copy = (evt->data_len < available) ? evt->data_len : available;
                if (to_copy > 0) {
                    memcpy(buf->buf + buf->len, evt->data, to_copy);
                    buf->len += to_copy;
                    buf->buf[buf->len] = '\0';
                }
            }
            break;
        case HTTP_EVENT_ON_FINISH:
            if (buf && buf->buf && buf->cap > 0) {
                if (buf->len >= buf->cap) {
                    buf->len = buf->cap - 1;
                }
                buf->buf[buf->len] = '\0';
            }
            break;
        default:
            break;
    }
    return ESP_OK;
}

static void set_common_headers(esp_http_client_handle_t client, bool json, bool accept_json)
{
    char auth_header[96];
    snprintf(auth_header, sizeof(auth_header), "Bearer %s", s_auth_token);
    esp_http_client_set_header(client, "Authorization", auth_header);
    if (json) {
        esp_http_client_set_header(client, "Content-Type", "application/json");
    }
    if (accept_json) {
        esp_http_client_set_header(client, "Accept", "application/json");
    }
}

static bool http_get_json(const char *url, char *resp, size_t resp_size)
{
    http_buffer_t buffer = {
        .buf = resp,
        .len = 0,
        .cap = resp_size,
    };
    if (resp && resp_size > 0) {
        resp[0] = '\0';
    }

    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_GET,
        .timeout_ms = ELDRA_CLOUD_HTTP_TIMEOUT_MS,
        .event_handler = http_event_handler,
        .user_data = resp ? &buffer : NULL,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        ESP_LOGE(TAG, "Failed to init HTTP client for GET");
        return false;
    }

    set_common_headers(client, false, true);

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP GET error: %s", esp_err_to_name(err));
        return false;
    }
    if (status < 200 || status >= 300) {
        ESP_LOGW(TAG, "HTTP GET unexpected status: %d", status);
        return false;
    }
    return true;
}

static bool http_post_json(const char *url, const char *body, char *resp, size_t resp_size)
{
    http_buffer_t buffer = {
        .buf = resp,
        .len = 0,
        .cap = resp_size,
    };
    if (resp && resp_size > 0) {
        resp[0] = '\0';
    }

    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = ELDRA_CLOUD_HTTP_TIMEOUT_MS,
        .event_handler = http_event_handler,
        .user_data = resp ? &buffer : NULL,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        ESP_LOGE(TAG, "Failed to init HTTP client for POST");
        return false;
    }

    set_common_headers(client, true, true);
    if (body) {
        esp_http_client_set_post_field(client, body, strlen(body));
    }

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP POST error: %s", esp_err_to_name(err));
        return false;
    }
    if (status < 200 || status >= 300) {
        ESP_LOGW(TAG, "HTTP POST unexpected status: %d", status);
        return false;
    }
    return true;
}

static bool fetch_and_dispatch_command(void)
{
    if (!s_initialized || s_base_url[0] == '\0') {
        return false;
    }

    char url[192];
    snprintf(url, sizeof(url), "%s%s", s_base_url, ELDRA_CLOUD_CMD_NEXT_URL);

    char resp[ELDRA_CLOUD_RESP_BUFFER_SIZE];
    if (!http_get_json(url, resp, sizeof(resp))) {
        return false;
    }

    cJSON *root = cJSON_Parse(resp);
    if (!root) {
        ESP_LOGW(TAG, "Failed to parse command response");
        return false;
    }

    cJSON *cmd_obj = cJSON_GetObjectItemCaseSensitive(root, "command");
    if (!cmd_obj || cJSON_IsNull(cmd_obj)) {
        ESP_LOGD(TAG, "No command available");
        cJSON_Delete(root);
        return true;
    }

    cJSON *id = cJSON_GetObjectItemCaseSensitive(cmd_obj, "id");
    cJSON *type = cJSON_GetObjectItemCaseSensitive(cmd_obj, "type");
    cJSON *arg0 = cJSON_GetObjectItemCaseSensitive(cmd_obj, "arg0");
    cJSON *arg1 = cJSON_GetObjectItemCaseSensitive(cmd_obj, "arg1");

    if (!cJSON_IsString(id) || !cJSON_IsString(type)) {
        ESP_LOGW(TAG, "Command missing id or type");
        cJSON_Delete(root);
        return false;
    }

    eldra_command_t cmd = {0};
    strncpy(cmd.id, id->valuestring, sizeof(cmd.id) - 1);
    cmd.type = map_command_type(type->valuestring);
    cmd.arg0 = cJSON_IsNumber(arg0) ? arg0->valueint : 0;
    cmd.arg1 = cJSON_IsNumber(arg1) ? arg1->valueint : 0;

    if (s_cmd_handler) {
        s_cmd_handler(&cmd);
    } else {
        ESP_LOGW(TAG, "Command received but no handler registered");
    }

    cJSON_Delete(root);
    return true;
}

static bool send_pending_ack(void)
{
    char id_copy[sizeof(s_ack_id)] = {0};
    char details_copy[sizeof(s_ack_details)] = {0};
    bool ack_ok = false;
    bool has_pending = false;

    if (s_mutex && xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        if (s_ack_pending) {
            strncpy(id_copy, s_ack_id, sizeof(id_copy) - 1);
            strncpy(details_copy, s_ack_details, sizeof(details_copy) - 1);
            ack_ok = s_ack_ok;
            has_pending = true;
        }
        xSemaphoreGive(s_mutex);
    }

    if (!has_pending || s_base_url[0] == '\0') {
        return false;
    }

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return false;
    }

    cJSON_AddStringToObject(root, "id", id_copy);
    cJSON_AddStringToObject(root, "status", ack_ok ? "OK" : "FAILED");
    if (strlen(details_copy) > 0) {
        cJSON_AddStringToObject(root, "details", details_copy);
    }

    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!body) {
        return false;
    }

    char url[192];
    snprintf(url, sizeof(url), "%s%s", s_base_url, ELDRA_CLOUD_CMD_ACK_URL);

    bool ok = http_post_json(url, body, NULL, 0);
    free(body);

    if (ok && s_mutex && xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        s_ack_pending = false;
        s_ack_id[0] = '\0';
        s_ack_details[0] = '\0';
        xSemaphoreGive(s_mutex);
    }

    return ok;
}

static bool push_state_snapshot(void)
{
    eldra_state_t state = {0};
    char emotion_copy[sizeof(s_state_emotion)];
    bool has_state = false;

    if (s_mutex && xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        if (s_state_valid) {
            state = s_state_cache;
            strncpy(emotion_copy, s_state_emotion, sizeof(emotion_copy) - 1);
            emotion_copy[sizeof(emotion_copy) - 1] = '\0';
            state.emotion_state = emotion_copy;
            has_state = true;
        }
        xSemaphoreGive(s_mutex);
    }

    if (!has_state) {
        return false;
    }

    if (s_base_url[0] == '\0') {
        return false;
    }

    cJSON *root = cJSON_CreateObject();
    cJSON *meters = cJSON_CreateObject();
    cJSON *battery = cJSON_CreateObject();
    cJSON *flags = cJSON_CreateObject();
    if (!root || !meters || !battery || !flags) {
        cJSON_Delete(root);
        cJSON_Delete(meters);
        cJSON_Delete(battery);
        cJSON_Delete(flags);
        return false;
    }

    cJSON_AddStringToObject(root, "emotion_state", state.emotion_state ? state.emotion_state : "IDLE");

    cJSON_AddNumberToObject(meters, "happiness", state.happiness);
    cJSON_AddNumberToObject(meters, "hunger", state.hunger);
    cJSON_AddNumberToObject(meters, "energy", state.energy);
    cJSON_AddNumberToObject(meters, "social", state.social);
    cJSON_AddNumberToObject(meters, "fear", state.fear);
    cJSON_AddNumberToObject(meters, "eldritch_charge", state.eldritch_charge);
    cJSON_AddItemToObject(root, "meters", meters);

    cJSON_AddNumberToObject(battery, "percentage", state.battery_percentage);
    cJSON_AddNumberToObject(battery, "voltage_mv", state.battery_voltage_mv);
    cJSON_AddBoolToObject(battery, "is_charging", state.battery_is_charging);
    cJSON_AddItemToObject(root, "battery", battery);

    cJSON_AddBoolToObject(flags, "asleep", state.flag_asleep);
    cJSON_AddBoolToObject(flags, "low_power", state.flag_low_power);
    cJSON_AddBoolToObject(flags, "debug_mode", state.flag_debug_mode);
    cJSON_AddItemToObject(root, "flags", flags);

    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!body) {
        return false;
    }

    char url[192];
    snprintf(url, sizeof(url), "%s%s", s_base_url, ELDRA_CLOUD_STATE_URL);

    bool ok = http_post_json(url, body, NULL, 0);
    free(body);
    return ok;
}

static bool flush_logs(void)
{
    eldra_cloud_log_entry_t entries[ELDRA_CLOUD_LOG_FLUSH_BATCH];
    size_t to_send = 0;

    if (s_base_url[0] == '\0') {
        return false;
    }

    if (s_mutex && xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        if (s_log_count > 0) {
            to_send = (s_log_count < ELDRA_CLOUD_LOG_FLUSH_BATCH) ? s_log_count : ELDRA_CLOUD_LOG_FLUSH_BATCH;
            size_t tail = (s_log_head + ELDRA_CLOUD_LOG_BUFFER_SIZE - s_log_count) % ELDRA_CLOUD_LOG_BUFFER_SIZE;
            for (size_t i = 0; i < to_send; ++i) {
                size_t idx = (tail + i) % ELDRA_CLOUD_LOG_BUFFER_SIZE;
                entries[i] = s_log_buffer[idx];
            }
        }
        xSemaphoreGive(s_mutex);
    }

    if (to_send == 0) {
        return false;
    }

    cJSON *root = cJSON_CreateObject();
    cJSON *lines = cJSON_CreateArray();
    if (!root || !lines) {
        cJSON_Delete(root);
        cJSON_Delete(lines);
        return false;
    }

    for (size_t i = 0; i < to_send; ++i) {
        cJSON *line = cJSON_CreateObject();
        if (!line) {
            continue;
        }
        cJSON_AddStringToObject(line, "ts", entries[i].ts);
        cJSON_AddStringToObject(line, "level", entries[i].level);
        cJSON_AddStringToObject(line, "tag", entries[i].tag);
        cJSON_AddStringToObject(line, "msg", entries[i].msg);
        cJSON_AddItemToArray(lines, line);
    }
    cJSON_AddItemToObject(root, "lines", lines);

    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!body) {
        return false;
    }

    char url[192];
    snprintf(url, sizeof(url), "%s%s", s_base_url, ELDRA_CLOUD_LOGS_URL);

    bool ok = http_post_json(url, body, NULL, 0);
    free(body);

    if (ok && s_mutex && xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        if (s_log_count >= to_send) {
            s_log_count -= to_send;
        } else {
            s_log_count = 0;
        }
        xSemaphoreGive(s_mutex);
    }

    return ok;
}

static bool should_run_now(void)
{
    if (s_base_url[0] == '\0' || s_auth_token[0] == '\0') {
        return false;
    }
    bool asleep = false;
    bool low_power = false;
    if (s_mutex && xSemaphoreTake(s_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        asleep = s_state_valid && s_state_cache.flag_asleep;
        low_power = s_state_valid && s_state_cache.flag_low_power;
        xSemaphoreGive(s_mutex);
    }
    return !(asleep || low_power);
}

static bool check_health(void)
{
    if (s_base_url[0] == '\0') {
        return false;
    }
    char url[192];
    snprintf(url, sizeof(url), "%s%s", s_base_url, ELDRA_CLOUD_HEALTH_URL);
    char resp[ELDRA_CLOUD_RESP_BUFFER_SIZE];
    bool ok = http_get_json(url, resp, sizeof(resp));
    if (ok) {
        ESP_LOGI(TAG, "Health OK: %s", resp[0] ? resp : "{}");
    } else {
        ESP_LOGW(TAG, "Health check failed");
    }
    return ok;
}

static void log_enqueue(const char *level, const char *tag, const char *msg)
{
    if (!s_mutex) {
        return;
    }

    eldra_cloud_log_entry_t entry = {0};
    strncpy(entry.ts, "1970-01-01T00:00:00Z", sizeof(entry.ts) - 1);
    strncpy(entry.level, level, sizeof(entry.level) - 1);
    strncpy(entry.tag, tag, sizeof(entry.tag) - 1);
    strncpy(entry.msg, msg, sizeof(entry.msg) - 1);

    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        s_log_buffer[s_log_head] = entry;
        s_log_head = (s_log_head + 1) % ELDRA_CLOUD_LOG_BUFFER_SIZE;
        if (s_log_count < ELDRA_CLOUD_LOG_BUFFER_SIZE) {
            s_log_count++;
        }
        xSemaphoreGive(s_mutex);
    }
}

static void eldra_cloud_task(void *arg)
{
    int64_t poll_elapsed = 0;
    int64_t state_elapsed = 0;
    int64_t log_elapsed = 0;

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(ELDRA_CLOUD_TASK_DELAY_MS));

        poll_elapsed += ELDRA_CLOUD_TASK_DELAY_MS;
        state_elapsed += ELDRA_CLOUD_TASK_DELAY_MS;
        log_elapsed += ELDRA_CLOUD_TASK_DELAY_MS;

        if (!s_online) {
            continue;
        }

        if (!should_run_now()) {
            continue;
        }

        if (s_ack_pending) {
            send_pending_ack();
        }

        if (poll_elapsed >= ELDRA_CLOUD_COMMAND_POLL_INTERVAL_MS) {
            fetch_and_dispatch_command();
            poll_elapsed = 0;
        }

        if (state_elapsed >= ELDRA_CLOUD_STATE_PUSH_INTERVAL_MS) {
            push_state_snapshot();
            state_elapsed = 0;
        }

        if (log_elapsed >= ELDRA_CLOUD_LOG_FLUSH_INTERVAL_MS) {
            flush_logs();
            log_elapsed = 0;
        }
    }
}

void eldra_cloud_init(const char *base_url, const char *auth_token)
{
    snprintf(s_base_url, sizeof(s_base_url), "%s", base_url ? base_url : "");
    snprintf(s_auth_token, sizeof(s_auth_token), "%s", auth_token ? auth_token : "");

    s_online = false;
    s_cmd_handler = NULL;
    s_state_valid = false;
    s_ack_pending = false;
    s_log_head = 0;
    s_log_count = 0;
    memset(s_state_emotion, 0, sizeof(s_state_emotion));

    if (!s_mutex) {
        s_mutex = xSemaphoreCreateMutex();
    }

    if (!s_task_handle) {
        BaseType_t res = xTaskCreate(eldra_cloud_task, "eldra_cloud", 6144, NULL, 5, &s_task_handle);
        if (res != pdPASS) {
            ESP_LOGE(TAG, "Failed to create cloud task");
        }
    }

    s_initialized = true;
    ESP_LOGI(TAG, "eldra_cloud initialized");
}

void eldra_cloud_set_online(bool online)
{
    if (!s_initialized) {
        return;
    }
    bool ok = online;
    if (online) {
        if (s_base_url[0] == '\0' || s_auth_token[0] == '\0') {
            ESP_LOGW(TAG, "Cloud online requested but base_url/token missing");
            ok = false;
        } else {
            ok = check_health();
        }
    }
    if (s_mutex && xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        s_online = ok;
        xSemaphoreGive(s_mutex);
    }
    ESP_LOGI(TAG, "Network %s", ok ? "online" : "offline");
}

void eldra_cloud_register_command_handler(eldra_command_handler_t handler)
{
    if (!s_initialized) {
        return;
    }
    s_cmd_handler = handler;
}

void eldra_cloud_ack_command(const eldra_command_t *cmd, bool ok, const char *details)
{
    if (!s_initialized || !cmd) {
        return;
    }

    if (s_mutex && xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        strncpy(s_ack_id, cmd->id, sizeof(s_ack_id) - 1);
        s_ack_id[sizeof(s_ack_id) - 1] = '\0';
        s_ack_ok = ok;
        if (details) {
            strncpy(s_ack_details, details, sizeof(s_ack_details) - 1);
            s_ack_details[sizeof(s_ack_details) - 1] = '\0';
        } else {
            s_ack_details[0] = '\0';
        }
        s_ack_pending = true;
        xSemaphoreGive(s_mutex);
    }
}

void eldra_cloud_set_state(const eldra_state_t *state)
{
    if (!s_initialized || !state) {
        return;
    }
    if (s_mutex && xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        if (state->emotion_state) {
            strncpy(s_state_emotion, state->emotion_state, sizeof(s_state_emotion) - 1);
            s_state_emotion[sizeof(s_state_emotion) - 1] = '\0';
        } else {
            s_state_emotion[0] = '\0';
        }

        s_state_cache.emotion_state = s_state_emotion;
        s_state_cache.happiness = state->happiness;
        s_state_cache.hunger = state->hunger;
        s_state_cache.energy = state->energy;
        s_state_cache.social = state->social;
        s_state_cache.fear = state->fear;
        s_state_cache.eldritch_charge = state->eldritch_charge;
        s_state_cache.battery_percentage = state->battery_percentage;
        s_state_cache.battery_voltage_mv = state->battery_voltage_mv;
        s_state_cache.battery_is_charging = state->battery_is_charging;
        s_state_cache.flag_asleep = state->flag_asleep;
        s_state_cache.flag_low_power = state->flag_low_power;
        s_state_cache.flag_debug_mode = state->flag_debug_mode;
        s_state_valid = true;
        xSemaphoreGive(s_mutex);
    }
}

void eldra_cloud_log(const char *level, const char *tag, const char *msg)
{
    const char *lvl = normalize_level(level);
    const char *log_tag = (tag && tag[0]) ? tag : TAG;
    const char *message = msg ? msg : "";

    if (str_ieq(lvl, "DEBUG")) {
        ESP_LOGD(log_tag, "%s", message);
    } else if (str_ieq(lvl, "WARN")) {
        ESP_LOGW(log_tag, "%s", message);
    } else if (str_ieq(lvl, "ERROR")) {
        ESP_LOGE(log_tag, "%s", message);
    } else {
        ESP_LOGI(log_tag, "%s", message);
    }

    log_enqueue(lvl, log_tag, message);
}
