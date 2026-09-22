#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/i2c_master.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "network_provisioning/manager.h"
#include "network_provisioning/scheme_softap.h"
#include "http_server.h"
#include "ble_sensors.h"
#include "sdkconfig.h"
#include "u8g2.h"

void wifi_eraseconfig(void);
void displayString(char *str);
/* Shown on separate lines: the joined SSID and IP do not fit on one 128px row. */
char connection_ssid[33];
char connection_ip[24];
char topic[CONFIG_SERVER_TOPIC_LENGTH] = {0};

static const char *TAG = "U8G2";
u8g2_t u8g2;
static volatile bool station_connected;
static bool station_reconnect_enabled;
static TaskHandle_t reconnect_task_handle;
static SemaphoreHandle_t display_mutex;

typedef struct {
    char *topic;
    char *message;
} ntfy_notification_args_t;

static void ntfy_notification_task(void *argument)
{
    ntfy_notification_args_t *args = (ntfy_notification_args_t *)argument;

    char url[64];
    int url_length = snprintf(url, sizeof(url), "https://ntfy.sh/%s", args->topic);
    if (url_length < 0 || url_length >= (int)sizeof(url)) {
        ESP_LOGE(TAG, "ntfy Topic URL is too long");
        goto cleanup;
    }

    vTaskDelay(pdMS_TO_TICKS(2000));
    ESP_LOGI(TAG, "Sending ntfy notification to %s", url);

    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 10000,
    };
    for (int attempt = 1; attempt <= 3; ++attempt) {
        esp_http_client_handle_t client = esp_http_client_init(&config);
        if (client == NULL) {
            ESP_LOGE(TAG, "Could not allocate ntfy HTTP client");
            break;
        }

        esp_err_t ret = esp_http_client_set_header(client, "Content-Type", "text/plain");
        if (ret == ESP_OK) {
            ret = esp_http_client_set_post_field(client, args->message, strlen(args->message));
        }
        if (ret == ESP_OK) {
            ret = esp_http_client_perform(client);
        }
        if (ret == ESP_OK) {
            int status_code = esp_http_client_get_status_code(client);
            if (status_code >= 200 && status_code < 300) {
                ESP_LOGI(TAG, "ntfy notification sent successfully");
                esp_http_client_cleanup(client);
                break;
            }
            ESP_LOGE(TAG, "ntfy.sh returned HTTP status %d (attempt %d)", status_code, attempt);
        } else {
            ESP_LOGE(TAG, "ntfy.sh request failed on attempt %d: %s (0x%x)",
                     attempt, esp_err_to_name(ret), ret);
        }
        esp_http_client_cleanup(client);
        if (attempt < 3) {
            vTaskDelay(pdMS_TO_TICKS(2000));
        }
    }

cleanup:
    free(args->topic);
    free(args->message);
    free(args);
    vTaskDelete(NULL);
}

