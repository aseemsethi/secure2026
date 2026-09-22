#include "ble_sensors.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "host/ble_gap.h"
#include "host/ble_hs.h"
#include "host/ble_hs_adv.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"

#include "http_server.h"

/* Provided by main.c, declared here as http_server.c already does for displayString. */
void displayString(char *str);
esp_err_t send_ntfy_notification(const char *topic, const char *message);

static const char *TAG = "BLE_SENSOR";

#define BLE_SCAN_INTERVAL_MS 160
#define BLE_SCAN_WINDOW_MS 80
#define BLE_EVENT_QUEUE_DEPTH 8

/* A sensor bouncing in its holder can rattle; ignore flips closer than this. */
#define BLE_STATE_DEBOUNCE_MS 1000

typedef struct {
    uint8_t address[6];                    /*!< NimBLE order: least significant byte first */
    char text[CONFIG_SERVER_BT_ADDRESS_LENGTH];  /*!< AA:BB:CC:DD:EE:FF, for logs */
    char name[CONFIG_SERVER_BT_NAME_LENGTH];     /*!< Friendly name from the config page */
    bool state_known;
    bool is_open;
    int64_t last_change_us;
} ble_sensor_t;

typedef struct {
    size_t sensor_index;
    bool is_open;
    int8_t rssi;
} ble_door_event_t;

static ble_sensor_t sensors[CONFIG_SERVER_MAX_BT_ADDRESSES];
static size_t sensor_count;
static QueueHandle_t event_queue;

/**
 * @brief Convert "AA:BB:CC:DD:EE:FF" into the byte order NimBLE reports.
 */
static bool parse_address(const char *text, uint8_t *address)
{
    unsigned int bytes[6];
    if (sscanf(text, "%2x:%2x:%2x:%2x:%2x:%2x",
               &bytes[0], &bytes[1], &bytes[2],
               &bytes[3], &bytes[4], &bytes[5]) != 6) {
        return false;
    }

    /* Advertisement reports carry the address least significant byte first. */
    for (int index = 0; index < 6; ++index) {
        address[index] = (uint8_t)bytes[5 - index];
    }
    return true;
}

/**
 * @brief Locate one AD element inside an advertisement payload.
 *
 * Advertisement data is a sequence of [length][type][value...] elements.
 *
 * @param[in]  data          Advertisement payload
 * @param[in]  length        Payload length
 * @param[in]  type          AD type to look for, e.g. BLE_HS_ADV_TYPE_MFG_DATA
 * @param[out] field_length  Length of the returned value, 0 when not found
 *
 * @return Pointer to the value bytes, or NULL when the type is not present.
 */
static const uint8_t *find_ad_field(const uint8_t *data, uint8_t length,
                                    uint8_t type, uint8_t *field_length)
{
    uint8_t offset = 0;
    while (offset + 1 < length) {
        uint8_t element_length = data[offset];
        if (element_length == 0 || offset + 1 + element_length > length) {
            break;
        }
        if (data[offset + 1] == type) {
            *field_length = element_length - 1;
            return &data[offset + 2];
        }
        offset += element_length + 1;
    }

    *field_length = 0;
    return NULL;
}

/* IM24-BLE advertisement layout:
 *
 *   02 01 06 | 09 08 "iSensor " | 09 FF 10 49 7D DE 46 00 3B 35
 *         ^^   device type tag,    ^^^^^ ^^^^^^^^ ^^ ^^ ^^
 *              0x06 = door sensor  |     |        |  |  └─ unidentified
 *                                  |     |        |  └──── unidentified
 *                                  |     |        └─────── state, bit 1 = open
 *                                  |     └──────────────── unidentified
 *                                  └────────────────────── company id 0x4910
 *
 * The state offset counts from the start of the manufacturer data value, so
 * the two company identifier bytes are included in the index.
 */
#define IM24_DEVICE_TYPE_DOOR_SENSOR 0x06
#define IM24_STATE_OFFSET 5
#define IM24_STATE_OPEN_MASK 0x02

