# Phone Call & SMS System — Implementation Plan

## Context

Add phone call and SMS support to the OASIS ecosystem via the Waveshare SIM7600G-H 4G modem (USB, SIMCom chipset, US Mobile SIM). The modem is connected to the Jetson with 5 serial ports (`/dev/ttyUSB0-4`) and provides tertiary internet via RNDIS (`usb0`, route-metric 20100). A crossover cable from the modem's 3.5mm audio jack to a dedicated USB sound card on the Jetson provides call audio. During calls, DAWN's main mic continues wake-word listening so the user can say "Friday, hang up" etc.

**Scope**: Voice calls, SMS, call/SMS history DB, HUD notifications, LLM tool, TTS announcements for incoming events.

---

## Architecture — Two-Daemon Design

The modem logic is split between a standalone daemon (**ECHO**) and DAWN, following the OASIS pattern of dedicated daemons per hardware domain with MQTT as the bus. This keeps DAWN focused on AI/voice/tools and avoids burdening it with serial I/O and AT command parsing.

```
DAWN (AI assistant)                     ECHO (modem daemon)
  phone_tool.c  ── MQTT ──────────────>  oasis-echo
    |                echo/cmd               |
  phone_service.c <── MQTT ─────────────  AT commands
    |                echo/events            |
    |                echo/telemetry         |
  phone_db.c                            SIM7600G-H
    |                                   /dev/ttyUSB2
  audio bridge
    |
  USB sound card <── crossover cable ── modem 3.5mm jack
```

**ECHO** (`~/code/The-OASIS-Project/echo/`): Owns the serial port, handles all AT command traffic, publishes telemetry and events, receives commands via MQTT. ~1,850 lines of C, modeled after STAT.

**DAWN**: Thin `phone_tool.c` (LLM tool interface), `phone_service.c` (call state machine, TTS, HUD, audio bridge), `phone_db.c` (SQLite call/SMS logs). ~800 lines total added.

**MIRAGE**: Subscribes directly to `echo/telemetry` for LTE signal bars on the HUD. No DAWN relay needed.


---

## ECHO Daemon (Standalone)

**Repo**: `~/code/The-OASIS-Project/echo/`
**Binary**: `oasis-echo`
**Template**: STAT (`~/code/The-OASIS-Project/stat/`)

### Responsibilities

- Own `/dev/ttyUSB2` exclusively (flock)
- AT command send/receive with response parsing
- URC reader thread (RING, +CLIP, +CMTI, NO CARRIER, +CREG)
- Signal strength polling (AT+CSQ every 10s)
- Phone number validation (`^[+*#0-9]{1,20}$`)
- SMS body sanitization (strip bytes 0x00-0x1F except newline, reject Ctrl-Z/ESC)
- Hard rate limiting (5 calls/hour, 20 SMS/hour — defense in depth)
- Health monitoring (AT heartbeat, reset on timeout, reconnect on USB disconnect)
- RNDIS data path setup (bundles `sim7600-rndis-up.sh`, previously untracked)

### File Structure

```
echo/
  CMakeLists.txt
  LICENSE, README.md, CLAUDE.md, .clang-format
  config/
    oasis-echo.service
    sim7600-rndis.service           (bundled from /etc/systemd, previously untracked)
    echo.conf
  scripts/
    sim7600-rndis-up.sh             (bundled from /usr/local/sbin, previously untracked)
  install.sh
  include/
    at_command.h                    AT command send/receive API
    logging.h                       Copy from STAT
    modem.h                         Modem state, init, shutdown, signal polling
    mqtt_comms.h                    MQTT init, publish, subscribe, command dispatch
    echo.h                          Global types, version, config struct
    sms.h                           SMS text-mode helpers, phone number validation
    urc_handler.h                   URC reader thread, event classification
  src/
    at_command.c       (~350 lines)
    logging.c          (copy from STAT)
    modem.c            (~400 lines)
    mqtt_comms.c       (~300 lines)
    oasis-echo.c       (~350 lines)
    sms.c              (~200 lines)
    urc_handler.c      (~250 lines)
  tests/
    unity/
      unity.h                       Unity test framework (vendored, MIT license)
      unity.c
      unity_internals.h
    test_at_command.c               AT response parsing, timeout handling
    test_sms.c                      Phone number validation, SMS body sanitization
    test_urc_handler.c              URC classification, RING+CLIP merge, state machine
    test_mqtt_messages.c            OCP message format, JSON construction/parsing
```

### MQTT Topics

| Topic | Dir | QoS | Retain | Content |
|-------|-----|-----|--------|---------|
| `echo/telemetry` | out | 0 | true | Signal, network state, call state (every 10s) |
| `echo/events` | out | 1 | false | Incoming call, SMS received, call ended |
| `echo/response` | out | 1 | false | Command responses with `request_id` |
| `echo/status` | out | 1 | true | Component status online/offline (LWT) |
| `echo/cmd` | in | — | — | Commands from DAWN |

All messages conform to OCP v1.4 (`docs/OASIS_COMMUNICATIONS_PROTOCOL.md`).

### Message Formats

**Telemetry** (`echo/telemetry`, every 10s):
```json
{
  "device": "echo",
  "signal_dbm": -67,
  "signal_bars": 4,
  "csq": 18,
  "registration": "registered_home",
  "operator": "US Mobile",
  "network_type": "LTE",
  "call_state": "idle",
  "sim_status": "ready",
  "timestamp": 1744300000
}
```

