# AGENTS.md

This file provides guidance to Codex (Codex.ai/code) when working with code in this repository.

## What this is

ESP32-C3 firmware (Arduino framework) for a low-cost SMS forwarder. A 4G/LTE modem
(ML307R-DC) receives SMS over UART/AT; the ESP32-C3 decodes PDU, then forwards the
message over WiFi to email (SMTP) and up to 5 simultaneous push channels, and serves a
web UI for config/diagnostics. Receive-only by design — multi-SIM, calls, dialing, and
open APIs are explicitly out of scope (see README).

All source lives in `code/` as a single Arduino sketch. Inline doc/comments are in Chinese;
keep that convention when editing.

## Build / flash / monitor

Board FQBN: `esp32:esp32:makergo_c3_supermini` (or `esp32:esp32:esp32c3` as CI uses).
Toolchain: arduino-cli. Required libraries: **pdulib** (PDU SMS codec) and **ReadyMail**
(SMTP), plus the ESP32 Arduino core (3.x).

```powershell
# Build (Windows / arduino-cli) — keep all paths ASCII-only; Chinese paths break compilation
arduino-cli compile --fqbn esp32:esp32:makergo_c3_supermini --build-path "<ascii-build-dir>" "<repo>\code"

# Flash over USB CDC (device shows as a COM port, e.g. COM4)
arduino-cli upload --fqbn esp32:esp32:makergo_c3_supermini --port COM4 --input-dir "<ascii-build-dir>" "<repo>\code"

# Serial monitor (logs at 115200)
arduino-cli monitor --port COM4 --config 115200
```

If upload fails: hold BOOT, tap RESET, release BOOT, retry. On-device verification is manual via
the serial monitor and the web UI. CI (`.github/workflows/build.yml`) only compiles on changes
under `code/**`.

**Host unit tests** (`test/`): portable pure-logic helpers live in `code/sms_logic.h` and are
unit-tested natively with g++ (no Arduino/pdulib deps) via a small `String` shim. Run on a dev box:
`./test/run.sh` (or `test/run.ps1`). Add a case in `test/run_tests.cpp` whenever you add/extend a
pure helper in `sms_logic.h`. PDU decode, AT state machines, and WebServer streaming can't be
host-tested — verify those on device.

`code/wifi_config.h` (gitignored, holds plaintext creds — never commit) defines `WIFI_SSID`/
`WIFI_PASS`. These are now only a **fallback/seed**: WiFi creds are stored in NVS
(`config.wifiSsid/wifiPass`) and editable from the web UI's **WiFi 设置** panel. Resolution order:
NVS creds → wifi_config.h macro → if both empty (or STA connect fails), the device starts a
**provisioning SoftAP** (`SMS-Forwarder-<mac>`, open, captive portal via DNSServer) so you connect
to it and configure WiFi at `192.168.4.1`. So wifi_config.h may be left with empty strings for a
pure web-provisioned setup; it still must define the macros for compilation. Saving WiFi in the web
UI persists to NVS and reboots to connect.

## Architecture

Standard single-threaded Arduino model: everything runs serially in `loop()`, no RTOS tasks.
`loop()` each frame: `server.handleClient()` → invalid-config IP print → `checkConcatTimeout()`
→ USB↔modem AT passthrough → `checkSerial1URC()`. Keep per-frame work non-blocking.

`loop()` also runs (non-blocking, millis-gated): `wifiEnsureConnected()` (reconnect), `modemHealthTick()`
(AT+CEREG probe + auto power-cycle recovery), `processRetryQueue()` (push retry), `heapGuardTick()`
(low-heap orderly restart), `keepAliveTick()` (SIM keep-alive), and a `yield()`.

Module layering (`#include` direction, bottom → top):
`config_types.h` (enums/structs/constants + tunable `#define`s, no deps) → `sms_logic.h`
(header-only inline pure helpers: json/url/html escape, phone mask, blacklist match, fnv1a dedup,
keep-alive due-date, OTP, backoff, `streamTemplate`; host-tested) → `globals.h/.cpp` (extern globals
+ shared includes; includes `sms_logic.h` once for all modules) → feature modules (`config`, `modem`,
`push`, `sms_process`, `scheduler`, `web_handlers`, `web_html`) → `code.ino` (`setup()` + `loop()`).
Detailed per-file guide: `dev_doc/module_details.md`; data-flow diagrams: `dev_doc/architecture.md`.

Key flows:
- **SMS receive** (`sms_process.cpp`): `checkSerial1URC()` reads `Serial1` line-by-line, detects
  `+CMT:`, feeds the hex PDU to `pdu.decodePDU()`. Multipart (long) SMS is buffered per ref-number
  in `concatBuffer` (5 slots × 10 parts) and merged when complete, or force-forwarded after a 30s
  timeout. Assembled text goes to `processSmsContent()` → blacklist filter → admin-command check →
  `sendSMSToServer()` + `sendEmailNotification()`.
