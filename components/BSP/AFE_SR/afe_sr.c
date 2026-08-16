#include "afe_sr.h"
#include "multinet_cmd.h"
#include "esp_wn_models.h"
#include "esp_wn_iface.h"

static const char *TAG = "AFE_SR";

static const esp_afe_sr_iface_t *afe_handle = NULL;
static esp_afe_sr_data_t *afe_data = NULL;
static QueueHandle_t pcm_queue = NULL;
static TaskHandle_t feed_task = NULL;
static TaskHandle_t fetch_task = NULL;

/*
 * esp-sr 库不是多线程安全的：feed()（含 WakeNet/VAD 推理）在 feed 任务（核1）
 * 与 fetch()/multinet 命令识别（TIE DSP，RNNT）在 fetch 任务（核0）并发执行时
 * 会死锁（dl_tie728_memcpy 卡死、中断关闭 -> WDT 硬复位，GDB 已证实）。
 * 所有 esp-sr 调用必须用本锁串行化。
 */
static SemaphoreHandle_t afe_lock = NULL;
/* feed 任务每喂入一帧就 give 一次，fetch 任务取到信号后才 fetch()。
 * 目的：让 fetch 以 feed 的节奏运行，而不是在持锁状态下长时间阻塞等待
 * 输出数据（esp-sr fetch() 默认超时 ~200ms，持锁阻塞会饿死 feed()）。 */
static SemaphoreHandle_t fetch_sem = NULL;

#define AFE_LOCK()   do { if (afe_lock) xSemaphoreTake(afe_lock, portMAX_DELAY); } while (0)
#define AFE_UNLOCK() do { if (afe_lock) xSemaphoreGive(afe_lock); } while (0)

static int16_t tail[AUDIO_FRAME_SAMPLES];
static size_t tail_samples = 0;
/* false=上传/播 TTS 中，忽略 VAD，避免喇叭声被当成用户说话 */
static volatile bool capture_enabled = true;
static volatile int capture_warmup = 0;
static volatile TickType_t session_last_activity = 0;   /* 会话最后活动 tick */

typedef enum {
    SR_IDLE = 0,       /* 休眠：只等唤醒词/按键 */
    SR_LISTENING = 1,  /* 会话：持续多轮 VAD，不需重复唤醒 */
} sr_state_t;

/* fetch 任务当前状态；供 main 同步 LCD（待机/聆听）。只在 fetch 任务里写 */
static volatile sr_state_t afe_cur_state = SR_IDLE;

bool afe_sr_in_session(void)
{
    return afe_cur_state == SR_LISTENING;
}

void afe_sr_set_capture_enabled(bool enabled)
{
    bool was_enabled = capture_enabled;
    capture_enabled = enabled;
    if (was_enabled != enabled) {
        ESP_LOGI(TAG, "capture %s", enabled ? "ON" : "OFF");
    }
    if (!enabled) {
        tail_samples = 0;
    } else {
        /* 复位 AFE 内部缓冲，丢弃 TTS 播放期间累积的回声 */
        if (afe_handle && afe_data) {
            AFE_LOCK();
            afe_handle->reset_buffer(afe_data);
            AFE_UNLOCK();
        }
        capture_warmup = 30;   /* ~1 s 冷却 */
        session_last_activity = xTaskGetTickCount();  /* 刷新会话活动时间 */
    }
}

static bool queue_send_frame(const audio_frame_t *frame)
{
    if (pcm_queue == NULL || frame == NULL) {
        return false;
    }
    /* Keep one queue slot for AUDIO_FRAME_END so a slow network cannot stall
     * the AFE state machine in the capture-disabled state. */
    if (frame->type == AUDIO_FRAME_DATA && uxQueueSpacesAvailable(pcm_queue) <= 1) {
        ESP_LOGW(TAG, "pcm queue reserved END slot, drop data");
        return false;
    }
    if (xQueueSend(pcm_queue, frame, 0) != pdTRUE) {
        ESP_LOGW(TAG, "pcm queue full, drop type=%d", (int)frame->type);
        return false;
    }
    return true;
}

