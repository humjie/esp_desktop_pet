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
#include "audio.h"

static const char *TAG = "deskpet_main";

static void handle_host_command(const char *cmd)
{
    ESP_LOGI(TAG, "Host command: %s", cmd);

    if (strncmp(cmd, "CMD,VOICE_WAKE", 14) == 0) {
        if (bsp_display_lock(100)) {
            ui_set_voice_listening(true);
            bsp_display_unlock();
        }
        audio_play_soft_wake_ring();
        return;
    }

    if (strncmp(cmd, "CMD,VOICE_IDLE", 14) == 0) {
        if (bsp_display_lock(100)) {
            ui_set_voice_listening(false);
            bsp_display_unlock();
        }
        return;
    }

    if (strncmp(cmd, "CMD,VOL,", 8) == 0) {
        const char *val = cmd + 8;
        int cur_vol = audio_get_spk_volume();
        if (strcmp(val, "UP") == 0) {
            cur_vol += 10;
        } else if (strcmp(val, "DOWN") == 0) {
            cur_vol -= 10;
        } else {
            cur_vol = atoi(val);
        }
        audio_set_spk_volume(cur_vol);
        return;
    }

    // Format: CMD,VOICE_RESP,<action>,<text>
    if (strncmp(cmd, "CMD,VOICE_RESP,", 15) == 0) {
        char buf[128];
        strncpy(buf, cmd + 15, sizeof(buf) - 1);
        buf[sizeof(buf) - 1] = '\0';

        char *action = strtok(buf, ",");
        char *text = strtok(NULL, "");
        if (!text) text = "";

        if (bsp_display_lock(100)) {
            if (action && strcmp(action, "APPEARANCE_CAT") == 0) {
                ui_set_pet_type(PET_TYPE_CAT);
                ui_trigger_voice_reaction(PET_EMOTION_LOVE, "SWITCHED TO CAT");
            } else if (action && strcmp(action, "APPEARANCE_ORIGINAL") == 0) {
                ui_set_pet_type(PET_TYPE_ORIGINAL);
                ui_trigger_voice_reaction(PET_EMOTION_LOVE, "CYBER PET READY");
            } else if (action && strcmp(action, "APPEARANCE_TOGGLE") == 0) {
                ui_cycle_appearance();
            } else if (action && strcmp(action, "SCREEN_AI") == 0) {
                ui_show_ai_mode(true);
            } else if (action && strcmp(action, "SCREEN_IDLE") == 0) {
                ui_show_ai_mode(false);
            } else if (action && strcmp(action, "RELAX") == 0) {
                ui_trigger_voice_reaction(PET_EMOTION_RELAX, text);
            } else if (action && strcmp(action, "HAPPY") == 0) {
                ui_trigger_voice_reaction(PET_EMOTION_HAPPY, text);
            } else {
                ui_trigger_voice_reaction(PET_EMOTION_LOVE, text);
            }
            bsp_display_unlock();
        }
        audio_play_confirm_ring();
    }
}

/* MAIN button callback: toggle the Idle <-> AI Mode screen, or stop music if playing. */
static void main_btn_cb(void *arg, void *user_data)
{
    (void)arg;
    (void)user_data;
    if (audio_is_music_mode()) {
        printf("EVENT,MUSIC_STOP\n");
        fflush(stdout);
        audio_set_music_mode(false);
        audio_spk_stop();
        return;
    }
    if (bsp_display_lock(100)) {
        ui_toggle_ai_mode();
        bsp_display_unlock();
    } else {
        ESP_LOGW(TAG, "Could not acquire display lock for button toggle");
    }
}

/* SIDE button callback: cycle pet appearance (Original <-> Cat) */
static void side_btn_appearance_cb(void *arg, void *user_data)
{
    (void)arg;
    (void)user_data;
    if (audio_is_music_mode()) {
        printf("EVENT,MUSIC_STOP\n");
        fflush(stdout);
        audio_set_music_mode(false);
        audio_spk_stop();
        return;
    }
    if (bsp_display_lock(100)) {
        ui_cycle_appearance();
        bsp_display_unlock();
    } else {
        ESP_LOGW(TAG, "Could not acquire display lock for appearance toggle");
    }
}