- **Push** (`push.cpp`): `sendToChannel()` is a `switch(channel.type)` over `PushType` (1–10:
  POST JSON, Bark, GET, DingTalk, PushPlus, ServerChan, Custom template, Feishu, Gotify, Telegram).
  DingTalk/Feishu HMAC-SHA256 signing uses ESP32's built-in `mbedtls_md`.
- **Web** (`web_handlers.cpp` + `web_html.cpp`): single SPA page (`htmlPage` raw-literal in
  web_html.cpp, JS-toggled panels). All routes behind HTTP Basic Auth (`config.webUser/webPass`,
  default `admin`/`admin123`). `handleRoot()` **streams** the page from flash via
  `streamTemplate()` + `server.sendContent_P()` (no full-page heap copy); `%PLACEHOLDER%` tokens are
  resolved by a lambda, not `html.replace()`. Routes: `/`,`/save`,`/sendsms`,`/ping`,`/query`,
  `/flight`,`/at`,`/log`(streamed, `?since=` cursor),`/modem`,`/wifi`,`/status`(health JSON),
  `/keepalive`,`/testpush`,`/ussd`. Sidebar icons are inline SVG (no emoji).
- **Scheduler** (`scheduler.cpp`): E0 SIM keep-alive — absolute-date (NVS `kaLastTime`), power-loss
  safe; action = ping/SMS/USSD; `keepAliveTick()` in loop, `/keepalive?action=status|run|reset`.
- **Config** (`config.cpp`): persisted to NVS via `Preferences`, namespace `sms_config`.
  `loadConfig()` migrates legacy single-channel keys (`httpUrl`, `barkMode`) into `pushChannels[0]`.
  New keys are additive with defaults (zero-migration). Tunable defaults (timeouts, queue size,
  retry backoff, modem-health interval, low-heap threshold, dedup window) live atop `config_types.h`.
- **Privacy/logging**: SMS body + phone numbers are masked in the (web-visible) log ring buffer by
  default (`maskPhone`/`bodyPreview`); full content only when compiled with `-DSMS_LOG_VERBOSE=1`.

## Conventions when extending

- **New push channel**: add to `PushType` enum (config_types.h) → add validation in
  `isPushChannelValid()` → add a `case` in `sendToChannel()` → add the `<option>` + JS hint in the
  web UI. `MAX_PUSH_CHANNELS` is 5.
- **New config field**: add to `Config` struct → read (with default) in `loadConfig()` → write in
  `saveConfig()` → parse from the form in `handleSave()`. Keep keys additive (default-valued) so
  upgrades need no migration.
- **New HTTP route**: add a handler in web_handlers.cpp → declare in web_handlers.h → register with
  `server.on()` in `setup()`. For large/dynamic responses prefer chunked `server.sendContent()`
  (set `CONTENT_LENGTH_UNKNOWN`, end with `sendContent("")`) over building one big `String`.
- **New page placeholder**: add a `%TOKEN%` in web_html.cpp → add a branch in the `handleRoot()`
  resolver lambda. (Tokens are matched as `%[A-Z0-9_]{2,}%`; bare `%` in CSS is safe.)
- **New pure/portable logic**: put it in `sms_logic.h` (inline, depends only on `String`+libc) and
  add a host test in `test/run_tests.cpp` so it's verifiable without hardware.
- **Logging**: use `logCapture()` to append to the current line buffer and `logCaptureLn()` /
  `logCaptureF("...\n")` to commit a full line to the 120-entry ring buffer (also mirrored to
  `Serial`). Don't commit partial lines — that's why the line buffer exists.
- **Modem init order matters**: AT setup (handshake → `AT+CGACT=0,1` to disable data and avoid
  traffic charges → `AT+CNMI` URC reporting → `AT+CMGF=0` PDU mode → wait for CEREG registration)
  must complete before WiFi. PDU mode (not Text) is required for Chinese SMS. The `modemPowerCycle()`
  EN-pin timing (`MODEM_POWERUP_MS`/`MODEM_POWERDOWN_MS` in config_types.h, default 6000/1200) is
  tuned for ML307R-DC — when swapping modems retune those defines. Pins: TXD=GPIO3, RXD=GPIO4,
  modem EN=GPIO5, LED=GPIO8.
- **Modem compatibility**: the whole ML307 family (ML307R / ML307A / "DC" variants) shares the same
  China-Mobile AT command set, so PDU SMS / CNMI / CMGL / CMGS / CMGD / CEREG / CUSD all work
  unchanged. The only model branch is `if (model == "ML307Y") need_set_CGACT = false`. Connect via
  the modem's **UART** pads (not its USB connector) and wire the EN/power pad to GPIO5 for hardware
  power-cycle recovery (soft `AT+CFUN=1,1` reset works even without it).
- **Receive robustness**: reception uses both the `+CMT` URC path AND a 60s `AT+CMGL` storage poll
  (`smsReceiveWatchdogTick`) that re-asserts CNMI — this is what prevents the "after ~3 days only
  outbound works" failure when the modem drops URC routing. fnv1a dedup makes the dual path safe.