static bool send_end_frame(void)
{
    audio_frame_t frame;
    memset(&frame, 0, sizeof(frame));
    frame.type = AUDIO_FRAME_END;
    frame.bytes = 0;
    if (xQueueSend(pcm_queue, &frame, 0) != pdTRUE) {
        ESP_LOGW(TAG, "send END_FRAME failed");
        return false;
    }
    return true;
}

static void push_pcm_bytes(const int16_t *pcm, int pcm_bytes)
{
    if (pcm == NULL || pcm_bytes <= 0) {
        return;
    }

    const int16_t *src = pcm;
    size_t samples_left = (size_t)pcm_bytes / sizeof(int16_t);

    while (samples_left > 0) {
        size_t need = AUDIO_FRAME_SAMPLES - tail_samples;
        size_t take = (samples_left < need) ? samples_left : need;

        memcpy(&tail[tail_samples], src, take * sizeof(int16_t));
        tail_samples += take;
        src += take;
        samples_left -= take;

        if (tail_samples == AUDIO_FRAME_SAMPLES) {
            audio_frame_t frame;
            memset(&frame, 0, sizeof(frame));
            frame.type = AUDIO_FRAME_DATA;
            frame.bytes = AUDIO_FRAME_BYTES;
            memcpy(frame.data, tail, AUDIO_FRAME_BYTES);
            queue_send_frame(&frame);
            tail_samples = 0;
        }
    }
}

static void drain_pcm_queue(void)
{
    if (pcm_queue == NULL) return;
    audio_frame_t junk;
    while (xQueueReceive(pcm_queue, &junk, 0) == pdTRUE) {
    }
}

/* 休眠：开 WakeNet，等下次唤醒 */
static void enter_idle(esp_afe_sr_data_t *data, bool *in_speech, int *cur_hangover)
{
    ESP_LOGW(TAG, "enter_idle() called");
    *in_speech = false;
    *cur_hangover = 0;
    session_last_activity = xTaskGetTickCount();
    tail_samples = 0;
    afe_cur_state = SR_IDLE;
    if (afe_handle && data) {
        AFE_LOCK();
        afe_handle->enable_wakenet(data);
        AFE_UNLOCK();
    }
    ESP_LOGI(TAG, "state -> SLEEP (wait 你好小鑫 or KEY->GND on GPIO%d)",
             (int)KEY_GPIO_PIN);
    lcd_ui_set_status(LCD_STATUS_STANDBY);
    lcd_ui_set_emotion(LCD_EMO_DROOL);   /* 待机：流口水脸 */
}

/* 进入会话：关 WakeNet，持续多轮聆听 */
static void enter_listening(esp_afe_sr_data_t *data, sr_state_t *state,
                            bool *in_speech, int *cur_hangover,
                            const char *reason)
{
    TickType_t now = xTaskGetTickCount();
    ESP_LOGI(TAG, "wake by %s -> SESSION (tick=%u, timeout=%u)",
             reason ? reason : "?", (unsigned)now,
             (unsigned)pdMS_TO_TICKS(SESSION_IDLE_TIMEOUT_MS));
    if (afe_handle && data) {
        AFE_LOCK();
        afe_handle->disable_wakenet(data);
        AFE_UNLOCK();
    }
    *state = SR_LISTENING;
    afe_cur_state = SR_LISTENING;
    *in_speech = false;
    *cur_hangover = 0;
    session_last_activity = now;
    tail_samples = 0;
    multinet_cmd_reset();
    lcd_ui_set_status(LCD_STATUS_LISTENING);
    lcd_ui_set_emotion(LCD_EMO_NORMAL);   /* 唤醒：流口水脸回正常脸 */
    ESP_LOGI(TAG, "state -> SESSION, capture=%s", capture_enabled ? "ON" : "OFF");
}