/**
 * @brief Decode an IM24-BLE advertisement into a door state.
 *
 * Every advertisement from a configured sensor is also logged as raw hex at
 * debug level. Enable it with esp_log_level_set("BLE_SENSOR", ESP_LOG_DEBUG).
 *
 * @param[in]  data     Advertisement payload
 * @param[in]  length   Payload length
 * @param[out] is_open  Decoded door state, only valid when this returns true
 *
 * @return true when the advertisement carried a door state.
 */
static bool decode_door_state(const uint8_t *data, uint8_t length, bool *is_open)
{
    /* The first element tags the device type; ignore anything else the sensor sends. */
    uint8_t type_length = 0;
    const uint8_t *device_type = find_ad_field(data, length, BLE_HS_ADV_TYPE_FLAGS,
                                               &type_length);
    if (device_type == NULL || type_length < 1 ||
        device_type[0] != IM24_DEVICE_TYPE_DOOR_SENSOR) {
        return false;
    }

    uint8_t mfg_length = 0;
    const uint8_t *mfg_data = find_ad_field(data, length, BLE_HS_ADV_TYPE_MFG_DATA,
                                            &mfg_length);
    if (mfg_data == NULL || mfg_length <= IM24_STATE_OFFSET) {
        return false;
    }

    *is_open = (mfg_data[IM24_STATE_OFFSET] & IM24_STATE_OPEN_MASK) != 0;
    return true;
}

/**
 * @brief NimBLE discovery callback.
 *
 * Runs on the NimBLE host task, so it only matches, decodes and queues. The
 * display and the ntfy POST are left to ble_sensor_task.
 */
static int gap_event_handler(struct ble_gap_event *event, void *arg)
{
    if (event->type != BLE_GAP_EVENT_DISC) {
        return 0;
    }

    const struct ble_gap_disc_desc *desc = &event->disc;
    for (size_t index = 0; index < sensor_count; ++index) {
        ble_sensor_t *sensor = &sensors[index];
        if (memcmp(desc->addr.val, sensor->address, sizeof(sensor->address)) != 0) {
            continue;
        }

        ESP_LOGD(TAG, "%s rssi=%d len=%u", sensor->text, desc->rssi, desc->length_data);
        ESP_LOG_BUFFER_HEX_LEVEL(TAG, desc->data, desc->length_data, ESP_LOG_DEBUG);

        bool is_open = false;
        if (!decode_door_state(desc->data, desc->length_data, &is_open)) {
            ESP_LOGD(TAG, "%s advertisement carried no door state", sensor->text);
            return 0;
        }
        ESP_LOGD(TAG, "%s decoded as %s", sensor->text, is_open ? "open" : "closed");

        /* Sensors re-advertise the same state continuously; report transitions. */
        if (sensor->state_known && sensor->is_open == is_open) {
            return 0;
        }

        int64_t now_us = esp_timer_get_time();
        if (sensor->state_known &&
            (now_us - sensor->last_change_us) < (int64_t)BLE_STATE_DEBOUNCE_MS * 1000) {
            return 0;
        }

        sensor->state_known = true;
        sensor->is_open = is_open;
        sensor->last_change_us = now_us;

        ble_door_event_t door_event = {
            .sensor_index = index,
            .is_open = is_open,
            .rssi = desc->rssi,
        };

        /* Never block the host task: drop the event if the worker is behind. */
        if (xQueueSend(event_queue, &door_event, 0) != pdTRUE) {
            ESP_LOGW(TAG, "Door event queue full; dropped %s event", sensor->text);
        }
        return 0;
    }

    return 0;
}

/**
 * @brief Report door transitions on the display and through ntfy.
 */
static void ble_sensor_task(void *argument)
{
    ble_door_event_t door_event;
    char topic[CONFIG_SERVER_TOPIC_LENGTH];
    char line[32];
    char message[96];

    while (true) {
        if (xQueueReceive(event_queue, &door_event, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        const ble_sensor_t *sensor = &sensors[door_event.sensor_index];
        const char *label = sensor->name[0] != '\0' ? sensor->name : sensor->text;
        const char *state = door_event.is_open ? "OPEN" : "closed";

        ESP_LOGI(TAG, "%s (%s) is %s, rssi=%d", label, sensor->text, state, door_event.rssi);

        snprintf(line, sizeof(line), "%s %s", label, state);
        displayString(line);

        if (get_config_topic(topic, sizeof(topic)) == ESP_OK && topic[0] != '\0') {
            snprintf(message, sizeof(message), "%s (%s) is %s", label, sensor->text, state);
            esp_err_t ret = send_ntfy_notification(topic, message);
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "Could not queue door notification: %s", esp_err_to_name(ret));
            }
        } else {
            ESP_LOGW(TAG, "No ntfy Topic configured; door notification skipped");
        }
    }
}

