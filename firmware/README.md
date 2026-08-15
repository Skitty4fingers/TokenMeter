# Token Meter firmware (ESP32-C6-LCD-1.47)

ESP-IDF v6.0 project. Shows Claude Pro/Max and ChatGPT/Codex rate-limit utilization on
the Waveshare 1.47" LCD, configured entirely through a web UI. Design reference:
`../design/ui-samples.html`; plan: `../docs/PLAN.md`.

## Build & flash

```sh
source ~/esp/esp-idf/export.sh        # IDF v6.0.x (5.4+ should also work)
cd firmware
idf.py set-target esp32c6              # first time only; downloads managed components
idf.py build
idf.py -p /dev/ttyACM0 flash monitor   # board enumerates as USB-JTAG/serial
```

## First boot

1. LCD shows **SETUP MODE** with a QR + `TokenMeter-XXXX` / password.
2. Join that hotspot from a phone; the captive portal opens (or browse to `http://192.168.4.1`).
3. Wizard: WiFi → paste `~/.claude/.credentials.json` (Claude Code's login; **`claude setup-token` does NOT work** — it lacks the `user:profile` scope → 403) → paste `~/.codex/auth.json` → Finish.
   Either provider can be skipped; an unconfigured provider is never drawn on the LCD.
4. Afterwards the UI lives at `http://tokenmeter.local` (or the IP shown in the web header).

If WiFi drops later, the device re-raises the hotspot (`OFFLINE` banner or the QR card if
nothing is cached yet) while retrying in the background every 30 s.

## Layout

```
main/
  main.c              boot, poll task (backoff, LED, night dim, SD log), hooks
  app_state.[ch]      shared snapshot + mutex (UI reads, poller/wifi write)
  storage/config.*    NVS blob (wifi, tokens, settings)
  board/pins.h        Waveshare pin map + panel quirks
  board/lcd.*         esp_lcd ST7789 + esp_lvgl_port
  board/led.*         WS2812 status LED policy
  board/sdcard.*      optional TF card, usage.csv appender
  net/wifi_mgr.*      PROVISION / CONNECTING / ONLINE / FALLBACK state machine, captive DNS
  net/webui.*         esp_http_server: embedded page + REST API
  net/sntp_sync.*     NTP + TZ
  providers/          provider.h interface, claude.c, openai.c (token refresh), http_util.c
  ui/                 LVGL screens (dashboard/boot/offline, provisioning), theme tokens
  www/index.html      the whole web UI (embedded, ~20 KB)
components/dns_server vendored from the ESP-IDF captive_portal example
```

## REST API

| Method | Path | Body / notes |
|---|---|---|
| GET | `/api/status` | live state: net, ip, rssi, heap, per-provider usage windows |
| GET/POST | `/api/config` | settings (tokens masked in GET; POST accepts a partial JSON) |
| GET | `/api/scan` | `[{ssid,rssi,open}]` |
| POST/DELETE | `/api/wifi` | `{ssid,pass}` → connects; DELETE forgets → provisioning |
| POST/DELETE | `/api/claude` | `{credjson}` (raw credentials.json) or `{token}` → verifies against Anthropic, saves on success |
| POST/DELETE | `/api/openai` | `{authjson}` (raw auth.json) or `{access,refresh,account}` |
| POST | `/api/setup/done` | wizard finished → drop the setup hotspot |
| POST | `/api/reboot`, `/api/reset` | reset = wipe NVS config + reboot |

## Hardware notes learned on the bench

- Panel: default `LCD_BGR_ORDER=0`, `LCD_INVERT=1`, rotation 0 render correctly on the first flash — no tweaks needed.
- **SD card shares SPI2 with the LCD** and the IDF 6.0 `sdspi` + `esp_lcd` drivers race on the C6
  (`assert spi_hal_setup_trans … running_cmd == 0` → reboot loop). All SD access therefore goes through
  `lcd_bus_quiesce()` (LVGL lock + wait for the in-flight flush + 3 ms settle), and nothing reads the
  card at runtime unless CSV logging is enabled (`/api/status` reports stats measured at mount time).
- Board enumerates as `/dev/ttyACM0` (group `uucp` on Arch/CachyOS — add your user to it).

## Verified on the bench (2026-08-15)

- Panel geometry/colours correct with the defaults in `board/pins.h` (rotation 0 = USB left).
- Both providers fetch over TLS from the device; heap low-water ~160 KB with both enabled.
- Cloudflare accepted the ESP32's TLS fingerprint for `chatgpt.com` from a residential IP.
- Still unverified: rotation 1 ("USB right") mirror flags; long-run token refresh behaviour
  (Claude ~8 h, OpenAI rotation) — watch the Settings page's "exp" / "refreshed" fields.

## Unofficial endpoints (may change)

- Claude: `GET https://api.anthropic.com/api/oauth/usage` — needs `User-Agent: claude-code/x.y.z`,
  `anthropic-beta: oauth-2025-04-20` and a token with the `user:profile` scope (Claude Code login token;
  access ~8 h, refresh ~28 d). Refresh: `POST https://console.anthropic.com/v1/oauth/token` JSON
  `{grant_type:refresh_token, refresh_token, client_id:9d1c250a-e61b-44d9-88ed-5944d1962f5e}`.
  Poll ≥ 180 s or expect long 429 lockouts. TLS needs the FULL cert bundle + `CROSS_SIGNED_VERIFY`
  (chain tops out at GTS Root R4 cross-signed by GlobalSign R1).
- ChatGPT: `GET https://chatgpt.com/backend-api/wham/usage`; refresh via
  `POST https://auth.openai.com/oauth/token`. Refresh tokens rotate → persisted immediately.
