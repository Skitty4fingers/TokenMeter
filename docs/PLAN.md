# ESP32-C6 Token Burn Meter — Plan

## Context

Scott has a Waveshare ESP32-C6-LCD-1.47 (172×320 ST7789V3, SD slot) and wants a desk gadget that shows how much of his **Claude Pro/Max** and **ChatGPT Plus/Pro** rate limits he has burned — live, fully on-device (ESP32 polls the APIs itself). Configuration is via a web UI: first boot hosts a hotspot with SSID/password shown on the LCD; a captive portal configures WiFi + credentials; if WiFi becomes unreachable the device falls back to hotspot mode. No encryption for MVP. UI design samples come before firmware.

Decisions made with user:
- Sources: Claude subscription + ChatGPT (Codex) subscription
- Metric: rate-limit utilization % + reset countdowns
- Stack: ESP-IDF 5.4/5.5 + esp_lcd + LVGL 9.2
- LCD layout: **stacked bar dashboard** (landscape, 4 bars)
- Codex auth: fully on-device (paste auth.json tokens once; device refreshes itself)
- **Unconfigured providers are hidden everywhere** (LCD, web dashboard, LED): if only Claude has a token, ChatGPT is not drawn at all and the LCD switches to a single-provider layout (taller rows, bigger numbers, Claude's Opus/Sonnet weekly sub-limits). Wizard steps are skippable; removing a token in Settings drops the provider back out.

## Research findings (condensed)

### Claude subscription usage
- `GET https://api.anthropic.com/api/oauth/usage` — headers `Authorization: Bearer <token>`, `anthropic-beta: oauth-2025-04-20`, **`User-Agent: claude-code/<ver>`** (wrong UA ⇒ long 429 lockouts).
- Response (<1KB): `five_hour{utilization,resets_at}`, `seven_day{...}`, `seven_day_opus`, `seven_day_sonnet`, `extra_usage`.
- Poll ≥180s. Auth: `claude setup-token` → long-lived `sk-ant-oat01-…` (~1yr), no refresh needed on-device.
- Unofficial endpoint — document as such.

### ChatGPT/Codex usage
- `GET https://chatgpt.com/backend-api/wham/usage` — headers `Authorization: Bearer <access_token>`, `ChatGPT-Account-ID: <account_id>`.
- Response: `rate_limit.primary_window{used_percent,reset_at,reset_after_seconds}` (5h), `secondary_window` (weekly), `plan_type`, `credits`.
- Auth from `~/.codex/auth.json` (`tokens.access_token/refresh_token/account_id`). Refresh: `POST https://auth.openai.com/oauth/token` form `grant_type=refresh_token&refresh_token=…&client_id=app_EMoamEEZ73f0CkXaXp7hrann`. Refresh tokens rotate → persist new one in NVS immediately.
- Risks: Cloudflare TLS-fingerprint 403s (works from residential IPs today; show clear error state), shape may change. Poll ~60–120s.

### Hardware / stack
- LCD SPI: MOSI=6, SCLK=7, CS=14, DC=15, RST=21, BL=22 (PWM ≤50%). X offset 34, INVON, BGR order.
- SD: same SPI bus, MISO=5, CS=4. WS2812 LED GPIO8. Single-core 160MHz, 512KB SRAM, no PSRAM, 4MB flash.
- Provisioning: ESP-IDF captive_portal pattern (softAP 192.168.4.1 + DNS hijack + esp_http_server).
- TLS: esp_crt_bundle, `MBEDTLS_DYNAMIC_BUFFER`, sequential fetches, never during AP-only mode.
- Config in NVS; SD optional for CSV history. NTP via esp_netif_sntp.

## Deliverables

### Phase 0 — Design samples ✅ done
- `design/` folder: HTML page with pixel-accurate 320×172 LCD mockups (dashboard normal/warn/critical, provisioning screen, offline/fallback, error states) + web config UI mockups (setup wizard, dashboard, settings). Published as artifact for review.

### Phase 1 — Firmware skeleton ✅ flashed & running on the bench (2026-08-15)
Builds clean on ESP-IDF v6.0.2 (LVGL 9.5, esp_lvgl_port 2.9). See `firmware/README.md`. Project layout:
```
firmware/
  CMakeLists.txt, sdkconfig.defaults, partitions.csv (nvs, phy, factory 2.5MB, spiffs/www)
  main/
    main.c              boot, state machine, task creation
    app_state.h         shared struct (usage snapshots, wifi state, errors) + mutex
    board/lcd.c/.h      esp_lcd ST7789 init, LVGL port, backlight PWM
    board/led.c         WS2812 status LED
    board/sdcard.c      optional SD mount + CSV logger
    net/wifi_mgr.c      STA connect / AP fallback state machine, retry backoff
    net/captive_dns.c   DNS hijack for portal
    net/webui.c         esp_http_server: static assets + REST (/api/config, /api/status, /api/scan, /api/test)
    net/sntp.c
    providers/provider.h  common interface: enabled(), fetch() → usage_t; poll loop and UI iterate only enabled providers
    providers/claude.c
    providers/openai.c    incl. token refresh
    ui/ui_dashboard.c   LVGL stacked-bar screen; builds rows only for providers with `enabled` set — 2-provider layout (row 24px, pct font 18) or solo layout (row 30px, pct font 24, +opus/sonnet rows)
    ui/ui_provision.c   hotspot SSID/pass screen
    ui/ui_status.c      offline/error overlays
    ui/theme.c          colors, fonts (Montserrat 10/12/14/18/24)
    storage/config.c    NVS load/save of config struct
    www/                index.html/app.js/style.css (gzipped, embedded via EMBED_FILES)
```

Boot flow:
1. NVS load config → if no WiFi creds → **PROVISION** mode: softAP `TokenMeter-XXXX` / random 8-char pass shown on LCD + QR (LVGL qrcode). Captive portal → setup wizard.
2. Else **CONNECTING**: STA connect, 30s timeout with 3 retries → on failure enter **FALLBACK**: softAP + portal *and* keep retrying STA every 60s in background (AP+STA mode); LCD shows last-known data greyed + banner "Offline — hotspot TokenMeter-XXXX".
3. **ONLINE**: SNTP sync → poll loop (Claude every 180s, OpenAI every 120s, staggered) → update LVGL → optional CSV append to SD.
4. Web UI always available at device IP (mDNS `tokenmeter.local`) in ONLINE mode.

Web UI pages: Setup wizard (WiFi scan/select/password → Claude token → ChatGPT auth.json paste → done), Dashboard (mirror of LCD + raw JSON), Settings (poll intervals, brightness, rotation, thresholds, LED behaviour, timezone, factory reset), Status/Logs (heap, RSSI, last errors).

### Phase 2 — Polish
Warning colors at thresholds (default 70/90%), LED color mirrors worst gauge, night dim schedule, OTA (later).

## Verification
- Bench: flash, confirm provisioning screen, join hotspot from phone → captive portal pops → configure → reboots to dashboard.
- Pull WiFi router → fallback banner + hotspot within ~2min; restore → auto-recovers.
- Verify utilization matches `claude /usage` and `codex /status`. ✅ both providers polling on-device (2026-08-15); Claude needs credentials.json (not setup-token).
- Configure only Claude → LCD shows solo layout, no ChatGPT anywhere; add ChatGPT token → switches to two-provider layout without reboot; remove it → back to solo.
- Heap watermark logged; must stay >60KB free during TLS fetches. ✅ ~160KB low-water with both providers.
