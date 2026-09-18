#include "telemetry.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_timer.h"
#include "esp_log.h"

static const char *TAG = "telemetry";

static pet_telemetry_t s_telemetry = {
    .epoch_utc = 0,
    .cpu_pct = 0,
    .ram_used_mb = 0,
    .ram_total_mb = 0,
    .gpu_pct = 0,
    .gpu_temp_c = 0,
    .vram_used_mb = 0,
    .vram_total_mb = 0,
    .last_received_us = 0,
    .has_data = false,
};

static SemaphoreHandle_t s_telemetry_mutex = NULL;

void telemetry_init(void)
{
    if (!s_telemetry_mutex) {
        s_telemetry_mutex = xSemaphoreCreateMutex();
        assert(s_telemetry_mutex != NULL);
    }
}

void telemetry_parse_line(const char *line)
{
    if (!line) return;

    // Spec: Parse ONLY lines beginning with "PET,"
    if (strncmp(line, "PET,", 4) != 0) {
        return;
    }

    char buf[256];
    strncpy(buf, line, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    char *saveptr = NULL;
    char *token = strtok_r(buf, ",\r\n", &saveptr);
    if (!token || strcmp(token, "PET") != 0) {
        return;
    }

    // 1: epoch_utc
    token = strtok_r(NULL, ",\r\n", &saveptr);
    if (!token) return;
    int64_t epoch = strtoll(token, NULL, 10);

    // 2: cpu_pct
    token = strtok_r(NULL, ",\r\n", &saveptr);
    if (!token) return;
    int cpu = (int)roundf(strtof(token, NULL));

    // 3: ram_used_mb
    token = strtok_r(NULL, ",\r\n", &saveptr);
    if (!token) return;
    float ram_used = strtof(token, NULL);

    // 4: ram_total_mb
    token = strtok_r(NULL, ",\r\n", &saveptr);
    if (!token) return;
    float ram_total = strtof(token, NULL);

    // 5: gpu_pct
    token = strtok_r(NULL, ",\r\n", &saveptr);
    if (!token) return;
    int gpu = (int)roundf(strtof(token, NULL));

    // 6: gpu_temp_c
    token = strtok_r(NULL, ",\r\n", &saveptr);
    if (!token) return;
    int gpu_temp = (int)roundf(strtof(token, NULL));

    // 7: vram_used_mb
    token = strtok_r(NULL, ",\r\n", &saveptr);
    if (!token) return;
    float vram_used = strtof(token, NULL);

    // 8: vram_total_mb
    token = strtok_r(NULL, ",\r\n", &saveptr);
    if (!token) return;
    float vram_total = strtof(token, NULL);

    if (s_telemetry_mutex && xSemaphoreTake(s_telemetry_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        s_telemetry.epoch_utc = epoch;
        s_telemetry.cpu_pct = cpu;
        s_telemetry.ram_used_mb = ram_used;
        s_telemetry.ram_total_mb = ram_total;
        s_telemetry.gpu_pct = gpu;
        s_telemetry.gpu_temp_c = gpu_temp;
        s_telemetry.vram_used_mb = vram_used;
        s_telemetry.vram_total_mb = vram_total;
        s_telemetry.last_received_us = esp_timer_get_time();
        s_telemetry.has_data = true;
        xSemaphoreGive(s_telemetry_mutex);
        ESP_LOGD(TAG, "Parsed PET: cpu=%d%% ram=%.1f/%.1fMB gpu=%d%% temp=%dC vram=%.1f/%.1fMB",
                 cpu, ram_used, ram_total, gpu, gpu_temp, vram_used, vram_total);
    }
}

pet_telemetry_t telemetry_get(void)
{
    pet_telemetry_t copy;
    if (s_telemetry_mutex && xSemaphoreTake(s_telemetry_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        copy = s_telemetry;
        xSemaphoreGive(s_telemetry_mutex);
    } else {
        copy = s_telemetry;
    }
    return copy;
}

bool telemetry_is_host_online(void)
{
    pet_telemetry_t t = telemetry_get();
    if (!t.has_data) return false;
    int64_t diff = esp_timer_get_time() - t.last_received_us;
    return diff <= 10000000LL; // 10 seconds timeout
}
