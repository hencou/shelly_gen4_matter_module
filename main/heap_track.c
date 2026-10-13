#include "heap_track.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define TRACK_SLOTS 320
#define TRACK_TASKS 16
#define TRACK_NAME  12

typedef struct {
    uintptr_t ptr;
    uint16_t  size;
    uint8_t   task;
} track_entry_t;

static const char *TAG = "heap_track";

static DRAM_ATTR track_entry_t s_tab[TRACK_SLOTS];
static DRAM_ATTR TaskHandle_t  s_task_handle[TRACK_TASKS];
static DRAM_ATTR char          s_task_name[TRACK_TASKS][TRACK_NAME];
static DRAM_ATTR uint32_t      s_task_bytes[TRACK_TASKS];
static DRAM_ATTR uint16_t      s_task_count[TRACK_TASKS];
static DRAM_ATTR uint16_t      s_task_max[TRACK_TASKS];
static DRAM_ATTR unsigned      s_tasks, s_live, s_dropped;
static DRAM_ATTR volatile bool s_active;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

static IRAM_ATTR unsigned slot_home(uintptr_t ptr)
{
    return (unsigned)((ptr >> 2) % TRACK_SLOTS);
}

static IRAM_ATTR unsigned task_index(void)
{
    TaskHandle_t self = xTaskGetCurrentTaskHandle();
    for (unsigned i = 0; i < s_tasks; i++)
        if (s_task_handle[i] == self) return i;
    if (s_tasks == TRACK_TASKS - 1) {
        if (!s_task_name[TRACK_TASKS - 1][0])
            strncpy(s_task_name[TRACK_TASKS - 1], "other", TRACK_NAME - 1);
        return TRACK_TASKS - 1;
    }
    unsigned i = s_tasks++;
    s_task_handle[i] = self;
    const char *name = self ? pcTaskGetName(self) : "?";
    strncpy(s_task_name[i], name, TRACK_NAME - 1);
    return i;
}

/* Linear-probing delete that shifts later entries back, so lookups never
 * stop early at a hole. */
static IRAM_ATTR void slot_remove(unsigned i)
{
    unsigned j = i;
    for (;;) {
        j = (j + 1) % TRACK_SLOTS;
        if (!s_tab[j].ptr) break;
        unsigned k = slot_home(s_tab[j].ptr);
        bool stays = (i <= j) ? (i < k && k <= j) : (i < k || k <= j);
        if (stays) continue;
        s_tab[i] = s_tab[j];
        i = j;
    }
    s_tab[i].ptr = 0;
}

IRAM_ATTR void esp_heap_trace_alloc_hook(void *ptr, size_t size, uint32_t caps)
{
    if (!s_active || !ptr) return;
    portENTER_CRITICAL_SAFE(&s_mux);
    if (s_live >= TRACK_SLOTS - 1) {
        s_dropped++;
    } else {
        unsigned i = slot_home((uintptr_t)ptr);
        while (s_tab[i].ptr) i = (i + 1) % TRACK_SLOTS;
        unsigned t = task_index();
        uint16_t sz = size > UINT16_MAX ? UINT16_MAX : (uint16_t)size;
        s_tab[i] = (track_entry_t){ (uintptr_t)ptr, sz, (uint8_t)t };
        s_live++;
        s_task_bytes[t] += sz;
        s_task_count[t]++;
        if (sz > s_task_max[t]) s_task_max[t] = sz;
    }
    portEXIT_CRITICAL_SAFE(&s_mux);
}

IRAM_ATTR void esp_heap_trace_free_hook(void *ptr)
{
    if (!s_active || !ptr) return;
    portENTER_CRITICAL_SAFE(&s_mux);
    unsigned i = slot_home((uintptr_t)ptr);
    while (s_tab[i].ptr) {
        if (s_tab[i].ptr == (uintptr_t)ptr) {
            unsigned t = s_tab[i].task;
            s_task_bytes[t] -= s_tab[i].size;
            s_task_count[t]--;
            s_live--;
            slot_remove(i);
            break;
        }
        i = (i + 1) % TRACK_SLOTS;
    }
    portEXIT_CRITICAL_SAFE(&s_mux);
}

void heap_track_start(void)
{
    if (s_active) return;
    s_active = true;
    ESP_LOGI(TAG, "tracking heap allocations per task from now on");
}

void heap_track_log(void)
{
    if (!s_active) return;
    uint32_t bytes[TRACK_TASKS];
    uint16_t count[TRACK_TASKS], max[TRACK_TASKS];
    unsigned tasks, live, dropped;
    portENTER_CRITICAL(&s_mux);
    tasks = s_tasks;
    live = s_live;
    dropped = s_dropped;
    memcpy(bytes, s_task_bytes, sizeof(bytes));
    memcpy(count, s_task_count, sizeof(count));
    memcpy(max, s_task_max, sizeof(max));
    portEXIT_CRITICAL(&s_mux);

    ESP_LOGI(TAG, "live since start: %u blocks, %u not tracked (table full)",
             live, dropped);
    for (unsigned i = 0; i < TRACK_TASKS; i++) {
        if (i >= tasks && i != TRACK_TASKS - 1) continue;
        if (!count[i]) continue;
        ESP_LOGI(TAG, "  %-12s %6u B in %3u blocks (largest %u)",
                 s_task_name[i], (unsigned)bytes[i], count[i], max[i]);
    }
}
