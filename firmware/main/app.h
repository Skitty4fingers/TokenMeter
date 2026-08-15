#pragma once
/* cross-module hooks owned by main.c */
#ifdef __cplusplus
extern "C" {
#endif
void poller_poke(void);          /* wake the poll task now (config changed) */
void app_factory_reset(void);    /* erase NVS config + reboot */
void app_reboot(void);
void app_apply_settings(void);   /* brightness/tz/led after a settings save */
#ifdef __cplusplus
}
#endif
