#include "lcd_ui.h"
#include "lcd.h"
#include "face_data.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_lcd_panel_ops.h"

#include "lvgl.h"

/* SimSun CJK 字体（CONFIG_LV_FONT_SIMSUN_16_CJK=y 时可用） */
extern const lv_font_t lv_font_simsun_16_cjk;
/* 自定义 18px 宋体，覆盖状态栏所需汉字 */
extern const lv_font_t lv_font_cn_18;

static const char *TAG = "LCD_UI";

static esp_lcd_panel_handle_t panel = NULL;
static lv_disp_draw_buf_t disp_buf;
static lv_color_t *buf1 = NULL;
static lv_color_t *buf2 = NULL;
static lv_disp_drv_t disp_drv;

/* UI 元素 */
static lv_obj_t *status_bar = NULL;
static lv_obj_t *status_label = NULL;
static lv_obj_t *face_imgs[5] = {NULL};

/* 线程间通知 */
static volatile lcd_emotion_t pending_emotion = LCD_EMO_NORMAL;
static volatile bool emotion_dirty = false;
static volatile lcd_status_t pending_status = LCD_STATUS_STANDBY;
static volatile bool status_dirty = false;

static const char *lcd_status_text(lcd_status_t st)
{
    switch (st) {
    case LCD_STATUS_STANDBY:    return "待机中...";
    case LCD_STATUS_CONNECTING: return "联网中...";
    case LCD_STATUS_LISTENING:  return "聆听中...";
    case LCD_STATUS_REPLYING:   return "回复中...";
    default:                    return "待机中...";
    }
}

/* ── LVGL flush ── */
static void lcd_lvgl_flush_cb(lv_disp_drv_t *drv, const lv_area_t *area,
                               lv_color_t *color_map)
{
    esp_err_t err = ESP_FAIL;
    for (int attempt = 0; attempt < 3; ++attempt) {
        err = esp_lcd_panel_draw_bitmap(panel,
                                        area->x1, area->y1,
                                        area->x2 + 1, area->y2 + 1,
                                        color_map);
        if (err != ESP_ERR_NO_MEM) break;
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "LCD flush failed: %s", esp_err_to_name(err));
        lv_disp_flush_ready(drv);
    }
}

bool lcd_ui_color_trans_done(esp_lcd_panel_io_handle_t panel_io,
                             esp_lcd_panel_io_event_data_t *edata,
                             void *user_ctx)
{
    (void)panel_io;
    (void)edata;
    (void)user_ctx;
    lv_disp_flush_ready(&disp_drv);
    return false;
}

/* ── LVGL tick ── */
static void lcd_lvgl_tick_cb(void *arg)
{
    (void)arg;
    lv_tick_inc(portTICK_PERIOD_MS);
}

/* ── 绘制辅助：人脸（位图） ── */
#define FACE_COLOR 0x4488CC
#define FACE_IMG_OFFSET_Y 18   /* 顶部留出状态栏空间，图案整体下移 */

static lv_obj_t *create_face_image(lv_obj_t *parent, int idx)
{
    lv_obj_t *img = lv_img_create(parent);
    lv_img_set_src(img, &faces[idx]);
    lv_obj_set_pos(img, 0, FACE_IMG_OFFSET_Y);
    lv_obj_set_style_img_recolor(img, lv_color_hex(FACE_COLOR), 0);
    lv_obj_set_style_img_recolor_opa(img, LV_OPA_COVER, 0);
    lv_obj_add_flag(img, LV_OBJ_FLAG_HIDDEN);
    return img;
}

static void build_faces(lv_obj_t *parent)
{
    for (int i = 0; i < 5; i++) {
        face_imgs[i] = create_face_image(parent, i);
    }
}

static void apply_emotion(lcd_emotion_t emo)
{
    for (int i = 0; i < 5; i++) {
        if (face_imgs[i]) lv_obj_add_flag(face_imgs[i], LV_OBJ_FLAG_HIDDEN);
    }

    int show = 0;   /* 默认 NORMAL -> faces[0] */
    switch (emo) {
    case LCD_EMO_SMILE:  show = 2; break;   /* faces[2] = smile */
    case LCD_EMO_CRY:    show = 3; break;   /* faces[3] = sad  */
    case LCD_EMO_ANGRY:  show = 1; break;   /* faces[1] = angry */
    case LCD_EMO_DROOL:  show = 4; break;   /* faces[4] = drool (待机) */
    default:             show = 0; break;   /* faces[0] = normal */
    }
    if (face_imgs[show]) lv_obj_clear_flag(face_imgs[show], LV_OBJ_FLAG_HIDDEN);
}

/* ── 对外 API ── */
void lcd_ui_set_status(lcd_status_t st)
{
    pending_status = st;
    status_dirty = true;
}

void lcd_ui_set_emotion(lcd_emotion_t emo)
{
    pending_emotion = emo;
    emotion_dirty = true;
}

/* ── 初始化 ── */
void lcd_ui_init(void)
{
    lv_init();

    /* 获取 LCD panel */
    panel = lcd_get_panel();

    /* 获取 LCD panel */
    panel = lcd_get_panel();

    size_t buf_px = LCD_H_RES * LCD_DRAW_LINES;
    buf1 = heap_caps_malloc(buf_px * sizeof(lv_color_t),
                            MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    buf2 = NULL;

    lv_disp_draw_buf_init(&disp_buf, buf1, buf2, buf_px);
    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res = LCD_H_RES;
    disp_drv.ver_res = LCD_V_RES;
    disp_drv.flush_cb = lcd_lvgl_flush_cb;
    disp_drv.draw_buf = &disp_buf;
    lv_disp_drv_register(&disp_drv);

    esp_timer_create_args_t tick_args = {
        .callback = lcd_lvgl_tick_cb,
        .name     = "lvgl_tick",
    };
    esp_timer_handle_t tick_timer;
    esp_timer_create(&tick_args, &tick_timer);
    esp_timer_start_periodic(tick_timer, 1000);

    /* ── 屏幕 ── */
    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);

    /* ── 状态栏（顶部） ── */
    status_bar = lv_obj_create(scr);
    lv_obj_set_size(status_bar, 240, 36);
    lv_obj_align(status_bar, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_radius(status_bar, 0, 0);
    lv_obj_set_style_border_width(status_bar, 0, 0);
    lv_obj_set_style_bg_opa(status_bar, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(status_bar, 0, 0);

    status_label = lv_label_create(status_bar);
    lv_label_set_text(status_label, lcd_status_text(pending_status));
    lv_obj_set_style_text_color(status_label, lv_color_hex(0x4488CC), 0);
    lv_obj_set_style_text_font(status_label, &lv_font_cn_18, 0);
    lv_obj_center(status_label);

    /* ── 四帧表情（位图） ── */
    build_faces(scr);
    apply_emotion(LCD_EMO_NORMAL);

    ESP_LOGI(TAG, "LCD UI ready (4 faces + status bar)");
}

/* ── LVGL 主循环 ── */
void lcd_ui_task(void *arg)
{
    (void)arg;
    while (1) {
        lv_timer_handler();

        if (status_dirty) {
            lv_label_set_text(status_label, lcd_status_text(pending_status));
            status_dirty = false;
        }
        if (emotion_dirty) {
            apply_emotion(pending_emotion);
            emotion_dirty = false;
        }

        vTaskDelay(pdMS_TO_TICKS(5));
    }
}
