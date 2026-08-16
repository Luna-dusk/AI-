#include "key.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "KEY";

static volatile bool wake_request = false;

typedef struct {
    int pin;
    bool last_stable;
    bool last_raw;
    int64_t last_change_us;
} key_slot_t;

static key_slot_t keys[2];

static bool key_raw_pressed(int pin)
{
    return gpio_get_level(pin) == KEY_ACTIVE_LEVEL;
}

static bool key_update_edge(key_slot_t *k)
{
    bool raw = key_raw_pressed(k->pin);
    int64_t now = esp_timer_get_time();

    if (raw != k->last_raw) {
        k->last_raw = raw;
        k->last_change_us = now;
        return false;
    }
    if ((now - k->last_change_us) < (int64_t)KEY_DEBOUNCE_MS * 1000) {
        return false;
    }
    bool edge = (raw && !k->last_stable);
    k->last_stable = raw;
    return edge;
}

static void key_scan_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "key scan start GPIO%d+GPIO%d", keys[0].pin, keys[1].pin);
    while (1) {
        for (int i = 0; i < 2; i++) {
            if (key_update_edge(&keys[i])) {
                wake_request = true;
                ESP_LOGI(TAG, "key GPIO%d pressed", keys[i].pin);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

void key_init(void)
{
    keys[0] = (key_slot_t){ .pin = KEY_GPIO_PIN };
    keys[1] = (key_slot_t){ .pin = BOOT_GPIO_PIN };

    for (int i = 0; i < 2; i++) {
        gpio_config_t cfg = {
            .pin_bit_mask = (1ULL << keys[i].pin),
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = (KEY_ACTIVE_LEVEL == 0) ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE,
            .pull_down_en = (KEY_ACTIVE_LEVEL == 1) ? GPIO_PULLDOWN_ENABLE : GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        gpio_config(&cfg);
        keys[i].last_raw = key_raw_pressed(keys[i].pin);
        keys[i].last_stable = keys[i].last_raw;
        keys[i].last_change_us = esp_timer_get_time();
    }
    wake_request = false;

    static TaskHandle_t h = NULL;
    xTaskCreate(key_scan_task, "key_scan", 4096, NULL, 10, &h);

    ESP_LOGI(TAG, "keys: GPIO%d BOOT=%d (press->GND)", (int)KEY_GPIO_PIN, (int)BOOT_GPIO_PIN);
}

bool key_consume_wake_request(void)
{
    if (wake_request) { wake_request = false; return true; }
    return false;
}
