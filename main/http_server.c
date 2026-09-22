#include "http_server.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"

void displayString(char* str);

static const char *TAG = "CONFIG_HTTP";
static httpd_handle_t server_handle;

typedef struct {
    char device_name[CONFIG_SERVER_TEXT_LENGTH];
    char location[CONFIG_SERVER_TEXT_LENGTH];
    char mobile_phone[CONFIG_SERVER_PHONE_LENGTH];
    char topic[CONFIG_SERVER_TOPIC_LENGTH];
    char bluetooth_addresses[CONFIG_SERVER_MAX_BT_ADDRESSES][CONFIG_SERVER_BT_ADDRESS_LENGTH];
    char bluetooth_names[CONFIG_SERVER_MAX_BT_ADDRESSES][CONFIG_SERVER_BT_NAME_LENGTH];
} device_settings_t;

static device_settings_t settings;

/* Form bodies are heap allocated: handlers run on the httpd task stack. */
#define FORM_BODY_SIZE 2048

static void load_settings(void)
{
    nvs_handle_t handle;
    esp_err_t ret = nvs_open("device_cfg", NVS_READONLY, &handle);
    if (ret != ESP_OK) {
        return;
    }

    size_t length = sizeof(settings.device_name);
    nvs_get_str(handle, "name", settings.device_name, &length);
    length = sizeof(settings.location);
    nvs_get_str(handle, "location", settings.location, &length);
    length = sizeof(settings.mobile_phone);
    nvs_get_str(handle, "phone", settings.mobile_phone, &length);
    length = sizeof(settings.topic);
    nvs_get_str(handle, "topic", settings.topic, &length);

    for (int index = 0; index < CONFIG_SERVER_MAX_BT_ADDRESSES; ++index) {
        char key[12];
        snprintf(key, sizeof(key), "bt%d", index);
        length = sizeof(settings.bluetooth_addresses[index]);
        nvs_get_str(handle, key, settings.bluetooth_addresses[index], &length);
        snprintf(key, sizeof(key), "bt_name%d", index);
        length = sizeof(settings.bluetooth_names[index]);
        nvs_get_str(handle, key, settings.bluetooth_names[index], &length);
    }
    displayString("Load Config..");

    nvs_close(handle);
}

static esp_err_t save_settings(void)
{
    nvs_handle_t handle;
    esp_err_t ret = nvs_open("device_cfg", NVS_READWRITE, &handle);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = nvs_set_str(handle, "name", settings.device_name);
    if (ret == ESP_OK) {
        ret = nvs_set_str(handle, "location", settings.location);
    }
    if (ret == ESP_OK) {
        ret = nvs_set_str(handle, "phone", settings.mobile_phone);
    }
    if (ret == ESP_OK) {
        ret = nvs_set_str(handle, "topic", settings.topic);
    }
    for (int index = 0; ret == ESP_OK && index < CONFIG_SERVER_MAX_BT_ADDRESSES; ++index) {
        char key[12];
        snprintf(key, sizeof(key), "bt%d", index);
        ret = nvs_set_str(handle, key, settings.bluetooth_addresses[index]);
        if (ret == ESP_OK) {
            snprintf(key, sizeof(key), "bt_name%d", index);
            ret = nvs_set_str(handle, key, settings.bluetooth_names[index]);
        }
    }
    if (ret == ESP_OK) {
        ret = nvs_commit(handle);
    }

    nvs_close(handle);
    displayString("Save Config..");
    return ret;
}

/*
 * Worst-case expansion for html_escape: '"' becomes "&quot;", six bytes per
 * character. A source buffer of SIZE holds at most SIZE - 1 characters, so
 * (SIZE - 1) * 6 + 1 bytes are needed, which SIZE * 6 always covers.
 */
#define HTML_ESCAPED_SIZE(size) ((size) * 6)

static void html_escape(char *destination, size_t destination_size, const char *source)
{
    size_t used = 0;
    for (size_t index = 0; source[index] != '\0' && used + 1 < destination_size; ++index) {
        const char *replacement = NULL;
        switch (source[index]) {
        case '&': replacement = "&amp;"; break;
        case '<': replacement = "&lt;"; break;
        case '>': replacement = "&gt;"; break;
        case '"': replacement = "&quot;"; break;
        default: break;
        }

        if (replacement != NULL) {
            size_t replacement_length = strlen(replacement);
            if (used + replacement_length >= destination_size) {
                break;
            }
            memcpy(destination + used, replacement, replacement_length);
            used += replacement_length;
        } else {
            destination[used++] = source[index];
        }
    }
    destination[used] = '\0';
}

