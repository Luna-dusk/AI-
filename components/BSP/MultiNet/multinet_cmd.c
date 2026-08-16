#include "multinet_cmd.h"

#include "esp_log.h"
#include "esp_mn_models.h"
#include "esp_mn_iface.h"
#include "esp_mn_speech_commands.h"
#include "model_path.h"
#include "LED.h"
#include "i2s.h"
#include "lcd_ui.h"

#include <string.h>
#include <stdlib.h>

static const char *TAG = "MNET";

static esp_mn_iface_t *mn_handle = NULL;
static model_iface_data_t *mn_data = NULL;
static bool mn_inited = false;
static multinet_reply_cb_t reply_cb = NULL;

void multinet_cmd_set_reply_cb(multinet_reply_cb_t cb)
{
    reply_cb = cb;
}

static void register_commands(void)
{
    esp_mn_commands_alloc(mn_handle, mn_data);

    /* 同义可多条映射到同一 command_id */
    esp_mn_commands_add(MNET_CMD_LIGHT_ON,  "da kai deng");
    esp_mn_commands_add(MNET_CMD_LIGHT_ON,  "kai deng");
    esp_mn_commands_add(MNET_CMD_LIGHT_OFF, "guan bi deng");
    esp_mn_commands_add(MNET_CMD_LIGHT_OFF, "guan deng");
    esp_mn_commands_add(MNET_CMD_VOL_UP,    "da dian sheng");
    esp_mn_commands_add(MNET_CMD_VOL_DOWN,  "xiao dian sheng");
    esp_mn_commands_add(MNET_CMD_SMILE,     "xiao yi ge");
    esp_mn_commands_add(MNET_CMD_CRY,       "ku yi ge");
    esp_mn_commands_add(MNET_CMD_ANGRY,     "sheng qi lian");

    esp_mn_error_t *err = esp_mn_commands_update();
    if (err) {
        for (int i = 0; i < err->num; i++) {
            ESP_LOGW(TAG, "bad phrase id=%d", err->phrases[i]->command_id);
        }
    } else {
        ESP_LOGI(TAG, "commands registered: %d phrases",
                 MNET_CMD_MAX - 1);
    }
    esp_mn_active_commands_print();
}

esp_err_t multinet_cmd_init(void)
{
    if (mn_inited) return ESP_OK;

    srmodel_list_t *models = esp_srmodel_init("model");
    if (!models) {
        ESP_LOGE(TAG, "esp_srmodel_init failed");
        return ESP_FAIL;
    }

    char *mn_name = esp_srmodel_filter(models, ESP_MN_PREFIX, ESP_MN_CHINESE);
    if (!mn_name) {
        ESP_LOGE(TAG, "no CN multinet model (check menuconfig mn6_cn)");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "MultiNet model: %s", mn_name);

    mn_handle = esp_mn_handle_from_name(mn_name);
    if (!mn_handle) {
        ESP_LOGE(TAG, "esp_mn_handle_from_name failed");
        return ESP_FAIL;
    }

    mn_data = mn_handle->create(mn_name, 6000);
    if (!mn_data) {
        ESP_LOGE(TAG, "create failed");
        return ESP_FAIL;
    }

    register_commands();

    /* 降低检测阈值，提高识别灵敏度（0=最高灵敏度，1=最严格） */
    mn_handle->set_det_threshold(mn_data, 0.3f);

    mn_inited = true;
    ESP_LOGI(TAG, "MultiNet ready, chunk=%d", mn_handle->get_samp_chunksize(mn_data));
    return ESP_OK;
}

void multinet_cmd_deinit(void)
{
    if (mn_data) {
        mn_handle->destroy(mn_data);
        mn_data = NULL;
    }
    mn_inited = false;
}

int multinet_cmd_feed(const int16_t *pcm)
{
    if (!mn_inited || !mn_handle || !mn_data || !pcm) return 0;

    esp_mn_state_t st = mn_handle->detect(mn_data, (int16_t *)pcm);

    if (st == ESP_MN_STATE_DETECTED) {
        esp_mn_results_t *r = mn_handle->get_results(mn_data);
        if (r && r->num > 0) {
            ESP_LOGI(TAG, "DETECTED cmd_id=%d prob=%.2f phrase=%d",
                     r->command_id[0], r->prob[0], r->phrase_id[0]);
            return r->command_id[0];
        }
    }
    if (st == ESP_MN_STATE_TIMEOUT) {
        ESP_LOGI(TAG, "TIMEOUT");
        return -1;
    }
    return 0; /* DETECTING */
}

void multinet_cmd_reset(void)
{
    if (mn_data && mn_handle) {
        mn_handle->clean(mn_data);
    }
}

void multinet_cmd_exec(int cmd_id)
{
    switch (cmd_id) {
    case MNET_CMD_LIGHT_ON:
        ESP_LOGI(TAG, "EXEC: LED ON");
        LED(1);
        break;
    case MNET_CMD_LIGHT_OFF:
        ESP_LOGI(TAG, "EXEC: LED OFF");
        LED(0);
        break;
    case MNET_CMD_VOL_UP: {
        float v = i2s_spk_get_volume() + 0.1f;
        i2s_spk_set_volume(v);
        ESP_LOGI(TAG, "EXEC: VOL+ -> %.2f", (double)i2s_spk_get_volume());
        break;
    }
    case MNET_CMD_VOL_DOWN: {
        float v = i2s_spk_get_volume() - 0.1f;
        i2s_spk_set_volume(v);
        ESP_LOGI(TAG, "EXEC: VOL- -> %.2f", (double)i2s_spk_get_volume());
        break;
    }
    case MNET_CMD_SMILE:
        ESP_LOGI(TAG, "EXEC: SMILE");
        lcd_ui_set_emotion(LCD_EMO_SMILE);
        break;
    case MNET_CMD_CRY:
        ESP_LOGI(TAG, "EXEC: CRY");
        lcd_ui_set_emotion(LCD_EMO_CRY);
        break;
    case MNET_CMD_ANGRY:
        ESP_LOGI(TAG, "EXEC: ANGRY");
        lcd_ui_set_emotion(LCD_EMO_ANGRY);
        break;
    default:
        ESP_LOGW(TAG, "unknown cmd %d", cmd_id);
        break;
    }

    /* 语音回复：由 main 注册的回调从服务器拉取对应 TTS 并播放 */
    if (reply_cb) {
        reply_cb(cmd_id);
    }
}

int multinet_cmd_chunksize(void)
{
    if (mn_data && mn_handle) {
        return mn_handle->get_samp_chunksize(mn_data);
    }
    return 0;
}