**Events** (`echo/events`):
```json
{"device":"echo","event":"incoming_call","number":"+15551234567","timestamp":1744300000}
{"device":"echo","event":"call_connected","timestamp":1744300020}
{"device":"echo","event":"call_ended","reason":"remote_hangup","duration_sec":45,"timestamp":1744300065}
{"device":"echo","event":"sms_received","index":3,"sender":"+15551234567","body":"Hey are you free?","timestamp":1744300000}
{"device":"echo","event":"modem_lost","timestamp":1744300000}
{"device":"echo","event":"modem_reconnected","timestamp":1744300030}
```

**Commands** (`echo/cmd`) — OCP format:
```json
{"device":"echo","action":"dial","value":"+15551234567","request_id":"worker_0_42","timestamp":1744300000}
{"device":"echo","action":"answer","request_id":"worker_0_43","timestamp":1744300000}
{"device":"echo","action":"hangup","request_id":"worker_0_44","timestamp":1744300000}
{"device":"echo","action":"send_sms","value":"+15551234567","data":{"type":"text/plain","encoding":"utf8","content":"On my way"},"request_id":"worker_0_45","timestamp":1744300000}
{"device":"echo","action":"read_sms","value":"3","request_id":"worker_0_46","timestamp":1744300000}
{"device":"echo","action":"delete_sms","value":"3","request_id":"worker_0_47","timestamp":1744300000}
{"device":"echo","action":"signal","request_id":"worker_0_48","timestamp":1744300000}
{"device":"echo","action":"dtmf","value":"1","request_id":"worker_0_49","timestamp":1744300000}
{"device":"echo","action":"query_call","request_id":"worker_0_50","timestamp":1744300000}
```

**Responses** (`echo/response`) — OCP format:
```json
{"device":"echo","action":"dial","request_id":"worker_0_42","status":"success","timestamp":1744300000}
{"device":"echo","action":"dial","request_id":"worker_0_42","status":"error","error":{"code":"NO_CARRIER","message":"Call failed: no carrier"},"timestamp":1744300005}
{"device":"echo","action":"signal","request_id":"worker_0_48","status":"success","value":"{\"signal_dbm\":-67,\"csq\":18}","timestamp":1744300000}
```

### Threading Model