/* Not static: ble_sensors.c reports door events through the same channel. */
esp_err_t send_ntfy_notification(const char *topic, const char *message)
{
    if (topic == NULL || message == NULL || topic[0] == '\0' || message[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    ntfy_notification_args_t *args = calloc(1, sizeof(*args));
    if (args == NULL) {
        return ESP_ERR_NO_MEM;
    }
    args->topic = strdup(topic);
    args->message = strdup(message);
    if (args->topic == NULL || args->message == NULL) {
        free(args->topic);
        free(args->message);
        free(args);
        return ESP_ERR_NO_MEM;
    }

    BaseType_t task_result = xTaskCreate(ntfy_notification_task, "ntfy_http", 6144,
                                         args, 5, NULL);
    if (task_result != pdPASS) {
        free(args->topic);
        free(args->message);
        free(args);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static void wifi_reconnect_task(void *argument)
{
    esp_err_t ret;
    while (station_reconnect_enabled) {
        if (!station_connected) {
            ret = send_ntfy_notification(topic, "WiFi reconnecting");
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "Could not queue WiFi notification: %s", esp_err_to_name(ret));
            }
            displayString("Wi-Fi reconnecting..");
            ret = esp_wifi_connect();
            if (ret != ESP_OK && ret != ESP_ERR_WIFI_CONN) {
                ESP_LOGW(TAG, "Wi-Fi reconnect attempt failed: %s", esp_err_to_name(ret));
            } else {
                ESP_LOGI(TAG, "Wi-Fi reconnect attempt started");
                displayString("Wi-Fi reconnecting..");
            }
        }
        vTaskDelay(pdMS_TO_TICKS(5000));
    }

    reconnect_task_handle = NULL;
    vTaskDelete(NULL);
}

/* I2C Configuration (configurable via menuconfig) */
#define I2C_MASTER_NUM    I2C_NUM_0                        /*!< I2C master port number */
#define I2C_MASTER_SDA_IO 26
#define I2C_MASTER_SCL_IO 25
//#define I2C_MASTER_SDA_IO CONFIG_I2C_MASTER_SDA            /*!< GPIO number used for I2C master data  */
//#define I2C_MASTER_SCL_IO CONFIG_I2C_MASTER_SCL            /*!< GPIO number used for I2C master clock */
#define I2C_FREQ_HZ       CONFIG_I2C_MASTER_FREQUENCY      /*!< I2C master clock frequency */
#define I2C_TIMEOUT_MS    1000                             /*!< I2C master timeout */

/* Display Configuration (configurable via menuconfig) */
#define I2C_DISPLAY_ADDRESS  CONFIG_I2C_DISPLAY_ADDRESS    /*!< Display I2C address */

static i2c_master_bus_handle_t i2c_bus_handle = NULL;   /*!< I2C master bus handle */
static i2c_master_dev_handle_t display_dev_handle = NULL;  /*!< Display device handle */

static void security_text_display(u8g2_t *display)
{
    ESP_LOGI(TAG, "Security Text Display");

    xSemaphoreTakeRecursive(display_mutex, portMAX_DELAY);
    u8g2_ClearBuffer(display);
    u8g2_SetFont(display, u8g2_font_ncenB12_tr);
    u8g2_DrawStr(display, 0, 16, "Security Dev");
    u8g2_SetFont(display, u8g2_font_ncenB08_tr);
    u8g2_DrawStr(display, 0, 32, "aseemsethi@yahoo.com");
    u8g2_SetFont(display, u8g2_font_5x7_tr);
    u8g2_DrawStr(display, 0, 44, "Sept 2026");
    u8g2_DrawStr(display, 0, 54, "Bengaluru, India");
    u8g2_SendBuffer(display);
    xSemaphoreGiveRecursive(display_mutex);
    vTaskDelay(pdMS_TO_TICKS(4000));
}

static void provisioning_event_handler(void *arg, esp_event_base_t event_base,
                                       int32_t event_id, void *event_data)
{
    xSemaphoreTakeRecursive(display_mutex, portMAX_DELAY);
    u8g2_ClearBuffer(&u8g2);
    u8g2_SetFont(&u8g2, u8g2_font_ncenB08_tr);
    if (event_base == NETWORK_PROV_EVENT) {
        switch (event_id) {
        case NETWORK_PROV_START:
            ESP_LOGI(TAG, "Wi-Fi provisioning started");
            u8g2_DrawStr(&u8g2, 0, 32, "WiFi provisioning started");
            break;
        case NETWORK_PROV_WIFI_CRED_SUCCESS:
            ESP_LOGI(TAG, "Wi-Fi credentials accepted");
            u8g2_DrawStr(&u8g2, 0, 32, "WiFi credentials accepted");
            break;
        case NETWORK_PROV_WIFI_CRED_FAIL:
            ESP_LOGE(TAG, "Wi-Fi credentials failed");
            u8g2_DrawStr(&u8g2, 0, 32, "WiFi credentials failed");
            break;
        case NETWORK_PROV_END:
            ESP_ERROR_CHECK(network_prov_mgr_deinit());
            break;
        default:
            break;
        }
    } else if (event_base == WIFI_EVENT) {
        if (event_id == WIFI_EVENT_STA_START) {
            esp_err_t ret = esp_wifi_connect();
            if (ret != ESP_OK) {
                ESP_LOGW(TAG, "Wi-Fi station connect deferred: %s", esp_err_to_name(ret));
                u8g2_DrawStr(&u8g2, 0, 32, "Defer connect to Wi-Fi");
            }
        } else if (event_id == WIFI_EVENT_STA_DISCONNECTED) {
            station_connected = false;
            ESP_LOGW(TAG, "Wi-Fi disconnected; retry task will reconnect until successful");
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        station_connected = true;
        displayString("Wi-Fi connected");
        const ip_event_got_ip_t *got_ip = (const ip_event_got_ip_t *)event_data;
        wifi_config_t wifi_config = {0};
        esp_err_t ret = esp_wifi_get_config(WIFI_IF_STA, &wifi_config);
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "Connected to Wi-Fi SSID: %s", (char *)wifi_config.sta.ssid);
            if (got_ip != NULL) {
                ESP_LOGI(TAG, "DHCP IP address: " IPSTR, IP2STR(&got_ip->ip_info.ip));
            }
        } else {
            ESP_LOGE(TAG, "Could not read Wi-Fi SSID: %s", esp_err_to_name(ret));
        }
        char ssid_str[33] = {0};
        memcpy(ssid_str, wifi_config.sta.ssid, sizeof(ssid_str) - 1);

        snprintf(connection_ssid, sizeof(connection_ssid), "%s", ssid_str);

        if (got_ip != NULL) {
            snprintf(connection_ip, sizeof(connection_ip), "IP: " IPSTR,
                     IP2STR(&got_ip->ip_info.ip));

            if (get_config_topic(topic, sizeof(topic)) == ESP_OK && topic[0] != '\0') {
                ret = send_ntfy_notification(topic, "WiFi connected");
                if (ret != ESP_OK) {
                    ESP_LOGE(TAG, "Could not queue WiFi notification: %s", esp_err_to_name(ret));
                }
            } else {
                ESP_LOGW(TAG, "No ntfy Topic configured; WiFi notification skipped");
            }
        } else {
            connection_ip[0] = '\0';
        }
        u8g2_DrawStr(&u8g2, 0, 28, connection_ssid);
        u8g2_DrawStr(&u8g2, 0, 40, connection_ip);
    }
    u8g2_SendBuffer(&u8g2);
    xSemaphoreGiveRecursive(display_mutex);
}

static void start_wifi_provisioning(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    } else {
        ESP_ERROR_CHECK(ret);
    }

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t wifi_config = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wifi_config));
    ESP_ERROR_CHECK(esp_event_handler_register(NETWORK_PROV_EVENT, ESP_EVENT_ANY_ID,
                                               provisioning_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                               provisioning_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                               provisioning_event_handler, NULL));

    network_prov_mgr_config_t manager_config = {
        .scheme = network_prov_scheme_softap,
        .scheme_event_handler = NETWORK_PROV_EVENT_HANDLER_NONE,
        .app_event_handler = NETWORK_PROV_EVENT_HANDLER_NONE,
    };
    ESP_ERROR_CHECK(network_prov_mgr_init(manager_config));

    bool provisioned = false;
    ESP_ERROR_CHECK(network_prov_mgr_is_wifi_provisioned(&provisioned));

    if (provisioned) {
        ESP_LOGI(TAG, "Wi-Fi credentials found in flash; starting station mode");
        station_reconnect_enabled = true;
        ESP_ERROR_CHECK(network_prov_mgr_deinit());
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        ESP_ERROR_CHECK(esp_wifi_start());
        BaseType_t task_result = xTaskCreate(wifi_reconnect_task, "wifi_reconnect", 3072,
                                             NULL, 5, &reconnect_task_handle);
        if (task_result != pdPASS) {
            ESP_ERROR_CHECK(ESP_ERR_NO_MEM);
        }
        return;
    }

    uint8_t mac[6];
    ESP_ERROR_CHECK(esp_wifi_get_mac(WIFI_IF_STA, mac));
    char service_name[13];
    snprintf(service_name, sizeof(service_name), "PROV_%02X%02X%02X", mac[3], mac[4], mac[5]);

    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "No Wi-Fi credentials found; connect to SoftAP %s", service_name);
    ESP_ERROR_CHECK(network_prov_mgr_start_provisioning(
        NETWORK_PROV_SECURITY_1, "abcd1234", service_name, NULL));
}

