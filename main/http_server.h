#pragma once

#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CONFIG_SERVER_MAX_BT_ADDRESSES 8
#define CONFIG_SERVER_TEXT_LENGTH 16
#define CONFIG_SERVER_BT_ADDRESS_LENGTH 18
#define CONFIG_SERVER_BT_NAME_LENGTH 16
#define CONFIG_SERVER_PHONE_LENGTH 21
#define CONFIG_SERVER_TOPIC_LENGTH 13

esp_err_t start_configuration_server(void);
esp_err_t get_config_topic(char *topic, size_t topic_size);

/**
 * @brief Read one configured Bluetooth sensor slot from flash.
 *
 * @param[in]  index         Slot index, 0 to CONFIG_SERVER_MAX_BT_ADDRESSES - 1
 * @param[out] address       Receives "AA:BB:CC:DD:EE:FF", empty when unset
 * @param[in]  address_size  Size of the address buffer
 * @param[out] name          Receives the friendly name, empty when unset
 * @param[in]  name_size     Size of the name buffer
 */
esp_err_t get_config_bt_sensor(int index, char *address, size_t address_size,
                               char *name, size_t name_size);

#ifdef __cplusplus
}
#endif
