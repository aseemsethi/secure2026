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

#ifdef __cplusplus
}
#endif