/**
 * @brief U8X8 I2C communication callback function
 *
 * This function handles all I2C communication between U8G2 library and display controller.
 * Please refer to https://github.com/olikraus/u8g2/wiki/Porting-to-new-MCU-platform for more details.
 *
 * @param[in] u8x8 Pointer to u8x8 structure (not used)
 * @param[in] msg Message type from U8G2 library
 * @param[in] arg_int Integer argument (varies by message type)
 * @param[in] arg_ptr Pointer argument (varies by message type)
 *
 * @return
 *     - 1: Success
 *     - 0: Failure
 */
static uint8_t u8x8_byte_i2c_cb(u8x8_t *u8x8, uint8_t msg,
                                uint8_t arg_int, void *arg_ptr)
{
    static uint8_t buffer[132];  /*!< Enhanced buffer: control byte + 128 data bytes + margin */
    static uint8_t buf_idx;      /*!< Current buffer index */

    switch (msg) {
    case U8X8_MSG_BYTE_INIT:
        /* Add display device to the I2C bus */
        i2c_device_config_t dev_config = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = I2C_DISPLAY_ADDRESS,
            .scl_speed_hz = I2C_FREQ_HZ,
            .scl_wait_us = 0,  /* Use default value */
            .flags.disable_ack_check = false,
        };

        esp_err_t ret = i2c_master_bus_add_device(i2c_bus_handle, &dev_config, &display_dev_handle);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "I2C master driver initialized failed");
            return 0;
        }

        ESP_LOGI(TAG, "I2C master driver initialized successfully");
        break;

    case U8X8_MSG_BYTE_START_TRANSFER:
        /* Start transfer, reset buffer */
        buf_idx = 0;
        break;

    case U8X8_MSG_BYTE_SET_DC:
        /* DC (Data/Command) control - handled by SSD1306 protocol */
        break;

    case U8X8_MSG_BYTE_SEND:
        /* Add data bytes to buffer */
        for (size_t i = 0; i < arg_int; ++i) {
            buffer[buf_idx++] = *((uint8_t*)arg_ptr + i);
        }
        break;

    case U8X8_MSG_BYTE_END_TRANSFER:
        /* Transmit data using new I2C driver */
        if (buf_idx > 0 && display_dev_handle != NULL) {
            esp_err_t ret = i2c_master_transmit(display_dev_handle, buffer, buf_idx, I2C_TIMEOUT_MS);
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "I2C master transmission failed");
                return 0;
            }

            /* Debug output: show transmitted data */
            ESP_LOGD(TAG, "Sent %d bytes to 0x%02X: control_byte=0x%02X",
                     buf_idx, I2C_DISPLAY_ADDRESS, buffer[0]);
        }
        break;

    default:
        return 0;
    }
    return 1;
}

