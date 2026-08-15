#pragma once
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif
void sntp_sync_start(void);           /* idempotent; call when online */
bool sntp_time_valid(void);
void sntp_apply_tz(const char *posix_tz);
#ifdef __cplusplus
}
#endif