static void on_host_sync(void)
{
    uint8_t own_addr_type;
    int ret = ble_hs_id_infer_auto(0, &own_addr_type);
    if (ret != 0) {
        ESP_LOGE(TAG, "Could not determine BLE address type: %d", ret);
        return;
    }

    struct ble_gap_disc_params params = {
        .itvl = BLE_GAP_SCAN_ITVL_MS(BLE_SCAN_INTERVAL_MS),
        .window = BLE_GAP_SCAN_WIN_MS(BLE_SCAN_WINDOW_MS),
        .filter_policy = 0,
        .limited = 0,
        .passive = 1,           /* Door sensors broadcast; no scan request needed. */
        .filter_duplicates = 0, /* Repeats are how state changes arrive. */
    };

    ret = ble_gap_disc(own_addr_type, BLE_HS_FOREVER, &params, gap_event_handler, NULL);
    if (ret != 0) {
        ESP_LOGE(TAG, "Could not start BLE scan: %d", ret);
        return;
    }

    ESP_LOGI(TAG, "BLE scan started for %u sensor(s)", (unsigned)sensor_count);
}

static void on_host_reset(int reason)
{
    ESP_LOGW(TAG, "NimBLE host reset (reason %d); scan restarts once resynced", reason);
}

static void nimble_host_task(void *argument)
{
    nimble_port_run();  /* Returns only when the stack is stopped. */
    nimble_port_freertos_deinit();
}

esp_err_t start_ble_sensor_monitor(void)
{
    sensor_count = 0;
    for (int index = 0; index < CONFIG_SERVER_MAX_BT_ADDRESSES; ++index) {
        char address[CONFIG_SERVER_BT_ADDRESS_LENGTH] = {0};
        char name[CONFIG_SERVER_BT_NAME_LENGTH] = {0};

        if (get_config_bt_sensor(index, address, sizeof(address),
                                 name, sizeof(name)) != ESP_OK || address[0] == '\0') {
            continue;
        }

        ble_sensor_t *sensor = &sensors[sensor_count];
        if (!parse_address(address, sensor->address)) {
            ESP_LOGW(TAG, "Ignoring malformed Bluetooth address '%s'", address);
            continue;
        }

        strncpy(sensor->text, address, sizeof(sensor->text) - 1);
        sensor->text[sizeof(sensor->text) - 1] = '\0';
        strncpy(sensor->name, name, sizeof(sensor->name) - 1);
        sensor->name[sizeof(sensor->name) - 1] = '\0';
        sensor->state_known = false;
        sensor->last_change_us = 0;

        ESP_LOGI(TAG, "Watching sensor %u: %s (%s)", (unsigned)sensor_count,
                 sensor->text, sensor->name[0] != '\0' ? sensor->name : "unnamed");
        ++sensor_count;
    }

    if (sensor_count == 0) {
        ESP_LOGW(TAG, "No Bluetooth sensors configured; BLE monitor not started");
        return ESP_OK;
    }

    event_queue = xQueueCreate(BLE_EVENT_QUEUE_DEPTH, sizeof(ble_door_event_t));
    if (event_queue == NULL) {
        return ESP_ERR_NO_MEM;
    }

    if (xTaskCreate(ble_sensor_task, "ble_sensor", 4096, NULL, 5, NULL) != pdPASS) {
        vQueueDelete(event_queue);
        event_queue = NULL;
        return ESP_ERR_NO_MEM;
    }

    esp_err_t ret = nimble_port_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Could not initialise NimBLE: %s", esp_err_to_name(ret));
        return ret;
    }

    ble_hs_cfg.sync_cb = on_host_sync;
    ble_hs_cfg.reset_cb = on_host_reset;
    nimble_port_freertos_init(nimble_host_task);
    return ESP_OK;
}
