#include "log_buffer.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define LOG_BUF_SIZE 8192
#define LOG_LINE_MAX  256

static char   s_buf[LOG_BUF_SIZE];
static size_t s_head;          /* next write position */
static bool   s_wrapped;
static SemaphoreHandle_t s_lock;
static vprintf_like_t s_next;  /* original sink (UART console) */

static void buf_append(const char *data, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        s_buf[s_head++] = data[i];
        if (s_head == LOG_BUF_SIZE) {
            s_head = 0;
            s_wrapped = true;
        }
    }
}

static int log_vprintf(const char *fmt, va_list ap)
{
    va_list ap_copy;
    va_copy(ap_copy, ap);

    char line[LOG_LINE_MAX];
    int n = vsnprintf(line, sizeof(line), fmt, ap_copy);
    va_end(ap_copy);

    if (n > 0) {
        size_t len = (n < (int)sizeof(line) - 1) ? (size_t)n : sizeof(line) - 1;
        /* The log hook runs from any task, including ISR-adjacent contexts on
         * the CHIP/OpenThread stacks; skip buffering rather than block. */
        if (s_lock && xSemaphoreTake(s_lock, 0) == pdTRUE) {
            buf_append(line, len);
            xSemaphoreGive(s_lock);
        }
    }

    return s_next ? s_next(fmt, ap) : vprintf(fmt, ap);
}

void log_buffer_init(void)
{
    if (s_lock) return;
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) return;
    s_next = esp_log_set_vprintf(log_vprintf);
}

size_t log_buffer_read(char *out, size_t out_size)
{
    if (!out || out_size == 0) return 0;
    out[0] = '\0';
    if (!s_lock) return 0;

    xSemaphoreTake(s_lock, portMAX_DELAY);

    size_t avail = s_wrapped ? LOG_BUF_SIZE : s_head;
    size_t want  = (avail < out_size - 1) ? avail : out_size - 1;
    /* Oldest bytes first; drop the excess from the front when out is smaller. */
    size_t start = s_wrapped ? (s_head + (avail - want)) % LOG_BUF_SIZE
                             : s_head - want;

    size_t first = LOG_BUF_SIZE - start;
    if (first > want) first = want;
    memcpy(out, s_buf + start, first);
    if (want > first) memcpy(out + first, s_buf, want - first);
    out[want] = '\0';

    xSemaphoreGive(s_lock);
    return want;
}
