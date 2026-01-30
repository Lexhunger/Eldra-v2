#include "eldra_cloud.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "wifi_driver.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define TAG "eldra_cloud"

#define ELDRA_CLOUD_TASK_DELAY_MS           1000
#define ELDRA_CLOUD_COMMAND_POLL_INTERVAL_MS 40000
#define ELDRA_CLOUD_STATE_PUSH_INTERVAL_MS   60000
#define ELDRA_CLOUD_LOG_FLUSH_INTERVAL_MS    30000
#define ELDRA_CLOUD_LOG_FLUSH_BATCH          16
#define ELDRA_CLOUD_HTTP_TIMEOUT_MS          1500
#define ELDRA_CLOUD_RESP_BUFFER_SIZE         2048
#define ELDRA_CLOUD_HEALTH_RETRY_MS          30000
#define ELDRA_CLOUD_HEALTH_GRACE_MS          5000
#define ELDRA_CLOUD_HEALTH_BACKOFF_MS        120000
#define ELDRA_CLOUD_HEALTH_FAIL_MAX          3

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
static int64_t s_health_elapsed = 0;
static int64_t s_health_grace = 0;
static int s_health_failures = 0;
static bool s_online_requested = false;
static bool s_health_ok = false;
static bool s_console_logs_pref = false;
static bool s_console_logs_enabled = false; // default off; enabled after health OK or explicit toggle
static int s_poll_interval_ms = ELDRA_CLOUD_COMMAND_POLL_INTERVAL_MS;
static int s_state_interval_ms = ELDRA_CLOUD_STATE_PUSH_INTERVAL_MS;
static int s_log_interval_ms = ELDRA_CLOUD_LOG_FLUSH_INTERVAL_MS;
static bool s_last_health_ok = false;
static int64_t s_last_health_time_ms = 0;
static bool s_last_state_ok = false;
static int64_t s_last_state_time_ms = 0;
static bool s_last_cmd_ok = false;
static int64_t s_last_cmd_time_ms = 0;

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
static void mark_offline_with_backoff(void);
static void log_ping_result(void);
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
    int64_t t_start = esp_timer_get_time();
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

    int64_t elapsed_ms = (esp_timer_get_time() - t_start) / 1000;

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP GET error: %s (elapsed=%lldms)", esp_err_to_name(err), (long long)elapsed_ms);
        return false;
    }
    if (status < 200 || status >= 300) {
        ESP_LOGW(TAG, "HTTP GET unexpected status: %d (elapsed=%lldms)", status, (long long)elapsed_ms);
        return false;
    }
    ESP_LOGI(TAG, "HTTP GET ok status=%d elapsed=%lldms", status, (long long)elapsed_ms);
    return true;
}

static bool http_post_json(const char *url, const char *body, char *resp, size_t resp_size)
{
    int64_t t_start = esp_timer_get_time();
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

    int64_t elapsed_ms = (esp_timer_get_time() - t_start) / 1000;

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP POST error: %s (elapsed=%lldms)", esp_err_to_name(err), (long long)elapsed_ms);
        return false;
    }
    if (status < 200 || status >= 300) {
        ESP_LOGW(TAG, "HTTP POST unexpected status: %d (elapsed=%lldms)", status, (long long)elapsed_ms);
        return false;
    }
    ESP_LOGI(TAG, "HTTP POST ok status=%d elapsed=%lldms", status, (long long)elapsed_ms);
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
        s_last_cmd_ok = true;
        s_last_cmd_time_ms = esp_timer_get_time() / 1000;
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
        s_last_cmd_ok = false;
        s_last_cmd_time_ms = esp_timer_get_time() / 1000;
        return false;
    }

    eldra_command_t cmd = {0};
    strncpy(cmd.id, id->valuestring, sizeof(cmd.id) - 1);
    cmd.type = map_command_type(type->valuestring);
    cmd.arg0 = cJSON_IsNumber(arg0) ? arg0->valueint : 0;
    cmd.arg1 = cJSON_IsNumber(arg1) ? arg1->valueint : 0;

    if (s_cmd_handler) {
        s_cmd_handler(&cmd);
        s_last_cmd_ok = true;
    } else {
        ESP_LOGW(TAG, "Command received but no handler registered");
        s_last_cmd_ok = false;
    }
    s_last_cmd_time_ms = esp_timer_get_time() / 1000;
    cJSON_Delete(root);
    return s_last_cmd_ok;
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
    s_last_state_ok = ok;
    s_last_state_time_ms = esp_timer_get_time() / 1000;
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
    ESP_LOGI(TAG, "Health check start url=%s", url);
    bool ok = http_get_json(url, resp, sizeof(resp));
    ESP_LOGI(TAG, "Health check %s", ok ? "OK" : "FAILED");
    if (resp[0]) {
        ESP_LOGD(TAG, "Health body: %s", resp);
    }
    if (ok) {
        s_health_ok = true;
        s_console_logs_enabled = s_console_logs_pref;
    }
    s_last_health_ok = ok;
    s_last_health_time_ms = esp_timer_get_time() / 1000;
    return ok;
}

