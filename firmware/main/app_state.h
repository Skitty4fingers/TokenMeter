#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- connectivity state machine ---- */
typedef enum {
    NET_BOOT = 0,
    NET_PROVISION,   /* no creds: softAP + captive portal */
    NET_CONNECTING,  /* STA attempt */
    NET_ONLINE,      /* STA up, IP, polling */
    NET_FALLBACK,    /* STA down: AP+STA, retrying */
} net_state_t;

/* ---- provider usage snapshot ---- */
#define PROV_MAX_WIN 4
#define PROV_ERR_LEN 64
#define PROV_PLAN_LEN 20

typedef struct {
    bool   valid;
    float  pct;          /* 0..100 utilization */
    time_t resets_at;    /* epoch UTC, 0 if unknown */
    char   label[8];     /* "5h", "wk", "opus", "fable"… */
} usage_window_t;

typedef struct {
    bool   enabled;      /* provider configured (has credentials) */
    bool   ok;           /* last fetch succeeded */
    char   err[PROV_ERR_LEN];
    char   plan[PROV_PLAN_LEN];
    time_t fetched_at;   /* epoch of last successful fetch, 0 = never */
    time_t next_poll;    /* epoch of next scheduled poll */
    int    nwin;
    usage_window_t win[PROV_MAX_WIN];
} provider_usage_t;

typedef enum { PROV_CLAUDE = 0, PROV_OPENAI = 1, PROV_COUNT } provider_id_t;

/* ---- global state ---- */
typedef struct {
    net_state_t net;
    char  ssid[33];
    char  ip[16];
    int   rssi;
    char  ap_ssid[33];
    char  ap_pass[17];
    bool  time_synced;
    provider_usage_t prov[PROV_COUNT];
    uint32_t seq;         /* increments on every change; UI redraws when it moves */
} app_state_t;

void app_state_init(void);
/* lock/unlock around reads or writes of app_state_get() */
void app_state_lock(void);
void app_state_unlock(void);
app_state_t *app_state_get(void);
/* mark dirty (bump seq) — call while locked */
void app_state_touch(void);
/* copy-out helper: takes lock, copies snapshot, releases */
void app_state_snapshot(app_state_t *out);

#ifdef __cplusplus
}
#endif
