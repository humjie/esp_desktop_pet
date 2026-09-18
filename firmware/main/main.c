#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_check.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"

#include "bsp/esp-bsp.h"
#include "bsp_board.h"
#include "telemetry.h"
#include "ui.h"

static const char *TAG = "deskpet_main";

/* MAIN button callback: toggle the Idle <-> AI Mode screen. */
static void main_btn_cb(void *arg, void *user_data)
{
    (void)arg;
    (void)user_data;
    if (bsp_display_lock(100)) {
        ui_toggle_ai_mode();
        bsp_display_unlock();
    } else {
        ESP_LOGW(TAG, "Could not acquire display lock for button toggle");
    }
}

static void serial_rx_task(void *pvParameters)
{
    uint8_t rx_buf[128];
    char line_buf[256];
    size_t line_idx = 0;

    ESP_LOGI(TAG, "Serial RX task started (USB Serial/JTAG console)");

    while (1) {
        int len = usb_serial_jtag_read_bytes(rx_buf, sizeof(rx_buf), pdMS_TO_TICKS(50));
        if (len > 0) {
            for (int i = 0; i < len; i++) {
                char c = (char)rx_buf[i];
                if (c == '\n' || c == '\r') {
                    if (line_idx > 0) {
                        line_buf[line_idx] = '\0';
                        telemetry_parse_line(line_buf);
                        line_idx = 0;
                    }
                } else {
                    if (line_idx < sizeof(line_buf) - 1) {
                        line_buf[line_idx++] = c;
                    } else {
                        // Protect against line buffer overflow
                        line_idx = 0;
                    }
                }
            }
        }
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "Desk Pet firmware starting... (Build: %s %s)", __DATE__, __TIME__);

    /* 1. Initialize NVS */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    /* 2. Initialize Telemetry store and mutex */
    telemetry_init();

    /* 3. Initialize I2C for touch */
    ESP_ERROR_CHECK(bsp_i2c_init());

    /* 4. Start Display and LVGL task (Core 1) */
    bsp_display_cfg_t cfg = {
        .lvgl_port_cfg = ESP_LVGL_PORT_INIT_CONFIG(),
        .buffer_size = BSP_LCD_H_RES * CONFIG_BSP_LCD_DRAW_BUF_HEIGHT,
        .double_buffer = 0,
        .flags = {
            .buff_dma = true,
            .buff_spiram = false,
        }
    };
    cfg.lvgl_port_cfg.task_affinity = 1;
    bsp_display_start_with_config(&cfg);

    /* 5. Initialize UI under LVGL display lock */
    bsp_display_lock(0);
    ui_init();
    bsp_display_unlock();

    /* 6. Turn on Backlight */
    vTaskDelay(pdMS_TO_TICKS(100));
    bsp_display_backlight_on();
    ESP_LOGI(TAG, "Display and backlight initialized");

    /* 7. Install USB-Serial-JTAG driver for console RX */
    usb_serial_jtag_driver_config_t usj_config = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    usj_config.rx_buffer_size = 1024;
    usj_config.tx_buffer_size = 1024;
    ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&usj_config));
    usb_serial_jtag_vfs_use_driver();

    /* 8. Launch background serial listener on Core 0 */
    xTaskCreatePinnedToCore(serial_rx_task, "serial_rx", 4096, NULL, 5, NULL, 0);

    /* 9. Buttons: BSP_BUTTON_MUTE (physical GPIO 1) and BSP_BUTTON_MAIN (capacitive touch home) */
    ESP_ERROR_CHECK(bsp_btn_init());
    ESP_ERROR_CHECK(bsp_btn_register_callback(BSP_BUTTON_MUTE, BUTTON_SINGLE_CLICK, main_btn_cb, NULL));
    bsp_btn_register_callback(BSP_BUTTON_MAIN, BUTTON_SINGLE_CLICK, main_btn_cb, NULL);

    ESP_LOGI(TAG, "Desk Pet initialization completed successfully (Touchscreen + Button toggle ready)");
}