static void afe_feed_task(void *arg)
{
    esp_afe_sr_data_t *data = (esp_afe_sr_data_t *)arg;
    int chunksize = afe_handle->get_feed_chunksize(data);
    int nch = afe_handle->get_feed_channel_num(data);
    size_t bytes = (size_t)chunksize * (size_t)nch * sizeof(int16_t);

    ESP_LOGI(TAG, "feed: chunksize=%d nch=%d bytes=%u", chunksize, nch, (unsigned)bytes);

    int16_t *buf = (int16_t *)heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (buf == NULL) {
        buf = (int16_t *)malloc(bytes);
    }
    if (buf == NULL) {
        ESP_LOGE(TAG, "feed buffer alloc failed");
        vTaskDelete(NULL);
        return;
    }

    while (1) {
        size_t real_bytes = 0;
        if (receive_audio(buf, bytes, &real_bytes, portMAX_DELAY) != ESP_OK) {
            continue;
        }
        if (real_bytes < bytes) {
            continue;
        }
        AFE_LOCK();
        afe_handle->feed(data, buf);          /* 始终喂，不中断 */
        AFE_UNLOCK();
        if (fetch_sem) {
            xSemaphoreGive(fetch_sem);
        }
    }
}

static void afe_fetch_task(void *arg)
{
    esp_afe_sr_data_t *data = (esp_afe_sr_data_t *)arg;
    sr_state_t state = SR_IDLE;
    bool in_speech = false;
    int cur_hangover = 0;
    const int hangover_frames = CONTINUE_FRAME_TIME;
    const TickType_t idle_timeout_ticks = pdMS_TO_TICKS(SESSION_IDLE_TIMEOUT_MS);
    int local_cmd_cooldown = 0;    /* 本地命令命中后短暂抑制 VAD，避免同一句话被上传 */

    ESP_LOGI(TAG, "fetch task start (session mode, wake=你好小鑫)");
    enter_idle(data, &in_speech, &cur_hangover);

    while (1) {
        /* 等 feed 任务喂入新数据后再 fetch，避免在持锁状态下阻塞等待输出
         * （esp-sr fetch() 空 ringbuffer 时会阻塞并打印 "Ringbuffer of AFE is
         * empty"；持 afe_lock 阻塞会饿死 feed 任务，导致整条链路停摆）。 */
        if (fetch_sem == NULL || xSemaphoreTake(fetch_sem, pdMS_TO_TICKS(50)) != pdTRUE) {
            /* 超时（feed 停摆时）仍检查按键，避免卡死无法唤醒 */
            if (state == SR_IDLE && key_consume_wake_request()) {
                enter_listening(data, &state, &in_speech, &cur_hangover, "KEY");
            }
            continue;
        }

        /* 一次 feed 可能产生多个可 fetch 的结果，全部排空（非阻塞、不持锁久等）。
         * 最多取 4 次，避免排空末尾的空结果触发 esp-sr 库内警告。 */
        static TickType_t last_empty_tick = 0;
        int drain_count = 0;
        for (;;) {
            if (++drain_count > 4) {
                break;
            }
            /* 上一次排空到空不足 1 秒时跳过本次 fetch，避免库内警告刷屏 */
            if (last_empty_tick && (xTaskGetTickCount() - last_empty_tick) < pdMS_TO_TICKS(1000)) {
                break;
            }
            AFE_LOCK();
            afe_fetch_result_t *res = afe_handle->fetch_with_delay(data, 0);
            AFE_UNLOCK();
            if (res == NULL || res->ret_value == ESP_FAIL) {
                last_empty_tick = xTaskGetTickCount();
                /* 当前已无可取数据：检查按键后回到等 feed */
                if (state == SR_IDLE && key_consume_wake_request()) {
                    enter_listening(data, &state, &in_speech, &cur_hangover, "KEY");
                }
                break;
            }

            if (state == SR_IDLE) {
                if (res->wakeup_state == WAKENET_DETECTED ||
                    res->wakeup_state == WAKENET_CHANNEL_VERIFIED) {
                    enter_listening(data, &state, &in_speech, &cur_hangover, "WakeNet");
                    continue;
                }
                if (key_consume_wake_request()) {
                    enter_listening(data, &state, &in_speech, &cur_hangover, "KEY");
                }
                continue;
            }

            /* ========== SESSION：多轮对话 + 本地命令 ========== */

            /* 上传/播报中：丢弃 VAD，防止扬声器回灌导致自言自语 */
            if (!capture_enabled) {
                if (in_speech) {
                    in_speech = false;
                    cur_hangover = 0;
                    tail_samples = 0;
                }
                continue;
            }

            /* 刚恢复采集：冷却期跳过 VAD，消除扬声器余振 */
            if (capture_warmup > 0) {
                capture_warmup--;
                continue;
            }

            /* --- 本地命令检测（优先） --- */
            if (res->data != NULL && res->data_size > 0) {
                AFE_LOCK();
                int cmd = multinet_cmd_feed(res->data);
                AFE_UNLOCK();
                if (cmd > 0) {
                    ESP_LOGI(TAG, "local cmd=%d hit, drain queue & cooldown 1.5s", cmd);
                    multinet_cmd_exec(cmd);
                    AFE_LOCK();
                    multinet_cmd_reset();
                    AFE_UNLOCK();
                    drain_pcm_queue();
                    in_speech = false;
                    cur_hangover = 0;
                    tail_samples = 0;
                    local_cmd_cooldown = 50;
                    session_last_activity = xTaskGetTickCount();
                    /* 关闭采集：TTS 回复播放期间避免麦克风回灌触发 VAD 上传 */
                    capture_enabled = false;
                    lcd_ui_set_status(LCD_STATUS_REPLYING);
                    continue;
                } else if (cmd == -1) {
                    AFE_LOCK();
                    multinet_cmd_reset();
                    AFE_UNLOCK();
                }
            }

            /* 本地命令冷却期：继续 MultiNet，抑制 VAD 上传 */
            if (local_cmd_cooldown > 0) {
                local_cmd_cooldown--;
                continue;
            }

            if (res->vad_state == VAD_SPEECH) {
                cur_hangover = 0;
                session_last_activity = xTaskGetTickCount();

                if (!in_speech) {
                    ESP_LOGI(TAG, "VAD speech start");
                    tail_samples = 0;
                    if (res->vad_cache_size > 0 && res->vad_cache != NULL) {
                        push_pcm_bytes(res->vad_cache, res->vad_cache_size);
                    }
                    in_speech = true;
                }

                if (res->data != NULL && res->data_size > 0) {
                    push_pcm_bytes(res->data, res->data_size);
                }
            } else {
                if (in_speech) {
                    cur_hangover++;
                    if (res->data != NULL && res->data_size > 0) {
                        push_pcm_bytes(res->data, res->data_size);
                    }
                    if (cur_hangover >= hangover_frames) {
                        ESP_LOGI(TAG, "utterance end, stay SESSION (capture OFF until TTS done)");
                        tail_samples = 0;
                        bool end_queued = send_end_frame();
                        in_speech = false;
                        cur_hangover = 0;
                        session_last_activity = xTaskGetTickCount();
                        if (!end_queued) {
                            ESP_LOGW(TAG, "utterance aborted: END_FRAME unavailable");
                            capture_enabled = true;
                            lcd_ui_set_status(LCD_STATUS_LISTENING);
                            continue;
                        }
                        /* 立刻静音采集，等 main 播完 TTS 再打开 */
                        capture_enabled = false;
                        lcd_ui_set_status(LCD_STATUS_REPLYING);
                    }
                } else {
                    /* 无语音：检查 5 分钟会话空闲 */
                    TickType_t now = xTaskGetTickCount();
                    TickType_t diff = now - session_last_activity;
                    if (diff >= idle_timeout_ticks) {
                        ESP_LOGW(TAG, "session idle -> SLEEP (now=%u last=%u diff=%u timeout=%u)",
                                 (unsigned)now, (unsigned)session_last_activity,
                                 (unsigned)diff, (unsigned)idle_timeout_ticks);
                        state = SR_IDLE;
                        capture_enabled = true;
                        enter_idle(data, &in_speech, &cur_hangover);
                    }
                }
            }
        }
    }
}