static void mark_offline_with_backoff(void)
{
    if (s_mutex && xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        s_online = false;
        s_health_elapsed = 0;
        s_health_grace = ELDRA_CLOUD_HEALTH_BACKOFF_MS;
        if (s_health_failures < ELDRA_CLOUD_HEALTH_FAIL_MAX) {
            s_health_failures++;
        }
        xSemaphoreGive(s_mutex);
    }
    ESP_LOGW(TAG, "Cloud marked offline (failures=%d)", s_health_failures);
}

static void log_ping_result(void)
{
    // Extract host from base URL for a best-effort ICMP ping.
    const char *host = s_base_url;
    const char *p = strstr(s_base_url, "://");
    if (p) {
        host = p + 3;
    }
    char parsed[96] = {0};
    size_t i = 0;
    while (host[i] && host[i] != '/' && host[i] != ':' && i < sizeof(parsed) - 1) {
        parsed[i] = host[i];
        i++;
    }
    parsed[i] = '\0';
    if (parsed[0] == '\0') {
        ESP_LOGW(TAG, "Ping skipped (no host parsed)");
        return;
    }
    esp_err_t ping = wifi_driver_ping(parsed, 1, 1000);
    ESP_LOGW(TAG, "Ping %s -> %s", parsed, (ping == ESP_OK) ? "OK" : esp_err_to_name(ping));
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
    ESP_LOGI(TAG, "Cloud task loop started");

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(ELDRA_CLOUD_TASK_DELAY_MS));

        poll_elapsed += ELDRA_CLOUD_TASK_DELAY_MS;
        state_elapsed += ELDRA_CLOUD_TASK_DELAY_MS;
        log_elapsed += ELDRA_CLOUD_TASK_DELAY_MS;
        s_health_elapsed += ELDRA_CLOUD_TASK_DELAY_MS;

        if (!s_online) {
            if (!s_online_requested) {
                ESP_LOGD(TAG, "Cloud offline and not requested; skipping");
                continue;
            }
            if (s_health_grace > 0) {
                s_health_grace -= ELDRA_CLOUD_TASK_DELAY_MS;
                ESP_LOGD(TAG, "Cloud offline; grace %lldms remaining", (long long)s_health_grace);
                continue;
            }
            if (s_health_elapsed >= ELDRA_CLOUD_HEALTH_RETRY_MS && should_run_now()) {
                if (s_health_failures >= ELDRA_CLOUD_HEALTH_FAIL_MAX) {
                    ESP_LOGW(TAG, "Health retries exceeded; backoff");
                    mark_offline_with_backoff();
                    continue;
                }
                ESP_LOGI(TAG, "Cloud offline; retrying health");
                bool ok = check_health();
                if (ok && s_mutex && xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                    s_online = true;
                    s_health_failures = 0;
                    xSemaphoreGive(s_mutex);
                } else {
                    mark_offline_with_backoff();
                    if (s_health_failures >= ELDRA_CLOUD_HEALTH_FAIL_MAX) {
                        log_ping_result();
                    }
                }
                s_health_elapsed = 0;
            } else {
                ESP_LOGD(TAG, "Cloud offline; skipping cycle");
            }
            continue;
        }

        if (!should_run_now()) {
            ESP_LOGD(TAG, "Cloud skip: asleep/low_power");
            continue;
        }

        if (s_ack_pending) {
            ESP_LOGD(TAG, "Cloud sending pending ACK id=%s ok=%d", s_ack_id, s_ack_ok);
            send_pending_ack();
        }

        if (s_poll_interval_ms > 0 && poll_elapsed >= s_poll_interval_ms) {
            ESP_LOGD(TAG, "Cloud poll commands");
            fetch_and_dispatch_command();
            poll_elapsed = 0;
        }

        if (s_state_interval_ms > 0 && state_elapsed >= s_state_interval_ms) {
            ESP_LOGD(TAG, "Cloud push state");
            push_state_snapshot();
            state_elapsed = 0;
        }

        if (s_log_interval_ms > 0 && log_elapsed >= s_log_interval_ms) {
            ESP_LOGD(TAG, "Cloud flush logs");
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
    s_online_requested = false;
    s_health_ok = false;
    s_console_logs_enabled = false;
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
        BaseType_t res = xTaskCreate(eldra_cloud_task, "eldra_cloud", 6144, NULL, 3, &s_task_handle);
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
    if (s_mutex && xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        s_online = false;
        s_online_requested = online;
        s_health_elapsed = 0;
        s_health_failures = 0;
        s_health_grace = online ? ELDRA_CLOUD_HEALTH_GRACE_MS : 0;
        if (!s_health_ok && online && s_console_logs_pref) {
            s_console_logs_enabled = true; // honor preference early if requested
        }
        xSemaphoreGive(s_mutex);
    }
    ESP_LOGI(TAG, "Network %s (lazy health)", online ? "online" : "offline");
}

