#pragma once

#include <stdint.h>
#include <stdbool.h>

#define TIMEZONE_OFFSET_HOURS (8)
#define TIMEZONE_OFFSET_SEC   (TIMEZONE_OFFSET_HOURS * 3600)

typedef struct {
    int64_t epoch_utc;
    int cpu_pct;
    int ram_used_mb;
    int ram_total_mb;
    int gpu_pct;
    int gpu_temp_c;
    int vram_used_mb;
    int vram_total_mb;
    int64_t last_received_us;
    bool has_data;
} pet_telemetry_t;

void telemetry_init(void);
void telemetry_parse_line(const char *line);
pet_telemetry_t telemetry_get(void);
bool telemetry_is_host_online(void);
