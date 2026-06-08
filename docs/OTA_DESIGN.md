# Server→Satellite OTA Update System — Design

**Status:** Phases 1–2 SHIPPED (server side). Branch `satellite_ota`.
**Date:** 2026-06-06 (design); implementation status updated 2026-06-08. Reviewed by architecture /
embedded-efficiency / security agents (v2 folds in their criticals: sign-raw-bytes,
verify-before-parse, offline signing, ESP32 reboot-to-clean OTA, TOCTOU, server-push spine).

---

## Implementation status (2026-06-08)

Committed on branch `satellite_ota` (4 commits: `7847a3b` Phase 1, `b0cfc8d` satellite build
tooling, `9577b14` manifest core + keytool, `68395af` Phase 2 server engine):

**DONE — server side, fully tested (CI 69/69):**
- **Phase 1** — `firmware_version` + `ota` capability reported at registration; `ota_device_state`
  table (auth_db v59) + `src/core/ota_db.c`; WebUI admin shows per-device firmware. Validated on
  real hardware (dawn-kitchen Tier 1, Office Speaker Tier 2 both report 2.0.0).
- **Satellite build tooling** — CI `satellite-build` job + pre-push hook; `dawn_satellite/`
  Pi-target build container (`Dockerfile.pibase`/`.pibuild` + `build-pi.sh`, GHCR base
  `ghcr.io/the-oasis-project/dawn-satellite-build:trixie-arm64`); fixed two headless-build bugs.
- **Phase 2 #8 manifest core** — `src/core/ota_manifest.{c,h}` (fixed LE binary wire, Ed25519
  verify-before-parse + keyring, SHA-256, numeric semver, anti-rollback). 17 unit tests.
- **Phase 2 #9 offline keytool** — `tools/ota_keytool.c` (`make ota-keytool`): keygen / sign /
  verify / pubkey-header. Validated end-to-end.
- **Phase 2 #10 daemon glue** — `src/core/ota.{c,h}` (release store, resolve, begin_push, token
  download authz + realpath guard, finalize) + `ota_db` state machine (single-flight, one-time
  token, reconcile). `[ota]` config. 9 integration tests (`tests/test_ota.c`).
- **Phase 2 #11 transports** — HTTPS image route (`webui_http.c`, `/api/ota/..`, TLS+token+
  path-hardened), WS device handlers + server→device push (`src/webui/webui_ota.c`), admin-gated
  `ota_push`/`ota_list`, registration finalize hook. WS protocol documented (WEBSOCKET_PROTOCOL.md).

**REMAINING:**
- **#11 deferred follow-ups** (functional path works without them): (a) **dawn-admin CLI** transport
  — reserve admin opcode band `0xC0–0xCF` in `admin_socket.h`, add `admin_socket_ota.c` +
  `dawn-admin ota push/list` calling `webui_ota_push`/`ota_release_*`; (b) **WebUI admin-panel JS**
  buttons (server messages `ota_list`/`ota_push` exist — `www/js/admin/` needs the UI).
- **Phase 3 (RPi apply)** — install-path migration R1 (§7), device-side `abi_tag` check, libcurl
  download + libsodium verify-before-commit (0700, no TOCTOU), binary swap + self-restart,
  boot-count rollback (< systemd StartLimitBurst).
- **Phase 4 (ESP32 apply)** — reboot-to-clean OTA, `partitions.csv` dual-OTA bootstrap (already
  added), `Update` flow + WDT + sector-aligned writes, Ed25519 verify, native partition rollback.
- **Phase 5 (hardening)** — canary-then-rollout for `push all`, audit log.