static int hex_value(char value)
{
    if (value >= '0' && value <= '9') {
        return value - '0';
    }
    if (value >= 'a' && value <= 'f') {
        return value - 'a' + 10;
    }
    if (value >= 'A' && value <= 'F') {
        return value - 'A' + 10;
    }
    return -1;
}

static bool normalize_bluetooth_address(const char *input, char *output)
{
    char hex[13] = {0};
    size_t count = 0;
    for (size_t index = 0; input[index] != '\0'; ++index) {
        if (input[index] == ':' || input[index] == '-' || isspace((unsigned char)input[index])) {
            continue;
        }
        if (hex_value(input[index]) < 0 || count >= 12) {
            return false;
        }
        hex[count++] = input[index];
    }
    if (count != 12) {
        return false;
    }

    for (int index = 0; index < 6; ++index) {
        if (index != 0) {
            output[index * 3 - 1] = ':';
        }
        output[index * 3] = (char)toupper((unsigned char)hex[index * 2]);
        output[index * 3 + 1] = (char)toupper((unsigned char)hex[index * 2 + 1]);
    }
    output[17] = '\0';
    return true;
}

static int decode_form_value(const char *body, const char *key, char *output, size_t output_size)
{
    char search_key[24];
    snprintf(search_key, sizeof(search_key), "%s=", key);
    const char *start = strstr(body, search_key);
    if (start == NULL || (start != body && start[-1] != '&')) {
        output[0] = '\0';
        return 0;
    }

    start += strlen(search_key);
    size_t length = 0;
    while (start[length] != '\0' && start[length] != '&') {
        ++length;
    }

    size_t output_length = 0;
    for (size_t index = 0; index < length && output_length + 1 < output_size; ++index) {
        char value = start[index];
        if (value == '+') {
            value = ' ';
        } else if (value == '%' && index + 2 < length) {
            int high = hex_value(start[index + 1]);
            int low = hex_value(start[index + 2]);
            if (high >= 0 && low >= 0) {
                value = (char)((high << 4) | low);
                index += 2;
            }
        }
        output[output_length++] = value;
    }
    output[output_length] = '\0';
    return 1;
}

static bool is_alphanumeric_value(const char *value, size_t minimum_length, size_t maximum_length)
{
    size_t length = strlen(value);
    if (length < minimum_length || length > maximum_length) {
        return false;
    }
    for (size_t index = 0; index < length; ++index) {
        if (!isalnum((unsigned char)value[index])) {
            return false;
        }
    }
    return true;
}

static esp_err_t receive_body(httpd_req_t *request, char *body, size_t body_size)
{
    if (request->content_len <= 0 || (size_t)request->content_len >= body_size) {
        return ESP_ERR_INVALID_SIZE;
    }

    int received = httpd_req_recv(request, body, request->content_len);
    if (received <= 0) {
        return ESP_FAIL;
    }
    body[received] = '\0';
    return ESP_OK;
}

