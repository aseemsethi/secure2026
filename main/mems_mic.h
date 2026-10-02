#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Start listening to the INMP441 microphone.
 *
 * Brings up I2S in standard Philips mode and creates a task that measures the
 * sound level continuously. A sustained loud sound raises a help alert on the
 * display and through ntfy.
 *
 * Only the ESP32-S3 is wired for this; on other targets it logs and does
 * nothing, so the call is safe to make unconditionally.
 *
 * @return ESP_OK on success, otherwise the error from the I2S driver.
 */
esp_err_t start_mems_mic(void);

/**
 * @brief Most recent sound level in dBFS, for tuning the trigger threshold.
 *
 * Returns -180.0 before the first frame has been captured.
 */
float mems_mic_level_dbfs(void);

/**
 * @brief Raise a help alert directly, without needing a sound.
 *
 * Lets the display and ntfy path be tested before the audio threshold is tuned.
 */
void mems_mic_trigger_help(void);

#ifdef __cplusplus
}
#endif