NOTE: the server can now offer/serve/track updates, but **satellites do not yet apply `ota_offer`**
(that's Phase 3/4). Pushing to a current satellite is harmless (unknown WS type ignored by design).

---

## Context

Rolling new code to satellites is manual today (RPi: `install.sh` stop/copy/restart; ESP32: USB
re-flash) — painful for one device, worse for several. Goal: an operator-triggered OTA system
following embedded-industry security/process standards, across both tiers.

Two tiers, different *apply*, one control plane:
- **Tier 1 (RPi):** Linux binary under systemd. Apply = atomic binary(+libs) swap + self-restart.
- **Tier 2 (ESP32-S3):** Arduino app image. Apply = native dual-partition OTA + bootloader rollback.

**Decided:** control plane on DAP2 WebSocket; **image via HTTPS pull** (auth-gated); **operator-
triggered** (dawn-admin / WebUI) to device/tier/all; **Tier 1 = binary(+libs) only** with rollback.

---

## Trust model

- **Offline signing.** The release **private key never touches the daemon.** Signing happens at
  build/release time off-box (a `dawn-admin ota sign` / standalone tool using libsodium
  `crypto_sign_detached`). The daemon only stores + serves the signed artifacts.
- **Sign raw bytes, not JSON.** The signed object is a **fixed length-prefixed binary manifest**
  (`magic‖fmt_ver‖platform‖tier‖version‖abi_tag‖image_size‖sha256[32]‖min_version`). Device does
  `crypto_sign_verify_detached` over the exact bytes **first**, parses only after it returns 0.
  `abi_tag` carries the build target's OS/ABI identity (see §10) so a device rejects an image not
  built for its runtime.
- **Downgrade policy lives inside the signed manifest** (`min_version`), not the WS offer. The
  operator `allow_downgrade` only selects which signed release to push.
- **Root of trust = a baked-in keyring** (current + next public key) per device, for rotation
  without bricking; dual-sign during overlap; **offline split-custody backup** of private keys.
  Signature compromise is NOT mitigated by rollback. ESP32 has no secure-boot/flash-encryption
  (software-only RoT — a flash-access attacker can swap the pubkey; accepted v1, documented).
- **TLS mandatory for OTA.** Reject `ota_*` and `/api/ota/...` when `!lws_is_ssl(wsi)`; device
  verifies CA + hostname. The image is **integrity-protected, not confidential**; the download
  token is an anti-abuse/DoS gate, not secrecy.

---

## Components

### 1. Versioning + capability reporting (both tiers) — Phase 1 foundation
- Add `firmware_version` + an `ota` capability flag (or `ota_protocol_version`) to the
  `satellite_register` payload (`webui_satellite.c`; Tier 1 send existing `VERSION` `main.c:59`;
  Tier 2 add `#define FIRMWARE_VERSION`). ESP32 also reports running-partition label in status.
- **Server never offers to a device that didn't advertise OTA support;** unknown WS `type` must be
  ignored, not fatal.
- Persist version in a new `ota_device_state` table. Register handler writes `satellite_mappings` +
  `ota_device_state` **under one auth_db lock**.

### 2. Release store + keys (offline)
- Release dir `[ota] release_dir` (default `/var/lib/dawn/ota/<platform>/<version>/`) holding the
  image, signed binary manifest, detached `.sig`. Daemon validates manifests on load, serves them;
  does **not** sign.
- `dawn-admin ota keygen` (offline) emits the device keyring header (`ota_pubkey.h`, current+next)
  + writes the private key to operator-controlled storage; `dawn-admin ota sign` produces
  manifest+sig. Private keys split-custody, off the daemon.

### 3. Server module split (engine + adapters)
- **`src/core/ota_manifest.c`** — pure: build/verify the binary manifest (libsodium
  `crypto_sign_verify_detached` directly — already linked, `CMakeLists.txt:180`), SHA-256, numeric
  semver compare, anti-rollback. No globals/DB/config → host-unit-testable with zero stubs.
- **`src/core/ota.c`** (Layer 2) — daemon glue: release-store I/O, `ota_device_state` via the shared
  auth_db handle + `AUTH_DB_LOCK_*` (precedent: `src/core/missed_notifications_db.c`). **No `webui/`
  include**; server→device push via **callback inversion**.
- **`src/webui/webui_ota.c`** (Layer 4) — WS `ota_offer/ack/reject/status` + HTTPS download route +
  WebUI admin messages.
- **`src/auth/admin_socket_ota.c`** — dawn-admin transport (mirror `admin_socket_messaging.c`).
  Reserve admin opcode band **0xC0–0xCF** (avoid coding-harness 0xB0–0xB8).

### 4. Server→device push delivery (spine)
- `ota push` resolves uuid→session via `session_find_by_uuid` (server-build only; `#ifdef`-stubbed
  local), enqueues `ota_offer` on the session outbound queue, then `lws_callback_on_writable` +
  `lws_cancel_service`. Hold the session lock only to resolve+enqueue; **never hold the auth_db leaf
  lock while taking a session lock.** Offline → `state=offer_pending`, deliver on next register.

### 5. State machine — `ota_device_state(uuid PK, current_version, target_version, state, last_error, token, token_expires, updated_at)`
- **DB = single source of truth.** States: `idle → offered → downloading → verifying → applying →
  rebooting → success | failed | offer_pending`.
- **Single-flight:** reject new push if state ∈ {offered…rebooting} unless `--force-abort`.
- **Restart reconciliation:** transient rows older than a timeout → `unknown`, reconciled on
  reconnect.
- **Register-time finalize / server-owned commit:** on reconnect, reported `firmware_version ==
  target` → `success`. Device does NOT self-validate purely on "I connected."

### 6. WS control messages + download route
- S→C `ota_offer {version, platform, size, sha256, url, token, allow_downgrade}` (advisory;
  authority is the signed manifest).
- C→S `ota_ack` / `ota_reject {reason}` / `ota_status {state, progress, error, running_partition?}`.
- `GET /api/ota/<platform>/<version>/{image,manifest,sig}` — **token is the primary auth gate:**
  one-time (server ledger), bound to authenticated session uuid + version, short TTL, constant-time.
  Path hardening: platform enum whitelist, semver regex, `realpath` prefix vs `release_dir`. IP
  rate-limiter is a coarse DoS guard only; cap/stagger concurrent downloads.

### 7. Tier 1 (RPi) apply
- **ABI check first.** Before download/apply the device verifies the manifest's `abi_tag` matches
  its own runtime (§10) and rejects (`ota_reject{reason:abi_mismatch}`) if not. The marker-commit
  rollback is the backstop if a mismatch slips through (binary fails to start → revert).
- Image = **gzipped** signed `dawn_satellite-<ver>-rpi.tar.gz` (binary + changed libs).
- **R1 install migration (Phase 1 prereq):** run from a **dawn-owned writable dir**
  `/var/lib/dawn-satellite/bin/dawn_satellite`; `bin/`, `ota/rollback/`, `ota/tmp/` under **one**
  `ReadWritePaths` mount (atomic `rename()` needs same fs). `LD_LIBRARY_PATH`/rpath to the dawn-owned
  lib dir — **skip `ldconfig`** (read-only under `ProtectSystem=strict`). Ship in `install.sh`.
- Download via **libcurl**; verify via **libsodium** (`pkg_check_modules(SODIUM REQUIRED libsodium)`).
- **No TOCTOU:** stage to **0700**, hash the exact bytes being committed (verify before final
  `rename`, no re-open). Free-space preflight (< 2.5× image → `no_space`); delete staged tarball
  after extract; one rollback copy.
- **Rollback:** `ota_pending` marker (boot-count) → `exit(0)`; `Restart=always` relaunches. Restore
  the rollback copy at the **top of startup** if marker persists and **boot-count ≥ threshold
  (< systemd `StartLimitBurst=5`)**. Server-owned commit clears the marker on confirmed reconnect.

### 8. Tier 2 (ESP32) apply — reboot-to-clean-state
- **RAM is the gate, not flash.** On accept: `pending_ota` + token in NVS, **reboot**; a minimal-
  allocation boot path (audio buffers not yet `ps_malloc`'d) runs download→verify→flash.
- Hand-author `partitions.csv` with explicit byte math: two ~0x1F0000 app slots + otadata, **NVS at
  stock offset/size** to preserve `uuid`/`recon_sec` across the one-time bootstrap re-flash (or
  document re-provisioning), **no spiffs**. CI/build assertion: `.bin` < ~85% of slot size.
- Stream HTTPS via `WiFiClientSecure` (mbedTLS) into the inactive partition with
  `Update.begin/write/end`; **sector-aligned 4KB writes**; **feed the WDT each chunk**. SHA-256
  during write; **verify Ed25519 sig + hash before setting boot partition.** Verify lib: vendored
  `ed25519-donna` (~10-20KB) or confirmed mbedTLS Ed25519.
- **Rollback:** `esp_ota_set_boot_partition` + reboot; `esp_ota_mark_app_valid_cancel_rollback` only
  after server confirms reported-version==target; `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`. Raw
  `.bin`. Skip resume (~3-9s transfer).

### 9. Config / secrets / DB
- `[ota]` in dawn.toml: `enabled`, `release_dir`, `download_token_ttl_sec`, `require_tls`(=true).
- No private key on the daemon. Device: keyring baked at build.
- DB: `ota_device_state` migration in auth_db schema; audit via existing `auth_log`.

### 10. Build host & ABI compatibility (cross-platform correctness)

The Jetson (daemon host, JetPack/Ubuntu) and a Tier 1 RPi (Raspberry Pi OS) are **different
operating systems**: different `libwebsockets` soname (`.so.16` vs `.so.19`), different glibc, etc.
A Jetson-built aarch64 binary **will not run on the Pi** even though both are arm64. (Confirmed
2026-06-07: a Jetson-built `dawn_satellite` failed on `dawn-kitchen` with
`libwebsockets.so.16: cannot open shared object file`.) So OTA artifacts are **OS/ABI-specific**.

- **Tier 1 release builds run in a Debian arm64 container matching the Pi's distro** (currently
  `debian:trixie-slim` — the target Pis run Debian 13 / trixie, libwebsockets 4.3.5 `.so.19`) —
  never on the Jetson. On an arm64 host the container build is **native (no qemu)**. The artifact
  links against the Pi's runtime; the offline signing step (§2) consumes that container-built
  binary. **Build env is a pre-built base image** `ghcr.io/the-oasis-project/dawn-satellite-build`
  (`Dockerfile.pibase`, public) with the heavy ML deps baked in — ONNX Runtime, libvosk, espeak-ng
  (rhasspy fork), piper-phonemize — at the **same pinned versions as `scripts/lib/libs.sh`** so the
  container's `/usr/local/lib` matches the Pi's (a version skew is the same runtime break as the
  soname). Built once, pushed, reused; rebuild + re-push only when a pinned dep or the base distro
  changes. whisper.cpp is NOT baked in (repo submodule, compiled per-build). The dev tool
  `dawn_satellite/build-pi.sh` builds from the live working tree (no commit): `SAT_BUILD=full`
  (default) uses this base for a deployable voice satellite, `SAT_BUILD=headless` uses the apt-only
  `Dockerfile.pibuild` for connect/register/OTA-control tests. Phase 2 adds offline signing on top.
  **The base must track the Pi's distro** — bump when the Pi is upgraded (stock bookworm links
  `.so.17`, wrong for a trixie Pi; verified 2026-06-07).