static void send_page(httpd_req_t *request, const char *message)
{
    char name[HTML_ESCAPED_SIZE(CONFIG_SERVER_TEXT_LENGTH)];
    char location[HTML_ESCAPED_SIZE(CONFIG_SERVER_TEXT_LENGTH)];
    char phone[HTML_ESCAPED_SIZE(CONFIG_SERVER_PHONE_LENGTH)];
    char topic[HTML_ESCAPED_SIZE(CONFIG_SERVER_TOPIC_LENGTH)];
    char escaped_address[HTML_ESCAPED_SIZE(CONFIG_SERVER_BT_ADDRESS_LENGTH)];
    char escaped_bluetooth_name[HTML_ESCAPED_SIZE(CONFIG_SERVER_BT_NAME_LENGTH)];
    char ssid[33] = {0};
    char escaped_ssid[HTML_ESCAPED_SIZE(sizeof(ssid))];
    char ip_address[16] = "unavailable";
    wifi_config_t wifi_config = {0};

    html_escape(name, sizeof(name), settings.device_name);
    html_escape(location, sizeof(location), settings.location);
    html_escape(phone, sizeof(phone), settings.mobile_phone);
    html_escape(topic, sizeof(topic), settings.topic);

    if (esp_wifi_get_config(WIFI_IF_STA, &wifi_config) == ESP_OK) {
        memcpy(ssid, wifi_config.sta.ssid, sizeof(ssid) - 1);
    }
    html_escape(escaped_ssid, sizeof(escaped_ssid), ssid[0] != '\0' ? ssid : "unavailable");

    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t ip_info = {0};
    if (netif != NULL && esp_netif_get_ip_info(netif, &ip_info) == ESP_OK && ip_info.ip.addr != 0) {
        snprintf(ip_address, sizeof(ip_address), IPSTR, IP2STR(&ip_info.ip));
    }

    char *page = calloc(1, 7000);
    if (page == NULL) {
        httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return;
    }

    int used = snprintf(page, 7000,
                        "<!doctype html><html><head><meta name=viewport content=\"width=device-width,initial-scale=1\"><title>ESP32 Configuration</title>"
                        "<style>body{font-family:Arial;max-width:720px;margin:24px auto;padding:0 16px}input{width:100%%;padding:8px;margin:4px 0 12px;box-sizing:border-box}button{padding:10px 16px;margin:6px 6px 6px 0}table{width:100%%}td{padding:3px}</style></head><body>"
                        "<h2>ESP32 Configuration</h2><form method=post action=/save>"
                        "<h3>Wi-Fi status</h3><p>SSID: %s<br>DHCP IP: %s</p>"
                        "<label>Device name (max 15 characters)</label><input maxlength=15 name=device_name value=\"%s\">"
                        "<label>Device location (max 15 characters)</label><input maxlength=15 name=location value=\"%s\">"
                        "<label>Mobile phone number (max 20 characters)</label><input maxlength=20 name=mobile_phone value=\"%s\">"
                        "<label>Topic (3 to 12 alphanumeric characters)</label><input maxlength=12 minlength=3 name=topic value=\"%s\"><h3>Bluetooth devices</h3><table>",
                        escaped_ssid, ip_address, name, location, phone, topic);

    for (int index = 0; index < CONFIG_SERVER_MAX_BT_ADDRESSES && used > 0 && used < 7000; ++index) {
        html_escape(escaped_address, sizeof(escaped_address), settings.bluetooth_addresses[index]);
        html_escape(escaped_bluetooth_name, sizeof(escaped_bluetooth_name), settings.bluetooth_names[index]);
        used += snprintf(page + used, 7000 - (size_t)used,
                         "<tr><td>%d</td><td><input maxlength=17 name=bt%d placeholder=AA:BB:CC:DD:EE:FF value=\"%s\"></td>"
                         "<td><input maxlength=15 name=bt_name%d placeholder=Device name value=\"%s\"></td></tr>",
                         index + 1, index, escaped_address, index, escaped_bluetooth_name);
    }

    if (used > 0 && used < 7000) {
        used += snprintf(page + used, 7000 - (size_t)used,
                         "</table><button type=submit name=action value=save>Save values</button></form><p>%s</p></body></html>",
                         message != NULL ? message : "");
    }
    httpd_resp_set_type(request, "text/html");
    httpd_resp_send(request, page, HTTPD_RESP_USE_STRLEN);
    free(page);
}

static esp_err_t root_handler(httpd_req_t *request)
{
    send_page(request, "");
    return ESP_OK;
}

