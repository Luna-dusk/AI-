#ifndef __LCD_H__
#define __LCD_H__

#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"

/* ST7789 240x240 SPI 引脚 */
#define LCD_SCL  GPIO_NUM_12
#define LCD_SDA  GPIO_NUM_11   /* MOSI */
#define LCD_DC   GPIO_NUM_10   /* Data / Command */
#define LCD_CS   GPIO_NUM_8
#define LCD_RST  GPIO_NUM_9
#define LCD_BL   GPIO_NUM_3    /* 背光 PWM */

#define LCD_H_RES  240
#define LCD_V_RES  240
#define LCD_DRAW_LINES 20

void lcd_init(void);
void lcd_set_backlight(uint8_t percent);
esp_lcd_panel_handle_t lcd_get_panel(void);

#endif