- **`abi_tag` in the signed manifest** identifies the build target — a coarse, device-computable
  string: `<os_id>-<os_version_codename>-<arch>` from `/etc/os-release` + `uname -m`
  (e.g. `debian-trixie-aarch64`). The container stamps it at build; the device computes its own at
  apply time. (Granularity can tighten later to include a `libwebsockets` soname / glibc floor if
  coarse distro matching proves insufficient.)
- **Device-side check before flash:** reject (`ota_reject{reason:abi_mismatch}`) if
  `manifest.abi_tag != device.abi_tag`. The marker-commit rollback (§7) / native rollback (§8) is
  the backstop, not the primary guard — a mismatched image must be refused, not installed-then-
  reverted.
- **The CI `satellite-build` job is a compile gate, NOT a release-artifact producer.** It builds the
  headless config on Ubuntu only to catch build breakage; deployable Pi binaries come exclusively
  from the Pi-OS container build above.
- **ESP32 (Tier 2)** is firmware on bare metal — no OS/glibc dependency — so `abi_tag` for it is
  just `esp32s3` (the existing `platform`); the cross-distro problem is RPi-specific.

---

## Phasing
1. **Foundation (field-migration, max lead time):** `firmware_version` + OTA-capability advert in
   registration; `ota_device_state` table + admin version visibility; **ESP32 partitions.csv
   bootstrap** documented. Zero apply risk. *(SHIPPED 2026-06-06.)*
   — **RPi install-path migration (R1) moved to Phase 3** (2026-06-06 decision): it restructures a
   working satellite's service for no Phase-1 payoff and is only testable alongside the apply code.
