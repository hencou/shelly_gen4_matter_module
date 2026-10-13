#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* Attributes heap allocations made after heap_track_start() to the task that
 * made them, until they are freed. Needs CONFIG_HEAP_USE_HOOKS. */
void heap_track_start(void);
void heap_track_log(void);

#ifdef __cplusplus
}
#endif