static char s_rx_line_buf[2048];
static uint8_t s_rx_raw_buf[2048];

static void serial_rx_task(void *pvParameters)
{
    (void)pvParameters;
    size_t line_idx = 0;

    ESP_LOGI(TAG, "Serial RX task started (USB Serial/JTAG console)");

    while (1) {
        int len = usb_serial_jtag_read_bytes(s_rx_raw_buf, sizeof(s_rx_raw_buf), pdMS_TO_TICKS(10));
        if (len > 0) {
            for (int i = 0; i < len; i++) {
                char c = (char)s_rx_raw_buf[i];
                if (c == '\n' || c == '\r') {
                    if (line_idx > 0) {
                        s_rx_line_buf[line_idx] = '\0';
                        if (strncmp(s_rx_line_buf, "PET,", 4) == 0) {
                            telemetry_parse_line(s_rx_line_buf);
                        } else if (strncmp(s_rx_line_buf, "SPK,MUS,START", 13) == 0) {
                            audio_set_music_mode(true);
                        } else if (strncmp(s_rx_line_buf, "SPK,TTS,START", 13) == 0) {
                            audio_set_music_mode(false);
                        } else if (strncmp(s_rx_line_buf, "SPK,TTS,END", 11) == 0) {
                            // TTS finished, do not stop speaker so remaining audio finishes naturally
                        } else if (strncmp(s_rx_line_buf, "SPK,D,", 6) == 0) {
                            audio_feed_spk_base64(s_rx_line_buf + 6);
                        } else if (strncmp(s_rx_line_buf, "SPK,STOP", 8) == 0) {
                            audio_set_music_mode(false);
                            audio_spk_stop();
                        } else if (strncmp(s_rx_line_buf, "CMD,", 4) == 0) {
                            handle_host_command(s_rx_line_buf);
                        }
                        line_idx = 0;
                    }
                } else {
                    if (line_idx < sizeof(s_rx_line_buf) - 1) {
                        s_rx_line_buf[line_idx++] = c;
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

    /* 6. Turn on Backlight (50% brightness) */
    vTaskDelay(pdMS_TO_TICKS(100));
    bsp_display_brightness_set(50);
    ESP_LOGI(TAG, "Display and backlight initialized (50%% brightness)");

    /* 7. Install USB-Serial-JTAG driver for console RX */
    usb_serial_jtag_driver_config_t usj_config = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    usj_config.rx_buffer_size = 16384;
    usj_config.tx_buffer_size = 8192;
    ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&usj_config));
    usb_serial_jtag_vfs_use_driver();

    /* 8. Launch background serial listener on Core 0 */
    xTaskCreatePinnedToCore(serial_rx_task, "serial_rx", 6144, NULL, 5, NULL, 0);

    /* 9. Buttons:
     * - BSP_BUTTON_MUTE (physical GPIO 1 side button): cycle pet appearance
     * - BSP_BUTTON_CONFIG (physical GPIO 0 side button): cycle pet appearance
     * - BSP_BUTTON_MAIN (capacitive touch home button): toggle Pet <-> System Status screen
     */
    ESP_ERROR_CHECK(bsp_btn_init());
    ESP_ERROR_CHECK(bsp_btn_register_callback(BSP_BUTTON_MUTE, BUTTON_SINGLE_CLICK, side_btn_appearance_cb, NULL));
    bsp_btn_register_callback(BSP_BUTTON_CONFIG, BUTTON_SINGLE_CLICK, side_btn_appearance_cb, NULL);
    bsp_btn_register_callback(BSP_BUTTON_MAIN, BUTTON_SINGLE_CLICK, main_btn_cb, NULL);

    /* 10. Initialize Audio (ES7210 microphone + ES8311 speaker + on-chip VAD) */
    ESP_ERROR_CHECK(audio_init());

    ESP_LOGI(TAG, "Desk Pet initialization completed successfully (Voice + Touch + Side button ready)");
}
