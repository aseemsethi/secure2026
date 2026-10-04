#include "mems_mic.h"

#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "http_server.h"

/* Provided by main.c, declared here as http_server.c already does for displayString. */
void displayString(char *str);
esp_err_t send_ntfy_notification(const char *topic, const char *message);

static const char *TAG = "MEMS_MIC";

/*
 * INMP441 wiring. GPIO 47 does not exist on the original ESP32, so capture is
 * built for the S3 only; add a pin set here to bring up another board.
 *
 *   VDD -> 3V3        SCK (BCLK) -> GPIO 47
 *   GND -> GND        WS  (LRCL) -> GPIO 10
 *   L/R -> GND        SD  (DOUT) -> GPIO 21
 *
 * L/R tied low puts the microphone on the left slot, which is why the slot mask
 * below is I2S_STD_SLOT_LEFT.
 */
#if CONFIG_IDF_TARGET_ESP32S3
#define MIC_SUPPORTED 1
#define MIC_SCK_GPIO  GPIO_NUM_47
#define MIC_WS_GPIO   GPIO_NUM_10
#define MIC_SD_GPIO   GPIO_NUM_21
#else
#define MIC_SUPPORTED 0
#endif

/*
 * The ntfy topic the help alert is published to.
 *
 * NOTE: ntfy.sh topics are public and unauthenticated. "help" is a name other
 * people certainly already use, so alerts sent here are readable by strangers
 * and strangers can publish fake ones. Change this to something unguessable
 * before relying on it.
 */
#define HELP_NTFY_TOPIC "help"

/**
 * @brief Compose the help alert text, naming the device that heard it.
 *
 * Shared by the capture path and the unsupported-target stub. With two of these
 * devices running, an alert that does not say which one called out is useless.
 *
 * @param[out] out         Receives the message
 * @param[in]  out_size    Size of the buffer
 * @param[in]  level_dbfs  Measured level, or NULL to leave it out
 */
static void build_help_message(char *out, size_t out_size, const float *level_dbfs)
{
    char device_name[CONFIG_SERVER_TEXT_LENGTH];
    bool named = get_config_device_name(device_name, sizeof(device_name)) == ESP_OK &&
                 device_name[0] != '\0';

    if (named && level_dbfs != NULL) {
        snprintf(out, out_size, "Help (%s) called out at %.0f dBFS",
                 device_name, (double)*level_dbfs);
    } else if (named) {
        snprintf(out, out_size, "Help (%s) called out", device_name);
    } else if (level_dbfs != NULL) {
        snprintf(out, out_size, "Help called out at %.0f dBFS", (double)*level_dbfs);
    } else {
        snprintf(out, out_size, "Help called out");
    }
}

#if MIC_SUPPORTED

#include "driver/i2s_std.h"

#define MIC_SAMPLE_RATE    16000   /* Plenty for speech, and cheap to process */
#define MIC_FRAME_SAMPLES  512     /* 32 ms per frame at 16 kHz */
#define MIC_FULL_SCALE     8388608.0  /* 2^23: the INMP441 is a 24-bit part */

/*
 * Trigger shape: the level has to stay above the threshold for several frames
 * in a row, so a door slam or a handclap does not fire it. These numbers are a
 * starting point, not a measurement. Watch the level lines in the log and move
 * HELP_DBFS_THRESHOLD to sit between your room's background and a real shout.
 */
#define HELP_DBFS_THRESHOLD  (-18.0f)
#define HELP_HOLD_FRAMES     8        /* ~256 ms of sustained sound */
#define HELP_COOLDOWN_MS     15000    /* One alert per quarter minute at most */
#define MIC_LEVEL_REPORT_MS  5000

static i2s_chan_handle_t rx_handle;
static volatile float last_level_dbfs = -180.0f;
static int64_t last_trigger_us;

/**
 * @brief RMS level of one frame, in dBFS.
 *
 * The INMP441 sends 24 bits left aligned inside a 32-bit slot, so each sample
 * is shifted down by 8 before being scaled against full scale.
 */
static float frame_dbfs(const int32_t *frame, size_t count)
{
    if (count == 0) {
        return -180.0f;
    }

    double sum_squares = 0.0;
    for (size_t i = 0; i < count; ++i) {
        double sample = (double)(frame[i] >> 8) / MIC_FULL_SCALE;
        sum_squares += sample * sample;
    }

    double rms = sqrt(sum_squares / (double)count);
    if (rms < 1e-9) {
        return -180.0f;
    }
    return (float)(20.0 * log10(rms));
}

