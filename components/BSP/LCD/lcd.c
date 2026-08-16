#include "lcd.h"
#include "lcd_ui.h"

#include "esp_log.h"
#include "esp_lcd_panel_ops.h"
#include "driver/spi_master.h"
#include "driver/ledc.h"

static const char *TAG = "LCD";
static esp_lcd_panel_handle_t panel = NULL;
static esp_lcd_panel_io_handle_t io_handle = NULL;   /* 留着写寄存器 */

void lcd_init(void)
{
    /* ── 背光 PWM ── */
    ledc_timer_config_t ledc_timer = {
        .speed_mode       = LEDC_LOW_SPEED_MODE,
        .duty_resolution  = LEDC_TIMER_10_BIT,
        .timer_num        = LEDC_TIMER_0,
        .freq_hz          = 5000,
        .clk_cfg          = LEDC_AUTO_CLK,
    };
    ledc_timer_config(&ledc_timer);

    ledc_channel_config_t ledc_ch = {
        .gpio_num   = LCD_BL,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel    = LEDC_CHANNEL_0,
        .timer_sel  = LEDC_TIMER_0,
        .duty       = 0,
        .hpoint     = 0,
    };
    ledc_channel_config(&ledc_ch);

    /* ── SPI ── */
    spi_bus_config_t bus_cfg = {
        .sclk_io_num     = LCD_SCL,
        .mosi_io_num     = LCD_SDA,
        .miso_io_num     = -1,
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        .max_transfer_sz = LCD_H_RES * LCD_DRAW_LINES * sizeof(uint16_t),
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI3_HOST, &bus_cfg, SPI_DMA_CH_AUTO));

    esp_lcd_panel_io_spi_config_t io_cfg = {
        .cs_gpio_num          = LCD_CS,
        .dc_gpio_num          = LCD_DC,
        .spi_mode             = 0,
        .pclk_hz              = 40 * 1000 * 1000,
        .trans_queue_depth    = 10,
        .lcd_cmd_bits         = 8,
        .lcd_param_bits       = 8,
        .on_color_trans_done  = lcd_ui_color_trans_done,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI3_HOST,
                                              &io_cfg, &io_handle));

    /* ── ST7789 ── */
    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = LCD_RST,
        .bits_per_pixel = 16,
        .color_space    = ESP_LCD_COLOR_SPACE_RGB,   /* 先 RGB */
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(io_handle, &panel_cfg, &panel));

    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel));

    /* ── 打开反相（ST7789 模组通常需要） */
    esp_lcd_panel_invert_color(panel, true);

    /* 方向由 mirror 接管 */
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(panel, false, false));

    /* ── 手动锁死 MADCTL（在 mirror 之后写，防止被覆盖）
     *  MV=0 MX=0 MY=0 BGR=0 MH=0 → 全默认 RGB */
    uint8_t madctl = 0x00;
    esp_lcd_panel_io_tx_param(io_handle, 0x36, &madctl, 1);

    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel, true));

    lcd_ui_init();

    lcd_set_backlight(80);
    ESP_LOGI(TAG, "LCD ready (MADCTL=0x%02X invert=ON)", madctl);
}

void lcd_set_backlight(uint8_t percent)
{
    if (percent > 100) percent = 100;
    uint32_t duty = (uint32_t)percent * 1023 / 100;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

esp_lcd_panel_handle_t lcd_get_panel(void)
{
    return panel;
}
