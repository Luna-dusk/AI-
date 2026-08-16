#ifndef __KEY_H
#define __KEY_H

#include "driver/gpio.h"
#include <stdbool.h>

#define KEY_GPIO_PIN      GPIO_NUM_2
#define BOOT_GPIO_PIN     GPIO_NUM_0
#define KEY_ACTIVE_LEVEL  0
#define KEY_DEBOUNCE_MS   30

void key_init(void);
bool key_consume_wake_request(void);

#endif
