#pragma once
#include <stdbool.h>
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Mount the TF card on the shared SPI bus (call after lcd_init). Returns true if mounted. */
bool sdcard_mount(void);
bool sdcard_mounted(void);
/* Total/used bytes for status page (0 if not mounted). */
void sdcard_stats(size_t *total_kb, size_t *used_kb);
/* Append one CSV line (already formatted, newline added). */
bool sdcard_append_csv(const char *line);
#ifdef __cplusplus
}
#endif