bool eldra_cloud_health_check(void)
{
    return check_health();
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

    if (s_console_logs_enabled) {
        if (str_ieq(lvl, "DEBUG")) {
            ESP_LOGD(log_tag, "%s", message);
        } else if (str_ieq(lvl, "WARN")) {
            ESP_LOGW(log_tag, "%s", message);
        } else if (str_ieq(lvl, "ERROR")) {
            ESP_LOGE(log_tag, "%s", message);
        } else {
            ESP_LOGI(log_tag, "%s", message);
        }
    }

    log_enqueue(lvl, log_tag, message);
}

void eldra_cloud_set_console_logging(bool enabled)
{
    s_console_logs_pref = enabled;
    if (s_health_ok || enabled) {
        s_console_logs_enabled = enabled;
    }
}

void eldra_cloud_set_intervals(int poll_interval_ms, int state_interval_ms, int log_interval_ms)
{
    if (poll_interval_ms > 0) {
        s_poll_interval_ms = poll_interval_ms;
    }
    if (state_interval_ms > 0) {
        s_state_interval_ms = state_interval_ms;
    }
    if (log_interval_ms > 0) {
        s_log_interval_ms = log_interval_ms;
    }
}

void eldra_cloud_get_status(eldra_cloud_status_t *out)
{
    if (!out) return;
    if (s_mutex && xSemaphoreTake(s_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        out->online_requested = s_online_requested;
        out->online = s_online;
        out->last_health_ok = s_last_health_ok;
        out->last_health_time_ms = s_last_health_time_ms;
        out->last_state_ok = s_last_state_ok;
        out->last_state_time_ms = s_last_state_time_ms;
        out->last_cmd_ok = s_last_cmd_ok;
        out->last_cmd_time_ms = s_last_cmd_time_ms;
        out->health_failures = s_health_failures;
        out->health_backoff_ms = s_health_grace > 0 ? s_health_grace : ELDRA_CLOUD_HEALTH_BACKOFF_MS;
        out->poll_interval_ms = s_poll_interval_ms;
        out->state_interval_ms = s_state_interval_ms;
        out->log_interval_ms = s_log_interval_ms;
        xSemaphoreGive(s_mutex);
    }
}
