#ifndef __LCD_UI_H__
#define __LCD_UI_H__

#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"

typedef enum {
    LCD_EMO_SMILE  = 0,
    LCD_EMO_CRY    = 1,
    LCD_EMO_NORMAL = 2,
    LCD_EMO_ANGRY  = 3,
    LCD_EMO_DROOL  = 4,
} lcd_emotion_t;

typedef enum {
    LCD_STATUS_STANDBY    = 0,   /* 待机中 */
    LCD_STATUS_CONNECTING = 1,   /* 联网中 */
    LCD_STATUS_LISTENING  = 2,   /* 聆听中 */
    LCD_STATUS_REPLYING   = 3,   /* 回复中 */
} lcd_status_t;

void lcd_ui_init(void);
void lcd_ui_task(void *arg);

/* 线程安全：任意任务调用，lcd_ui_task 下一帧生效 */
void lcd_ui_set_status(lcd_status_t st);
void lcd_ui_set_emotion(lcd_emotion_t emo);

bool lcd_ui_color_trans_done(esp_lcd_panel_io_handle_t panel_io,
                             esp_lcd_panel_io_event_data_t *edata,
                             void *user_ctx);

#endif