2. **Server:** offline keygen/sign tooling, release store, `ota_manifest.c` (+ host unit test,
   incl. `abi_tag`), **Pi-OS arm64 container build** for Tier 1 release artifacts (§10), HTTPS
   download route (token + path hardening), push spine (§4) + state machine (§5), admin
   publish/list/push/status/abort over both transports.
3. **Tier 1 (RPi) apply:** install-path migration (R1, §7); **device-side `abi_tag` check** (§10);
   libcurl download, libsodium verify-before-commit (0700, no TOCTOU), swap+self-restart,
   boot-count rollback.
4. **Tier 2 (ESP32) apply:** reboot-to-clean OTA, Update flow (WDT + sector-aligned), verify, native
   rollback.
5. **Hardening:** canary-then-rollout for `push all`, anti-rollback enforcement audit, audit log,
   docs. (Resume intentionally dropped both tiers v1.)

---

## Verification
- **Unit (host):** manifest sign/verify roundtrip; verify-rejects-tampered-bytes; numeric semver
  compare + anti-rollback; binary-manifest parse only-after-verify.
- **Integration:** publish (offline-signed) → push to a test RPi → download→verify→swap→restart→
  reconnect reports new version → server commits; ESP32 same with reboot-to-clean + partition
  rollback (pull power mid-update → boots old slot, NVS/secret intact).
- **Negative (must reject):** bad signature; SHA-256 mismatch; duplicate-key/oversized manifest;
  expired / wrong-uuid / replayed token; downgrade below signed `min_version`; path traversal;
  plaintext (non-TLS) OTA attempt.
- **Resilience:** RPi crash-on-boot new binary → auto-rollback before `StartLimitBurst`; daemon
  restart mid-update → reconciled on reconnect; `push all` → canary first.