esp_err_t afe_init(void)
{
    if (afe_data != NULL) {
        return ESP_OK;
    }

    srmodel_list_t *models = esp_srmodel_init("model");
    if (models == NULL) {
        ESP_LOGE(TAG, "esp_srmodel_init failed (check model partition)");
        return ESP_FAIL;
    }

    afe_config_t *afe_config = afe_config_init("M", models, AFE_TYPE_SR, AFE_MODE_HIGH_PERF);
    if (afe_config == NULL) {
        ESP_LOGE(TAG, "afe_config_init failed");
        return ESP_FAIL;
    }

    afe_config->aec_init = false;
    afe_config->wakenet_init = true;

    /* 顺便初始化 LED 和本地命令 */
    LED_Init();
    LED(0);
    multinet_cmd_init();

    char *wake_name = esp_srmodel_filter(models, ESP_WN_PREFIX, "nihaoxiaoxin");
    if (wake_name == NULL) {
        wake_name = esp_srmodel_filter(models, ESP_WN_PREFIX, NULL);
    }
    if (wake_name != NULL) {
        afe_config->wakenet_model_name = wake_name;
        ESP_LOGI(TAG, "WakeNet model: %s", wake_name);
    } else {
        ESP_LOGW(TAG, "no WakeNet model in partition, check menuconfig NIHAOXIAOXIN");
    }

    afe_handle = esp_afe_handle_from_config(afe_config);
    if (afe_handle == NULL) {
        ESP_LOGE(TAG, "esp_afe_handle_from_config failed");
        afe_config_free(afe_config);
        return ESP_FAIL;
    }

    afe_data = afe_handle->create_from_config(afe_config);
    afe_config_free(afe_config);

    if (afe_data == NULL) {
        ESP_LOGE(TAG, "create_from_config failed");
        afe_handle = NULL;
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "AFE init ok (WakeNet on, session mode)");
    return ESP_OK;
}
esp_err_t afe_sr_start(QueueHandle_t queue)
{
    if (afe_data == NULL || afe_handle == NULL) {
        ESP_LOGE(TAG, "call afe_init first");
        return ESP_ERR_INVALID_STATE;
    }
    if (queue == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (feed_task != NULL) {
        return ESP_OK;
    }

    key_init();
    pcm_queue = queue;
    tail_samples = 0;

    if (afe_lock == NULL) {
        afe_lock = xSemaphoreCreateMutex();
        if (afe_lock == NULL) {
            ESP_LOGE(TAG, "create afe lock failed");
            return ESP_ERR_NO_MEM;
        }
    }

    if (fetch_sem == NULL) {
        fetch_sem = xSemaphoreCreateBinary();
        if (fetch_sem == NULL) {
            ESP_LOGE(TAG, "create fetch sem failed");
            return ESP_ERR_NO_MEM;
        }
    }

    if (xTaskCreatePinnedToCore(afe_feed_task, "afe_feed", 8 * 1024,
                                afe_data, 21, &feed_task, 1) != pdPASS) {
        ESP_LOGE(TAG, "create feed task failed");
        feed_task = NULL;
        return ESP_ERR_NO_MEM;
    }

    if (xTaskCreatePinnedToCore(afe_fetch_task, "afe_fetch", 8 * 1024,
                                afe_data, 20, &fetch_task, 0) != pdPASS) {
        ESP_LOGE(TAG, "create fetch task failed");
        afe_sr_stop();
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "AFE tasks started");
    return ESP_OK;
}

void afe_sr_stop(void)
{
    
}
