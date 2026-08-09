#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Captures ESP_LOG output into a RAM ring buffer so it can be read over HTTP.
 * Needed because the Shelly Plus Add-on occupies GPIO16/17 (UART0), which
 * leaves no serial console while the Add-on is connected. */
void log_buffer_init(void);

/* Copies the buffered log (oldest first, NUL-terminated) into out.
 * Returns the number of bytes written, excluding the terminator. */
size_t log_buffer_read(char *out, size_t out_size);

#ifdef __cplusplus
}
#endif