/**
 * @brief U8X8 GPIO control and delay callback function for ESP32
 *
 * This function handles GPIO operations and timing delays required by U8G2 library.
 * Please refer to https://github.com/olikraus/u8g2/wiki/Porting-to-new-MCU-platform for more details.
 *
 * @param[in] u8x8 Pointer to u8x8 structure (not used)
 * @param[in] msg Message type from U8G2 library
 * @param[in] arg_int Integer argument (varies by message type)
 * @param[in] arg_ptr Pointer argument (not used)
 *
 * @return
 *     - 1: Success
 *     - 0: Failure
 */
static uint8_t u8x8_gpio_delay_cb(u8x8_t *u8x8, uint8_t msg,
                                  uint8_t arg_int, void *arg_ptr)
{
    switch (msg) {
    case U8X8_MSG_GPIO_AND_DELAY_INIT:
        ESP_LOGI(TAG, "GPIO and delay initialization completed");
        break;

    case U8X8_MSG_DELAY_MILLI:
        /* Millisecond delay */
        vTaskDelay(pdMS_TO_TICKS(arg_int));
        break;

    case U8X8_MSG_DELAY_10MICRO:
        /* 10 microsecond delay */
        esp_rom_delay_us(arg_int * 10);
        break;

    case U8X8_MSG_DELAY_100NANO:
        /* 100 nanosecond delay - use minimal delay on ESP32 */
        __asm__ __volatile__("nop");
        break;

    case U8X8_MSG_DELAY_I2C:
        /* I2C timing delay: 5us for 100KHz, 1.25us for 400KHz */
        esp_rom_delay_us(5 / arg_int);
        break;

    case U8X8_MSG_GPIO_RESET:
        /* GPIO reset control (optional for most display controllers) */
        break;

    default:
        /* Other GPIO messages not handled */
        return 0;
    }
    return 1;
}

/**
 * @brief Display the current demo cycle number on the display screen.
 *
 * @param u8g2 Pointer to the U8G2 display structure.
 * @param demo_cycle The current demo cycle number to display.
 */
