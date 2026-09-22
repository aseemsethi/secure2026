#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Start scanning for the IM24-BLE door sensors stored in configuration.
 *
 * Reads the Bluetooth addresses saved through the configuration web page,
 * starts the NimBLE stack in observer mode, and creates a task that reports
 * door open and close transitions on the display and through ntfy.
 *
 * Call after nvs_flash_init() has run.
 *
 * @return
 *     - ESP_OK: monitor started, or no sensors are configured
 *     - ESP_ERR_NO_MEM: queue or task could not be created
 *     - otherwise: the error reported by the NimBLE stack
 */
esp_err_t start_ble_sensor_monitor(void);

#ifdef __cplusplus
}
#endif
