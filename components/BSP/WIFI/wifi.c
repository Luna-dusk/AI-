#include "wifi.h"
#include "lcd_ui.h"

static EventGroupHandle_t wifi_event_group;
const static char *TAG="wifi";
uint8_t retry_num=1;
uint8_t max_retry_num=100;

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    if(event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START)
    {
        ESP_LOGI(TAG, "WiFi started, connecting to AP...");
        wifi_connect();

    }
    else if(event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED)
    {
        wifi_event_sta_disconnected_t *wifi_event = (wifi_event_sta_disconnected_t *)event_data;
        ESP_LOGI(TAG, "WiFi disconnected, reason: %d", wifi_event->reason);
        ESP_LOGI(TAG, "Attempting to reconnect...");
        if(retry_num<=max_retry_num)
        {
            ESP_LOGI(TAG, "Already Retry %d",retry_num);
            retry_num++;
            wifi_connect();
           
        }
        else
        {
            ESP_LOGI(TAG, "Max retry attempts reached");
            xEventGroupSetBits(wifi_event_group, WIFI_FAIL_BIT);
        }
        
    }
    else if(event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP)
    {
        //event->ip_info.ip为获取的ip
        ip_event_got_ip_t *event = (ip_event_got_ip_t *) event_data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        retry_num=1;
        xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);
        esp_wifi_set_ps(WIFI_PS_NONE);   /* 关省电，减少延迟 */
    }
}

esp_err_t esp_create_wifi_task(void)
{
    BaseType_t ret = xTaskCreate(wifi_init_sta, "wifi_init_sta", 6144, NULL, 5, NULL);
    return (ret == pdPASS) ? ESP_OK : ESP_FAIL;
}

void wifi_init_sta(void *pvParameters)
{
    // 初始化 NVS（WiFi 驱动依赖 NVS 存储校准数据）
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) 
    {
        ESP_LOGW(TAG, "NVS partition needs erase, erasing...");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

   wifi_event_group= xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t wifi_init_config = WIFI_INIT_CONFIG_DEFAULT();

    ESP_ERROR_CHECK(esp_wifi_init(&wifi_init_config));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    //初始化WiFi配置
    wifi_config_t wifi_config={
    .sta={
        .password=WIFI_PASSWORD,
        .ssid=WIFI_ID
        }
    };
    
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL);
    
    EventBits_t bits = xEventGroupWaitBits(wifi_event_group,
                                           WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                           pdFALSE,  /* 不清除 bit，让 app_main 也能等到 */
                                           pdFALSE,  /* 任意一个 bit 就唤醒 */
                                           portMAX_DELAY);
    
    if(bits & WIFI_CONNECTED_BIT)
    {
        ESP_LOGI(TAG, "Connected to AP: %s", WIFI_ID);
    }
    else if(bits & WIFI_FAIL_BIT)
    {
        ESP_LOGI(TAG, "Failed to connect to AP: %s", WIFI_ID);
        vTaskDelete(NULL); 
    }

    while(1)
    {
        vTaskDelay(1000 / portTICK_PERIOD_MS);
    }
}

void wifi_connect()
{
    lcd_ui_set_status(LCD_STATUS_CONNECTING);
    esp_err_t ret=esp_wifi_connect();
    if(ret==ESP_OK)
    {
        ESP_LOGI(TAG,"wifi connect...");
    }
    else
    {
        ESP_LOGI(TAG,"wifi connect failed"); 
    }
}

EventGroupHandle_t wifi_get_event_group(void)
{
    return wifi_event_group;
}