static void show_demo_cycle(u8g2_t* u8g2, int demo_cycle)
{
    u8g2_ClearBuffer(u8g2);
    u8g2_SetFont(u8g2, u8g2_font_ncenB08_tr);
    u8g2_DrawStr(u8g2, 25, 25, "Demo Cycle");
    u8g2_SetFont(u8g2, u8g2_font_ncenB14_tr);
    char cycle_str[16];
    snprintf(cycle_str, sizeof(cycle_str), "%d", demo_cycle);
    u8g2_DrawStr(u8g2, 55, 45, cycle_str);
    u8g2_SendBuffer(u8g2);
    vTaskDelay(pdMS_TO_TICKS(2000));    /* delay for showing static display */
}

void displayString(char* str)
{
    xSemaphoreTakeRecursive(display_mutex, portMAX_DELAY);
    u8g2_ClearBuffer(&u8g2);
    u8g2_SetFont(&u8g2, u8g2_font_ncenB08_tr);
    u8g2_DrawStr(&u8g2, 0, 16, "Security Dev");
    u8g2_DrawStr(&u8g2, 0, 28, connection_ssid);
    u8g2_DrawStr(&u8g2, 0, 40, connection_ip);
    u8g2_DrawStr(&u8g2, 0, 56, str);
    u8g2_SendBuffer(&u8g2);
    xSemaphoreGiveRecursive(display_mutex);
}

/**
 * @brief Main application entry point
 *
 * This function initializes the U8G2 library, configures the display controller,
 * and runs a continuous demo loop showcasing various U8G2 features.
 *
 * The demo includes:
 * - Text display with different fonts
 * - Geometric shapes (rectangles, circles, triangles, lines)
 * - Pixel manipulation
 * - Animated progress bar
 * - Bouncing ball animation
 * - Bitmap display
 */
void app_main(void)
{
    esp_log_level_set("BLE_SENSOR", ESP_LOG_DEBUG);
    ESP_LOGI(TAG, "Starting U8G2 display demo program (menuconfig based configuration)");
    ESP_LOGI(TAG, "I2C Configuration: SDA=GPIO%d, SCL=GPIO%d, Freq=%dHz, Timeout=%dms",
             I2C_MASTER_SDA_IO, I2C_MASTER_SCL_IO, I2C_FREQ_HZ, I2C_TIMEOUT_MS);
    ESP_LOGI(TAG, "Display Configuration: Address=0x%02X",
             I2C_DISPLAY_ADDRESS);

    display_mutex = xSemaphoreCreateRecursiveMutex();
    ESP_ERROR_CHECK(display_mutex != NULL ? ESP_OK : ESP_ERR_NO_MEM);

    i2c_master_bus_config_t bus_config = {
        .i2c_port = I2C_MASTER_NUM,
        .sda_io_num = I2C_MASTER_SDA_IO,
        .scl_io_num = I2C_MASTER_SCL_IO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .intr_priority = 0,
        .trans_queue_depth = 0,  /* 0 = synchronous mode */
        .flags.enable_internal_pullup = true,
    };

    /* Create I2C master bus */
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_config, &i2c_bus_handle));

    /*
     * Initialize U8G2 for your display type.
     * Change the u8g2_Setup_xxx function below to match your display controller,
     * e.g. SSD1306, SH1106, SSD1327, etc.
     * See: https://github.com/olikraus/u8g2/wiki/u8g2setupc
     */
    u8g2_Setup_ssd1306_i2c_128x64_noname_f(
        &u8g2, U8G2_R0,
        u8x8_byte_i2c_cb,   /* I2C communication callback */
        u8x8_gpio_delay_cb  /* GPIO and delay callback */
    );

    /* Initialize display hardware */
    ESP_LOGI(TAG, "Initializing display...");
    u8g2_InitDisplay(&u8g2);
    ESP_LOGI(TAG, "Setting power mode...");
    u8g2_SetPowerSave(&u8g2, 0);  /* Wake up display */
    ESP_LOGI(TAG, "Display initialization completed");

    printf("\n Setting up boot button");
    // file to setup an INTR to clear WiFi NVS storage
    // Boot button is connected to GPIO0 and pressing that, erases the WiFi storage
    // data that contains username/password
    // Learnings from https://github.com/lucadentella/esp32-tutorial
    wifi_eraseconfig(); 

    security_text_display(&u8g2);
    start_wifi_provisioning();
    ESP_ERROR_CHECK(start_configuration_server());

    /* Door sensors are secondary: log a failure rather than halting the device. */
    esp_err_t ble_ret = start_ble_sensor_monitor();
    if (ble_ret != ESP_OK) {
        ESP_LOGE(TAG, "Could not start BLE sensor monitor: %s", esp_err_to_name(ble_ret));
    }
    while(1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

}
