#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "esp_system.h"

#define CONFIG_BUTTON_PIN 0  // for ESP32
//#define CONFIG_BUTTON_PIN 22 // for WEMOS TTGO OLED

#define ERASE_HOLD_TIME_MS 5000
#define BUTTON_POLL_PERIOD_MS 50
#define ERASE_CONFIRMATION_MS 1000

static const char *TAG = "ERASE_CONFIG";

static void erase_task(void *arg)
{
	bool erase_triggered = false;
	TickType_t pressed_at = 0;

	while (true) {
		bool pressed = gpio_get_level(CONFIG_BUTTON_PIN) == 0;

		if (pressed && pressed_at == 0) {
			pressed_at = xTaskGetTickCount();
			ESP_LOGI(TAG, "GPIO0 pressed; hold for %d seconds to erase configuration",
					 ERASE_HOLD_TIME_MS / 1000);
		}

		if (!pressed) {
			if (pressed_at != 0 && !erase_triggered) {
				ESP_LOGI(TAG, "GPIO0 released before erase confirmation");
			}
			pressed_at = 0;
			erase_triggered = false;
		} else if (!erase_triggered &&
				   pdTICKS_TO_MS(xTaskGetTickCount() - pressed_at) >= ERASE_HOLD_TIME_MS) {
			ESP_LOGW(TAG, "GPIO0 held for %d seconds; erasing NVS in %d second",
					 ERASE_HOLD_TIME_MS / 1000, ERASE_CONFIRMATION_MS / 1000);
			vTaskDelay(pdMS_TO_TICKS(ERASE_CONFIRMATION_MS));

			if (gpio_get_level(CONFIG_BUTTON_PIN) == 0) {
				esp_err_t ret = nvs_flash_erase();
				if (ret == ESP_OK) {
					ESP_LOGW(TAG, "NVS erased; restarting device");
					erase_triggered = true;
					esp_restart();
				} else {
					ESP_LOGE(TAG, "NVS erase failed: %s", esp_err_to_name(ret));
					erase_triggered = true;
				}
			} else {
				ESP_LOGI(TAG, "GPIO0 released before erase confirmation");
				pressed_at = 0;
			}
		}

		vTaskDelay(pdMS_TO_TICKS(BUTTON_POLL_PERIOD_MS));
	}
}

void wifi_eraseconfig(void)
{
	ESP_LOGI(TAG, "GPIO0 erase protection enabled; hold button for %d seconds",
			 ERASE_HOLD_TIME_MS / 1000);
	gpio_reset_pin(CONFIG_BUTTON_PIN);
	gpio_set_direction(CONFIG_BUTTON_PIN, GPIO_MODE_INPUT);
	gpio_set_pull_mode(CONFIG_BUTTON_PIN, GPIO_PULLUP_ONLY);
	xTaskCreate(erase_task, "erase_task", 3072, NULL, 10, NULL);
}