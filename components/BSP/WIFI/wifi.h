#ifndef __WIFI_H
#define __WIFI_H

#include "esp_wifi.h"
#include "esp_event.h"
#include "FreeRTOS/task.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_http_server.h"
#include "FreeRTOS/event_groups.h"
#include "esp_err.h"


#define WIFI_ID "月色真美" //建议使用英文
#define WIFI_PASSWORD "zhaohuaxishi"//密码长度：8~63位

#define WIFI_CONNECTED_BIT BIT(0)
#define WIFI_FAIL_BIT      BIT(1)

esp_err_t esp_create_wifi_task(void);
void wifi_init_sta(void *pvParameters);
void wifi_connect(void);

/* 获取 WiFi 事件组句柄，供其他模块等待连接就绪 */
EventGroupHandle_t wifi_get_event_group(void);
#endif