static void raise_help_alert(float level_dbfs)
{
    ESP_LOGW(TAG, "Help called out at %.1f dBFS", (double)level_dbfs);
    displayString("HELP called out");

    char message[96];
    build_help_message(message, sizeof(message), &level_dbfs);

    esp_err_t ret = send_ntfy_notification(HELP_NTFY_TOPIC, message);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Could not queue help notification: %s", esp_err_to_name(ret));
    }
}

static void mic_task(void *argument)
{
    size_t frame_bytes = MIC_FRAME_SAMPLES * sizeof(int32_t);
    int32_t *frame = malloc(frame_bytes);
    if (frame == NULL) {
        ESP_LOGE(TAG, "Could not allocate the capture frame");
        vTaskDelete(NULL);
        return;
    }

    int loud_frames = 0;
    float window_peak = -180.0f;
    double window_sum = 0.0;
    int window_count = 0;
    int64_t next_report_us = esp_timer_get_time() + (int64_t)MIC_LEVEL_REPORT_MS * 1000;

    while (true) {
        size_t bytes_read = 0;
        esp_err_t ret = i2s_channel_read(rx_handle, frame, frame_bytes, &bytes_read, 1000);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "I2S read failed: %s", esp_err_to_name(ret));
            continue;
        }

        float level = frame_dbfs(frame, bytes_read / sizeof(int32_t));
        last_level_dbfs = level;

        window_sum += level;
        window_count++;
        if (level > window_peak) {
            window_peak = level;
        }

        /* Sustained, not momentary: a single loud frame resets the run. */
        loud_frames = (level >= HELP_DBFS_THRESHOLD) ? loud_frames + 1 : 0;

        if (loud_frames >= HELP_HOLD_FRAMES) {
            loud_frames = 0;
            int64_t now_us = esp_timer_get_time();
            if (now_us - last_trigger_us >= (int64_t)HELP_COOLDOWN_MS * 1000) {
                last_trigger_us = now_us;
                raise_help_alert(level);
            }
        }

        int64_t now_us = esp_timer_get_time();
        if (now_us >= next_report_us) {
            ESP_LOGI(TAG, "level avg %.1f dBFS, peak %.1f dBFS (trigger at %.1f)",
                     window_count ? window_sum / window_count : -180.0,
                     (double)window_peak, (double)HELP_DBFS_THRESHOLD);
            window_sum = 0.0;
            window_count = 0;
            window_peak = -180.0f;
            next_report_us = now_us + (int64_t)MIC_LEVEL_REPORT_MS * 1000;
        }
    }
}

esp_err_t start_mems_mic(void)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    esp_err_t ret = i2s_new_channel(&chan_cfg, NULL, &rx_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Could not create the I2S channel: %s", esp_err_to_name(ret));
        return ret;
    }

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(MIC_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT,
                                                        I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = MIC_SCK_GPIO,
            .ws   = MIC_WS_GPIO,
            .dout = I2S_GPIO_UNUSED,
            .din  = MIC_SD_GPIO,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv   = false,
            },
        },
    };
    std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;  /* L/R is tied to GND */

    ret = i2s_channel_init_std_mode(rx_handle, &std_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Could not configure I2S: %s", esp_err_to_name(ret));
        i2s_del_channel(rx_handle);
        rx_handle = NULL;
        return ret;
    }

    ret = i2s_channel_enable(rx_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Could not enable I2S: %s", esp_err_to_name(ret));
        i2s_del_channel(rx_handle);
        rx_handle = NULL;
        return ret;
    }

    if (xTaskCreate(mic_task, "mems_mic", 4096, NULL, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Could not create the microphone task");
        i2s_channel_disable(rx_handle);
        i2s_del_channel(rx_handle);
        rx_handle = NULL;
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "INMP441 listening at %d Hz on SCK=%d WS=%d SD=%d",
             MIC_SAMPLE_RATE, MIC_SCK_GPIO, MIC_WS_GPIO, MIC_SD_GPIO);
    return ESP_OK;
}

float mems_mic_level_dbfs(void)
{
    return last_level_dbfs;
}

void mems_mic_trigger_help(void)
{
    last_trigger_us = esp_timer_get_time();
    raise_help_alert(last_level_dbfs);
}

#else  /* !MIC_SUPPORTED */

esp_err_t start_mems_mic(void)
{
    ESP_LOGW(TAG, "No INMP441 pins defined for this target; microphone not started");
    return ESP_OK;
}

float mems_mic_level_dbfs(void)
{
    return -180.0f;
}

void mems_mic_trigger_help(void)
{
    displayString("HELP called out");

    char message[96];
    build_help_message(message, sizeof(message), NULL);

    esp_err_t ret = send_ntfy_notification(HELP_NTFY_TOPIC, message);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Could not queue help notification: %s", esp_err_to_name(ret));
    }
}

#endif /* MIC_SUPPORTED */