Two threads (plus mosquitto's background thread):

```
Main Thread                         URC Reader Thread
    |                                     |
    |-- main loop (while g_running)       |-- blocking read() on serial fd
    |   - poll telemetry (every 10s)      |   - parse complete lines
    |   - MQTT on_message callback        |   - classify: URC vs AT response
    |     dispatches commands via          |   - URC → mqtt_publish(echo/events)
    |     at_command_send()               |   - AT response → signal cond_var
    |   - sleep(interval)                 |     back to main thread
    |                                     |
```

**AT command serialization**: A mutex + condition variable. Two command types:
- **Sync** (`AT`, `AT+CSQ`, `AT+CMGR`, etc.): `at_command_send()` acquires the mutex, writes the command, `cond_wait`s for OK/ERROR. URC reader signals the condvar with the result. Timeout: 1-2s for most, 60s for AT+CMGS.
- **Async** (`ATD`, `ATA`): `at_command_send_async()` acquires the mutex, writes the command, releases the mutex immediately. Returns "ok, dialing" to the caller. The URC reader handles the eventual result (`CONNECT`, `NO CARRIER`, `BUSY`) as events published to MQTT.

The URC reader thread owns ALL serial reads and maintains a state machine: it knows which command (if any) is pending and what response terminators to expect. URCs that arrive mid-response are queued and dispatched after the response completes. This single-reader design avoids the race condition of two threads reading the same fd.

**AT+CMGS two-phase protocol**: SMS send requires `>` prompt → body → Ctrl-Z (0x1A). A variant of `at_command_send()` handles this multi-step interaction separately.

### Modem Init Sequence

```
AT              — verify communication
ATE0            — disable echo
AT+CMEE=2       — verbose error messages
AT+CSMP=17,167,0,8  — SMS params: UCS2 DCS (enables emoji)
AT+CLIP=1       — enable caller ID
AT+CMGF=1       — SMS text mode
AT+CPMS="ME","ME","ME" — SMS storage to modem memory
AT+CNMI=2,1,0,0,0  — SMS notification via URC
AT+CREG=1       — network registration URCs
AT+CSDVC=1      — audio to headset jack
AT+CLVL=3       — volume mid-level
AT+CSQ          — initial signal read
AT+COPS?        — initial operator query
```

Note: Modem kept in default UCS2 charset (no `AT+CSCS="GSM"`). Phone numbers and SMS bodies are UCS2 hex-encoded for `AT+CMGS` and decoded from `AT+CMGR`. Echo cancellation (`AT+CECM=1`) is sent per-call on `VOICE CALL: BEGIN`, not at init.

### Existing Network Configuration (DO NOT BREAK)

The modem provides tertiary internet via two NetworkManager connections:

| Interface | Connection | Metric | Priority | Manager |
|-----------|-----------|--------|----------|---------|
| `enP8p1s0` (wired) | Wired connection 1 | 100 | Primary | NM |
| `wlP1p1s0` (WiFi) | TORTUGA | 1000 | Secondary | NM |
| `usb0` (RNDIS) | simcom-rndis | 20100 | Tertiary | NM (no ModemManager) |

1. **`simcom-rndis`** — RNDIS ethernet on `usb0`. autoconnect=yes, route-metric=20100 (lowest priority), MTU 1420. Managed by NetworkManager directly — **does NOT use ModemManager**.

2. **`US Mobile`** — GSM connection (autoconnect=no, APN=`pwg`, SIM=`8901260...`). Uses ModemManager for PPP/QMI. Already disabled. Will stop working when ModemManager is disabled — no impact.

### RNDIS Data Path (Bundled)

A systemd oneshot service `sim7600-rndis.service` runs at boot to activate the data connection:
- **Service**: `/etc/systemd/system/sim7600-rndis.service` (enabled, runs after network-online.target)
- **Script**: `/usr/local/sbin/sim7600-rndis-up.sh`

The script does:
1. Waits for `usb0` interface (30s)
2. Creates/updates `simcom-rndis` NM profile (route-metric 20100, MTU 1420)
3. Brings up the NM connection
4. Waits for AT port (`/dev/ttyUSB2` or `/dev/ttyUSB3`, 40s)
5. Sends AT commands to activate RNDIS data: `AT+CGATT=1`, `AT+CGDCONT=1,"IPV4V6","fast.t-mobile.com"`, `AT+NETOPEN`
6. Runs `dhclient` on `usb0`, sets MTU and DNS

These were previously untracked (manually installed to `/etc/systemd/system/` and `/usr/local/sbin/`). Now bundled in the ECHO repo under `config/` and `scripts/`.

**ECHO must coordinate with this script** — it uses AT commands on the same serial port at boot:
- **Phase 1** (simplest): ECHO starts after the RNDIS script completes (`After=sim7600-rndis.service`). ECHO then takes exclusive ownership via flock.
- **Future**: Absorb RNDIS setup into ECHO's init sequence. ECHO sends `AT+NETOPEN` as part of its own modem init. Retire the standalone script.

**Known issue** (2026-04-11): `simcom-rndis` is not active — `usb0` has a link-local address (`169.254.x.x`) and the NM connection shows no device. The route table has a fallback `default dev usb0 scope link metric 1015` but no real gateway. The boot script may be failing silently or the modem data path drops after init. **Verify and fix RNDIS data connectivity before or during Phase 1.** This is another argument for ECHO absorbing the setup — it can monitor the connection and re-establish it if it drops.

**Safe to disable ModemManager**: RNDIS uses plain ethernet on `usb0` via NetworkManager. ModemManager is not involved.

### Config and Deployment

**`config/echo.conf`**:
```conf
MQTT_HOST=localhost
MQTT_PORT=8883
MQTT_USERNAME=echo
MQTT_PASSWORD=changeme
MQTT_TLS=true
MQTT_CA_CERT=/etc/mosquitto/certs/ca.crt
SERIAL_PORT=/dev/ttyUSB2
SERIAL_BAUD=115200
TELEMETRY_INTERVAL=10
RATE_LIMIT_CALLS_PER_HOUR=5
RATE_LIMIT_SMS_PER_HOUR=20
```

**`config/oasis-echo.service`**:
```ini
[Unit]
Description=OASIS ECHO - Enhanced Cellular Handling Operations
After=network.target mosquitto.service sim7600-rndis.service
Wants=mosquitto.service

[Service]
Type=simple
ExecStart=/usr/local/bin/oasis-echo --service \
  --mqtt-host ${MQTT_HOST} --mqtt-port ${MQTT_PORT} \
  --serial-port ${SERIAL_PORT} --serial-baud ${SERIAL_BAUD}
EnvironmentFile=/etc/oasis/echo.conf
Restart=on-failure
RestartSec=5s
User=oasis
Group=oasis
ProtectSystem=full
ProtectHome=true
PrivateTmp=true
NoNewPrivileges=true
SupplementaryGroups=dialout

[Install]
WantedBy=multi-user.target
```

---

## ECHO Testing (Unity Framework)

ECHO uses [Unity](https://github.com/ThrowTheSwitch/Unity) (MIT license) as its test framework — a pure C, single-file test harness designed for embedded systems. Two files (`unity.c`, `unity.h`) are vendored into `tests/unity/`. No external dependencies.

### Test Targets

Built via CMake `enable_testing()` + `add_test()`. Each test is a standalone binary:

```bash
# Build and run all tests
cmake -B build -DCMAKE_BUILD_TYPE=Debug && make -C build -j8
ctest --test-dir build --output-on-failure

# Run individual test
./build/tests/test_sms
```

### Test Modules

**`test_at_command.c`** — AT command layer (no hardware needed):
- Response parsing: extract data lines, detect OK/ERROR/+CME ERROR terminators
- Timeout handling: verify timeout returns correct status
- Command formatting: verify AT string construction
- Sync vs async command classification

**`test_sms.c`** — SMS and phone number validation:
- Valid numbers: `+15551234567`, `*67+15551234567`, `#31#`, `911`
- Invalid numbers: empty, too long (>20), letters, semicolons, backticks
- SMS body sanitization: Ctrl-Z (0x1A) stripped, ESC (0x1B) stripped, newlines preserved
- SMS body length enforcement (800 byte max)
- AT command injection attempts rejected

**`test_urc_handler.c`** — URC classification and state machine:
- Classify known URCs: RING, +CLIP, +CMTI, NO CARRIER, BUSY, +CREG
- RING + CLIP merge: CLIP arrives within 300ms → combined event with number
- RING without CLIP: timeout → event with empty number (blocked caller ID)
- URCs during pending AT command: queued, dispatched after response completes
- Unknown lines: ignored gracefully

**`test_mqtt_messages.c`** — OCP message format:
- Telemetry JSON construction: all required fields present, valid types
- Event JSON: `device`, `event`, `timestamp` fields
- Response JSON: `request_id` echoed, `status` field, `error` object format
- Command parsing: extract `action`, `value`, `request_id`, `data` from JSON

### CMake Integration

```cmake
# In CMakeLists.txt
option(BUILD_TESTS "Build unit tests" ON)

if(BUILD_TESTS)
   enable_testing()

   # Unity test framework (vendored)
   add_library(unity STATIC tests/unity/unity.c)
   target_include_directories(unity PUBLIC tests/unity)

   # Test executables
   foreach(test_name test_at_command test_sms test_urc_handler test_mqtt_messages)
      add_executable(${test_name} tests/${test_name}.c)
      target_link_libraries(${test_name} unity ${JSONC_LIBRARIES})
      target_include_directories(${test_name} PRIVATE include ${JSONC_INCLUDE_DIRS})
      add_test(NAME ${test_name} COMMAND ${test_name})
   endforeach()
endif()
```

Tests link against Unity and the specific source files they exercise (not the full daemon binary). This allows testing AT parsing, SMS validation, and URC classification without a serial port or MQTT broker.

---

## DAWN Integration (Phone Tool + Service)

### New Files (3 source + 1 test)

#### 1. `include/tools/phone_tool.h` + `src/tools/phone_tool.c` (~350 lines)

Single tool, multiple actions — follows `email_tool.c` pattern.

**Actions**: `call`, `confirm_call`, `answer`, `hang_up`, `send_sms`, `confirm_sms`, `read_sms`, `call_log`, `sms_log`, `status`

**Metadata**:
```c
.name = "phone",
.device_string = "phone",
.topic = "dawn",
.aliases = {"telephone", "call", "sms", "text"},
.capabilities = TOOL_CAP_NETWORK | TOOL_CAP_DANGEROUS,
.init = phone_tool_init,
.cleanup = phone_tool_cleanup,
.is_available = phone_tool_available,
```

**Configurable two-step confirmation**:
- `confirm_outbound = true` (default): `call` returns "About to call Mom at +1555..., say confirm." Then `confirm_call` publishes `dial` to ECHO. Same for SMS.
- `confirm_outbound = false`: Execute immediately.

**MQTT integration**:
- Publishes OCP commands to `echo/cmd` with `request_id` via `command_router`
- `phone_tool_init()` subscribes to `echo/events` and `echo/response`
- Event handler routes to `phone_service` for state machine processing

#### 2. `include/tools/phone_service.h` + `src/tools/phone_service.c` (~500 lines)

**State**: `PHONE_STATE_IDLE`, `DIALING`, `RINGING_IN`, `ACTIVE`, `HANGING_UP`

**Functions**:
- `phone_service_init()` / `phone_service_shutdown()` / `phone_service_available()`
- `phone_service_call(user_id, name_or_number)` — resolve contact, publish dial to ECHO
- `phone_service_answer(user_id)` / `phone_service_hangup(user_id)`
- `phone_service_send_sms(user_id, name_or_number, body)`
- `phone_service_get_unread_sms(user_id, out, max)`
- `phone_service_call_log(user_id, count, out, max)` / `phone_service_sms_log(...)`
- `phone_service_get_state()` / `phone_service_get_signal()`
- `phone_service_handle_event(event_json)` — called when ECHO publishes to `echo/events`

**Call state machine** protected by `state_mutex`.

**Event handling** (from ECHO via MQTT):
- `incoming_call` → set state=RINGING_IN, reverse-lookup contact, insert call log (missed initially), send HUD MQTT, TTS announce
- `call_connected` → set state=ACTIVE, start audio bridge
- `call_ended` → calculate duration, update call log, stop audio bridge, set state=IDLE, send HUD MQTT
- `sms_received` → reverse-lookup sender, insert SMS log, publish `delete_sms` to ECHO (after DB commit), send HUD MQTT, TTS announce
- `modem_lost` → if call active, log as failed, stop audio bridge, set state=IDLE

**Contact resolution**: `contacts_find(user_id, input, "phone", results, 5)` from existing `contacts_db.c`.

**Reverse lookup for incoming**: Query contacts by phone number value to resolve display name.

**+CLIP sanitization**: Strip non-printable chars, validate number pattern, cap lengths.

**SMS content safety**: Prefix incoming SMS body with `[Incoming SMS - external content]` for LLM.

**TTS announcements**: `text_to_speech(strdup(text))` — suppress during active call.

**HUD MQTT dispatch** via `worker_pool_get_mosq()` to topic `"hud"`:
```json
{"device":"phone","action":"incoming_call","event_id":"ph-001","number":"+15551234","name":"Mom","initials":"M","ring_timeout":30,"timestamp":1710345600}
{"device":"phone","action":"call_active","event_id":"ph-001","number":"+15551234","name":"Mom","timestamp":1710345620}
{"device":"phone","action":"call_ended","event_id":"ph-001","number":"+15551234","name":"Mom","duration":142,"reason":"completed"}
{"device":"phone","action":"sms_received","event_id":"ph-003","number":"+15555678","name":"Jane","initials":"J","preview":"Hey are you free tom...","body_length":142,"priority":"normal","ttl":15}
```

**Rate limiting** (UX layer): Track last N outbound actions with timestamps. Defaults: max 5 SMS/minute, max 3 dial attempts/minute, max 30 SMS/day. Configurable. Refuse and log when exceeded. (ECHO has its own hard limits as defense in depth.)

**Cross-topic ordering**: Must handle `call_connected` event arriving before `dial` response (different MQTT topics, no ordering guarantee).

#### 3. `include/tools/phone_db.h` + `src/tools/phone_db.c` (~350 lines)

Uses Pattern A (shared `auth_db` handle, same as `calendar_db.c`).

**Tables**:
```sql
CREATE TABLE IF NOT EXISTS phone_call_log (
   id INTEGER PRIMARY KEY AUTOINCREMENT,
   user_id INTEGER NOT NULL,
   direction INTEGER NOT NULL,        -- 0=outgoing, 1=incoming
   number TEXT NOT NULL,
   contact_name TEXT DEFAULT '',
   duration_sec INTEGER DEFAULT 0,
   timestamp INTEGER NOT NULL,
   status INTEGER NOT NULL            -- 0=answered, 1=missed, 2=rejected, 3=failed
);
CREATE INDEX idx_phone_call_user_ts ON phone_call_log(user_id, timestamp DESC);

CREATE TABLE IF NOT EXISTS phone_sms_log (
   id INTEGER PRIMARY KEY AUTOINCREMENT,
   user_id INTEGER NOT NULL,
   direction INTEGER NOT NULL,
   number TEXT NOT NULL,
   contact_name TEXT DEFAULT '',
   body TEXT NOT NULL,
   timestamp INTEGER NOT NULL,
   read INTEGER DEFAULT 0
);
CREATE INDEX idx_phone_sms_user_ts ON phone_sms_log(user_id, timestamp DESC);
CREATE INDEX idx_phone_sms_unread ON phone_sms_log(user_id, read) WHERE read = 0;
```

Schema version bump (add to `auth_db_internal.h`).

#### 4. `tests/test_phone_db.c`

Unit tests for DB layer. Follow `test_scheduler` pattern.

### Files to Modify (5)

1. **`include/auth/auth_db_internal.h`** — prepared statements, schema version bump
2. **`src/auth/auth_db_core.c`** — migration: CREATE TABLE phone_call_log + phone_sms_log
3. **`include/config/dawn_config.h`** — add `phone_config_t` struct
4. **`cmake/DawnTools.cmake`** — add `DAWN_ENABLE_PHONE_TOOL` option
5. **`src/tools/tools_init.c`** — add registration call

### dawn.toml Addition

```toml
[phone]
enabled = true
confirm_outbound = true
audio_device = ""                  # USB sound card device (e.g. "hw:2,0")
sms_retention_days = 90
call_log_retention_days = 90
rate_limit_sms_per_min = 5
rate_limit_calls_per_min = 3
rate_limit_sms_per_day = 30
```

---

## Phone Audio Bridge

### Overview

A dedicated `phone_audio_bridge` thread in `phone_service.c` handles bidirectional PCM streaming between the USB sound card (modem analog audio) and the active client.

### Audio Flow

```
                    Crossover Cable
  SIM7600G-H  ──────────────────────>  USB Sound Card
  3.5mm jack   <──────────────────────  (audio device)
  (mic + spk)                           (capture + playback)
                                              |
                                    audio_backend API
                                   (2nd capture + playback handle)
                                              |
                                    phone_audio_bridge thread
                                     (resample + route)
                                              |
                          ┌───────────────────┼───────────────────┐
                          |                   |                   |
                    Local client         WebUI client        Satellite
                   (primary audio       (WebSocket Opus     (DAP2 audio
                    playback/capture)    0x11/0x12 out,      session)
                                         0x01/0x02 in)
```

### Bridge Thread Lifecycle

1. **Start** (on `call_connected` or successful `answer`):
   - Pause TTS: `tts_playback_state = TTS_PLAYBACK_PAUSE`
   - Pause music: `setMusicPlay(0)`
   - Open USB sound card: `audio_stream_capture_open()` + `audio_stream_playback_open()`
   - Create resamplers if sample rates differ

2. **Run** (bidirectional loop, ~20ms chunks):
   - Modem → client: read USB sound card, resample, route to active client
   - Client → modem: receive from active client, resample, write to USB sound card

3. **Stop** (on `call_ended`):
   - Close USB sound card streams
   - Destroy resamplers
   - Resume TTS and music

### Client Routing

- **Local** (helmet): Bridge USB sound card ↔ primary playback/capture device
- **WebUI** (browser): Encode/decode via `webui_opus_encode_stream()`/`webui_opus_decode_stream()`, send as binary WebSocket messages
- **Satellite**: Same binary audio path as WebUI

### Wake Word During Calls

Primary mic capture continues independently during calls. The user's voice goes to both the call bridge AND the ASR path. "Friday, hang up" triggers wake word → ASR → LLM → `phone_tool action=hang_up`.

---

## Hardware Notes

- **Modem**: Waveshare SIM7600G-H 4G HAT, USB ID `1e0e:9011`
- **Serial ports**: `/dev/ttyUSB0-4` (all `option1` driver). ttyUSB2 = AT command port (primary)
- **Network**: RNDIS via `usb0` (route-metric 20100, MTU 1420, tertiary behind wired at 100 and WiFi at 1000)
- **Audio**: 3.5mm analog jack only (`AT+CPCMREG=1` returns ERROR — no USB PCM)
- **SIM**: US Mobile (T-Mobile MVNO), APN `fast.t-mobile.com`

---

## Data Flows

**Outgoing call (with confirm)**: User says "call Mom" → LLM → phone_tool `action=call` → contact lookup → return "About to call Mom, confirm?" → User says "yes" → LLM → `action=confirm_call` → phone_service publishes `{"action":"dial","value":"+1555..."}` to `echo/cmd` → ECHO sends `ATD+1555...` → ECHO publishes `call_connected` on `echo/events` → phone_service starts audio bridge → HUD MQTT

**Incoming call**: ECHO URC reader detects RING + +CLIP → publishes `incoming_call` on `echo/events` → DAWN phone_service → reverse lookup → state=RINGING_IN → insert call log (missed initially) → HUD MQTT → TTS "Incoming call from Mom" → User says "answer" → LLM → phone_service publishes `{"action":"answer"}` to `echo/cmd` → ECHO sends `ATA` → ECHO publishes `call_connected` → phone_service starts audio bridge

**Incoming SMS**: ECHO detects +CMTI → auto-reads SMS → publishes `sms_received` with body on `echo/events` → DAWN phone_service → reverse lookup → sanitize body → `phone_db_sms_log_insert()` → on success: publish `delete_sms` to ECHO → HUD MQTT → TTS "New text from Jane"

---

## Implementation Order

### Phase 1: ECHO Skeleton + Serial ✓
Create repo, serial I/O, AT command/response. Verify: send `AT`, get `OK`.

### Phase 2: ECHO URC Thread + Modem Init ✓
URC reader, init sequence, signal polling. Verify: RING events in console.

### Phase 3: ECHO MQTT Integration ✓
MQTT connect/subscribe/publish, wire commands/events, SMS, telemetry. Verify: `mosquitto_pub` commands work.

### Phase 4: DAWN Integration + Deployment ✓
phone_tool.c, phone_service.c, phone_db.c, config, systemd. MIRAGE LTE signal bars. Verify: voice command "call Mom" flows end-to-end.

**Phase 4.5: Ring Events, Ringtone, LLM Context, Voicemail Detection** ✓ (April 2026)
- ECHO: ring broadcast on each RING URC, ring timeout watchdog (AT+CLCC poll after 10s), CMTI dedup, hangup publishes call_ended directly, ms timestamps in logging
- DAWN: Iron Man ringtone (NES pulse synthesis via audio_backend), ring event forwarding to HUD, LLM context injection for all phone events, SMS delete fire-and-forget (fixes MQTT callback self-deadlock), configurable phone service user_id
- MIRAGE: notification system (state machine, config-driven elements, base64 photo decode, fade in/out), contact photo display from MQTT

### Phase 5: Audio Bridge + End-to-End
Audio bridge thread, crossover cable + USB sound card setup, bidirectional audio across client types, SMS end-to-end, rate limiting, edge cases.

### Phase 5.5: PDU Mode + Concatenated SMS

Switch ECHO from text mode (`AT+CMGF=1`) to PDU mode (`AT+CMGF=0`) for SMS send. Text mode limits UCS2 messages to 70 characters — the modem returns `CMS ERROR` for anything longer because it cannot auto-concatenate in text mode. Verified 2026-04-15.

**How cell phones handle this**: PDU mode gives the sender full control over the SMS binary frame. Long messages are split into segments with a UDH (User Data Header) that carries a reference ID, segment count, and segment index. The receiving phone reassembles segments transparently. Each segment carries 67 UCS2 characters or 153 GSM7 characters (slightly less than single-SMS limits because the UDH consumes a few bytes).

**Required work (ECHO side)** — new `src/pdu.c` module (~200-300 lines):
- PDU frame builder: destination number in BCD, TP-MTI/TP-MR/TP-DA/TP-PID/TP-DCS/TP-UDL/TP-UD fields
- Data coding scheme selection: GSM7 (DCS=0x00) for ASCII-only bodies, UCS2 (DCS=0x08) when non-ASCII present
- Concatenation: split body into segments, prepend UDH (IE 0x00: ref_id, total, seq) to each segment
- Send each segment as a separate `AT+CMGS=<pdu_length>` in PDU mode
- Update `modem_init()`: change `AT+CMGF=1` to `AT+CMGF=0`
- SMS receive also switches to PDU mode: parse incoming PDU frames, extract sender/body/UDH

**Benefit**: ASCII messages get 153 chars/segment (up from 70 in UCS2 text mode). Unicode messages get 67 chars/segment with automatic concatenation instead of hard failure. Multi-segment messages work transparently.

**Complexity**: Medium. PDU encoding is well-documented (3GPP TS 23.040). The main work is the frame builder and segment splitter. SMS receive parsing is straightforward since we already handle UCS2 decoding.

### Phase 6: MMS Support (Future)
Receive and send multimedia messages (images, video, contacts). The SIM7600G-H supports MMS via AT commands (`AT+CMMSINIT`, `AT+CMMSDOWN`, `AT+CMMSVIEW`, `AT+CMMSSEND`). MMS uses a WAP/HTTP data channel to the carrier's MMSC (Multimedia Messaging Service Center) — the image payload is not delivered inline like SMS text.

**Current behavior**: MMS notifications arrive as empty SMS (blank sender and body via `+CMTI` / `AT+CMGR`). ECHO handles this gracefully (no crashes), but the image content is lost. Verified 2026-04-14.

**Required work (ECHO side)**:
- MMS init: `AT+CMMSINIT` to start MMS service, configure MMSC URL and WAP gateway (carrier-specific: T-Mobile MMSC is `http://mms.msg.eng.t-mobile.com/mms/wapenc`)
- MMS receive: detect MMS notification in `+CMTI`, fetch from MMSC via `AT+CMMSDOWN`, parse MIME multipart body (extract image + text parts)
- MMS send: compose MIME multipart, upload via `AT+CMMSSEND`
- Image storage: save received images to filesystem, reference by path in MQTT events
- New MQTT event fields: `"media_type": "image/jpeg"`, `"media_path": "/var/lib/echo/mms/..."`, `"media_size": 45320`

**Required work (DAWN side)**:
- `phone_service.c`: handle `sms_received` events with `media_path` — download or reference the image
- `phone_tool.c`: display image info to LLM, optionally pass to vision API for description
- WebUI: display MMS images in conversation/notification

**Complexity**: High. MMS involves HTTP transactions over the modem's data connection, MIME parsing, carrier-specific MMSC configuration, and image storage lifecycle. Consider as a separate project phase after audio bridge is complete.

### Phase 7: Smart Contact Resolution (Future)

Improve contact resolution to support natural language references ("my daughter", "the electrician") and multi-result disambiguation.

**Current behavior**: `resolve_number()` does a direct `contacts_find()` LIKE search on entity name. Works for exact names ("Kris", "Mom") but fails for relational references ("my daughter") and silently picks the first result when multiple contacts match.

**Required work**:

1. **Memory-aware resolution**: When `contacts_find()` returns zero results, query the memory system (entity relations, facts) to resolve references like "my daughter" → entity "Sarah" → phone contact. This leverages existing memory_db relations (e.g., `Kris → has_daughter → Sarah`).

2. **Multi-result disambiguation**: When `contacts_find()` returns multiple phone contacts for an ambiguous query:
   - Return all matches to the LLM with an index: "Multiple contacts found: 1. Sarah Kersey (+1555...) 2. Emily Kersey (+1555...). Which one?"
   - Add a `select_contact` action or extend `confirm_call`/`confirm_sms` to accept an index
   - LLM asks the user and re-calls with the selected index

3. **Relation-based lookup flow**:
   ```
   User: "Call my daughter"
   → resolve_number("my daughter") → contacts_find returns 0
   → memory_db query: find entities related to user with relation "daughter"
   → returns [Sarah, Emily]
   → look up phone contacts for each entity
   → if 1 match: proceed to confirm
   → if 2+ matches: return disambiguation list to LLM
   → if 0 matches: "No phone number found for your daughter(s)"
   ```

4. **Fuzzy matching**: Consider matching on entity aliases, nicknames, and partial names (e.g., "Chris" matching "Christopher").

**Complexity**: Medium. The memory system and contacts DB already exist — this is primarily logic in `phone_service.c`'s `resolve_number()` and a new disambiguation response format in `phone_tool.c`.

---

## LLM Message Deletion — SHIPPED (April 2026)

The original tech-debt gap (no way for the LLM to delete SMS/call records) is resolved.

**Implementation**:

- `phone_db.c` / `phone_db.h`: new `PHONE_DB_SUCCESS`/`FAILURE`/`NOT_FOUND` return codes, seven new functions — `phone_db_sms_log_delete`, `_delete_by_number`, `_count_by_number`, `phone_db_call_log_delete`, `_delete_older_than`, `_count_older_than` — plus a shared `phone_number_normalize()` helper so LLM-supplied formats (`"+1-555-..."`, `"(555) ..."`, bare 10-digit US) match stored E.164 rows. Every DELETE is `user_id`-scoped.
- `phone_tool.c`: four new actions — `delete_sms`, `confirm_delete_sms`, `delete_call`, `confirm_delete_call`. Two-step confirmation with a **120-second TTL** on pending state plus a per-user delete rate limit (10/hour default). Matches the `send_sms`/`confirm_sms` and email-tool pattern — the LLM previews, the user says "confirm" on the next turn. TTL bounds replay windows; rate limit caps prompt-injection-coerced bulk deletes.
- `phone_sms_log_t.body` widened from `[1024]` → `[2048]` so multi-segment inbound SMS (from the upcoming ECHO PDU work) doesn't truncate. Stack cost held constant by capping recent-query arrays at 10 entries (down from 20) in `handle_read_sms`/`handle_sms_log`.
- SMS segment-hint added to the `send_sms` confirmation prompt — "This will send as N text messages" when the body exceeds one segment. Gated on `warn_on_multi_segment` config (default `true`).
- Delete criteria in v1: `id`, `number`, or `older_than_days` — exactly one required. No by-contact-name (LLM pre-resolves). Bulk older-than-days is wired for the **call log** but not yet for SMS (follow-up).

**Still open** — tracked in `docs/TODO.md`:
- Back-port the 120s TTL to existing `send_sms`/`confirm_sms`.
- Migrate all `phone_tool` pending state from globals to session-scoped.
- Clean up `phone_db.c`'s legacy `-1` error returns across insert/update/query.
- SMS bulk delete by `older_than_days`.
- WebUI deletion parity (deferred — only build if a user asks).

---

## Verification Checklist

1. `oasis-echo` starts, opens `/dev/ttyUSB2`, runs init sequence
2. Signal strength appears in `echo/telemetry` every 10s
3. MIRAGE shows LTE signal bars on HUD
4. `mosquitto_pub` commands (dial, hangup, send_sms) work
5. Incoming call triggers `echo/events` with caller ID
6. DAWN voice command "call X" flows through MQTT to modem
7. Audio bridge works for calls (local speaker, WebUI, satellite)
8. SMS send/receive/log works end-to-end
9. Modem disconnect → `modem_lost` event → reconnect on replug
10. `systemctl start/stop/restart oasis-echo` works correctly
11. RNDIS data connectivity verified through modem route

---

## Security Considerations

- **MQTT auth mandatory**: TLS + username/password. Mosquitto ACL restricts `echo/cmd` to DAWN's identity only.
- **AT command injection**: Phone number validation (`^[+*#0-9]{1,20}$`) at both ECHO and DAWN.
- **SMS body Ctrl-Z injection**: ECHO strips bytes 0x00-0x1F (except newline), rejects 0x1A/0x1B. Max 800 bytes.
- **Rate limiting (two layers)**: DAWN limits for UX (5 SMS/min, 3 calls/min). ECHO limits for abuse prevention (5 calls/hour, 20 SMS/hour).
- **Serial port security**: `flock(LOCK_EX)` for exclusive access. Validate port path matches `/dev/ttyUSB*` or `/dev/ttyACM*`.
- **SMS content safety**: Incoming SMS body prefixed with `[Incoming SMS - external content]` for LLM.
- **CLIP sanitization**: Strip control chars, validate format, cap lengths before use in TTS/MQTT/DB.

---

## Key Reusable Code (DAWN)

| What | Where | How |
|------|-------|-----|
| Contact lookup | `contacts_find()` in `src/memory/contacts_db.c` | Resolve "Mom" → phone number |
| MQTT publish | `worker_pool_get_mosq()` in `include/core/worker_pool.h` | HUD events (NULL-check!) |
| Command router | `command_router_register/wait/deliver` in `include/core/command_router.h` | Sync MQTT request/response with ECHO |
| TTS announce | `text_to_speech(strdup(text))` | Announce incoming calls/SMS |
| Tool registration | `tool_registry_register()` | Standard tool pattern |
| DB shared handle | `AUTH_DB_*` macros in `include/auth/auth_db_internal.h` | Pattern A for phone_db |
| Audio streams | `audio_stream_capture/playback_open()` in `include/audio/audio_backend.h` | USB sound card for call audio |
| Resampler | `resampler_create/process()` in `include/audio/resampler.h` | Modem 8kHz ↔ 48kHz |
| WebUI Opus | `webui_opus_encode/decode_stream()` in `src/webui/webui_audio.c` | Browser call audio |
| TTS pause | `tts_playback_state` in `include/tts/text_to_speech.h` | Suppress TTS during call |
| Music pause | `setMusicPlay(0/1)` in `src/audio/flac_playback.c` | Pause music during call |
| OCP helpers | `ocp_get_timestamp_ms()` in `include/core/ocp_helpers.h` | Timestamp generation |

---

## Review Findings Incorporated

### Architecture (2 High, 4 Medium)

| Severity | Finding | Resolution |
|----------|---------|------------|
| HIGH | AT response state machine — `ATD` returns nothing until `CONNECT`/`NO CARRIER` (30+ sec). URC reader needs to know which command is pending and what terminators to expect. | State machine in URC reader tracks pending command type. URCs mid-response queued for dispatch after response completes. |
| HIGH | Sync vs async AT commands — `ATD`/`ATA` must not hold mutex for call duration. | Split into sync (hold mutex, wait OK/ERROR) and async (write, release mutex, result arrives as URC event). |
| MEDIUM | RING + CLIP merge timing — +CLIP arrives after RING but timing not guaranteed. | 300ms timer after RING. If CLIP doesn't arrive, publish `incoming_call` with empty number. |
| MEDIUM | AT+CMGS two-phase protocol — SMS send requires `>` prompt → body → Ctrl-Z, not simple send/response. | Variant of `at_command_send()` for multi-step interaction. |
| MEDIUM | DTMF and call query missing from command set. | Added `dtmf` (`AT+VTS`) and `query_call` (`AT+CLCC`) actions. |
| MEDIUM | Cross-topic MQTT ordering — `call_connected` event may arrive before `dial` response. | DAWN's phone_service handles out-of-order events across `echo/response` and `echo/events`. |

### Security (2 Critical, 2 High, 2 Medium)

| Severity | Finding | Resolution |
|----------|---------|------------|
| CRITICAL | MQTT command channel has zero authentication — anyone on the network can dial premium numbers. | MQTT auth + TLS mandatory. Mosquitto ACL restricts `echo/cmd` publish to DAWN's identity only. |
| CRITICAL | SMS body Ctrl-Z injection — body containing `\x1A` terminates SMS and executes arbitrary AT commands. | `sms.c` strips bytes 0x00-0x1F except 0x0A (newline). Reject 0x1A (Ctrl-Z) and 0x1B (ESC). Max 800 bytes. |
| HIGH | No rate limiting in ECHO itself — DAWN's limits can be bypassed via direct MQTT. | Hard limits in ECHO: 5 calls/hour, 20 SMS/hour. Configurable in `echo.conf`. |
| HIGH | Response spoofing via request_id if MQTT unauthenticated. | Mitigated by MQTT ACLs. Also add request_id timeout/matching in DAWN's command_router. |
| MEDIUM | SMS body published to MQTT in cleartext. | Enforce TLS (same fix as auth). |
| MEDIUM | Serial port path from config not validated. | Validate path matches `/dev/ttyUSB*` or `/dev/ttyACM*`. Log warning otherwise. |