static esp_err_t save_handler(httpd_req_t *request)
{
    char *body = malloc(FORM_BODY_SIZE);
    if (body == NULL) {
        httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return ESP_FAIL;
    }

    if (receive_body(request, body, FORM_BODY_SIZE) != ESP_OK) {
        free(body);
        httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "Invalid form data");
        return ESP_FAIL;
    }

    char value[64];
    decode_form_value(body, "device_name", value, sizeof(value));
    strncpy(settings.device_name, value, sizeof(settings.device_name) - 1);
    settings.device_name[sizeof(settings.device_name) - 1] = '\0';
    decode_form_value(body, "location", value, sizeof(value));
    strncpy(settings.location, value, sizeof(settings.location) - 1);
    settings.location[sizeof(settings.location) - 1] = '\0';

    decode_form_value(body, "mobile_phone", value, sizeof(value));
    strncpy(settings.mobile_phone, value, sizeof(settings.mobile_phone) - 1);
    settings.mobile_phone[sizeof(settings.mobile_phone) - 1] = '\0';

    decode_form_value(body, "topic", value, sizeof(value));
    if (!is_alphanumeric_value(value, 3, CONFIG_SERVER_TOPIC_LENGTH - 1)) {
        free(body);
        send_page(request, "Topic must contain 3 to 12 alphanumeric characters.");
        return ESP_OK;
    }
    strncpy(settings.topic, value, sizeof(settings.topic) - 1);
    settings.topic[sizeof(settings.topic) - 1] = '\0';

    for (int index = 0; index < CONFIG_SERVER_MAX_BT_ADDRESSES; ++index) {
        char key[12];
        char normalized[CONFIG_SERVER_BT_ADDRESS_LENGTH];
        snprintf(key, sizeof(key), "bt%d", index);
        decode_form_value(body, key, value, sizeof(value));
        if (value[0] == '\0') {
            settings.bluetooth_addresses[index][0] = '\0';
        } else if (normalize_bluetooth_address(value, normalized)) {
            strcpy(settings.bluetooth_addresses[index], normalized);
        } else {
            free(body);
            send_page(request, "Invalid Bluetooth address. Use 12 hex digits, optionally separated by ':' or '-'.");
            return ESP_OK;
        }

        snprintf(key, sizeof(key), "bt_name%d", index);
        decode_form_value(body, key, value, sizeof(value));
        strncpy(settings.bluetooth_names[index], value, sizeof(settings.bluetooth_names[index]) - 1);
        settings.bluetooth_names[index][sizeof(settings.bluetooth_names[index]) - 1] = '\0';
    }

    free(body);

    esp_err_t ret = save_settings();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Could not save settings: %s", esp_err_to_name(ret));
        send_page(request, "Could not save settings to flash.");
        return ESP_OK;
    }

    send_page(request, "Values saved.");
    return ESP_OK;
}

static void configuration_server_task(void *argument)
{
    load_settings();
    esp_netif_ip_info_t ip_info = {0};

    while (true) {
        esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        if (netif != NULL && esp_netif_get_ip_info(netif, &ip_info) == ESP_OK && ip_info.ip.addr != 0) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    /* The provisioning HTTP server may still be releasing port 80. */
    vTaskDelay(pdMS_TO_TICKS(2000));
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.max_open_sockets = 4;
    config.stack_size = 8192;  /* 4096 default overflows in the form handlers */
    while (httpd_start(&server_handle, &config) != ESP_OK) {
        ESP_LOGW(TAG, "Port 80 is still busy; retrying HTTP server startup");
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    displayString("HTTP Server Started");

    httpd_uri_t root_uri = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = root_handler,
    };
    httpd_uri_t save_uri = {
        .uri = "/save",
        .method = HTTP_POST,
        .handler = save_handler,
    };
    httpd_register_uri_handler(server_handle, &root_uri);
    httpd_register_uri_handler(server_handle, &save_uri);

    ESP_LOGI(TAG, "Configuration server available at http://" IPSTR, IP2STR(&ip_info.ip));
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}

esp_err_t start_configuration_server(void)
{
    BaseType_t result = xTaskCreate(configuration_server_task, "config_http", 8192, NULL, 5, NULL);
    return result == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t get_config_topic(char *topic, size_t topic_size)
{
    if (topic == NULL || topic_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    topic[0] = '\0';
    nvs_handle_t handle;
    esp_err_t ret = nvs_open("device_cfg", NVS_READONLY, &handle);
    if (ret != ESP_OK) {
        return ret;
    }

    size_t length = topic_size;
    ret = nvs_get_str(handle, "topic", topic, &length);
    nvs_close(handle);
    return ret;
}

esp_err_t get_config_bt_sensor(int index, char *address, size_t address_size,
                               char *name, size_t name_size)
{
    if (index < 0 || index >= CONFIG_SERVER_MAX_BT_ADDRESSES ||
        address == NULL || address_size == 0 || name == NULL || name_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    address[0] = '\0';
    name[0] = '\0';

    nvs_handle_t handle;
    esp_err_t ret = nvs_open("device_cfg", NVS_READONLY, &handle);
    if (ret != ESP_OK) {
        return ret;
    }

    char key[12];
    snprintf(key, sizeof(key), "bt%d", index);
    size_t length = address_size;
    ret = nvs_get_str(handle, key, address, &length);

    if (ret == ESP_OK) {
        snprintf(key, sizeof(key), "bt_name%d", index);
        length = name_size;
        if (nvs_get_str(handle, key, name, &length) != ESP_OK) {
            name[0] = '\0';
        }
    }

    nvs_close(handle);
    return ret;
}
