# Home Assistant Core: Bare Metal Setup on Jetson Orin

**Status:** Deployed and verified on Jetson AGX Orin hardware
**Date:** March 2026
**Platform:** Jetson Orin NX/AGX, JetPack 6.x (L4T R36.x), Ubuntu 22.04 Jammy aarch64
**HA Version:** 2026.2.3+
**DAWN Integration:** `src/tools/homeassistant_service.c`, `src/tools/homeassistant_tool.c`

---

## 1. Context

DAWN integrates with Home Assistant via REST API and WebSocket for smart home control (lights, climate, locks, covers, media players, scenes, scripts, automations). This document covers installing HA Core in a Python venv directly on the Jetson — no Docker, no Supervisor, no Wyoming, no HAOS.

> **If you previously used DAWN's SmartThings integration**: SmartThings was removed in May 2026 (its native OAuth integration with DAWN was permanently broken upstream by an AWS WAF rule). **The replacement is Home Assistant, which has a SmartThings integration of its own** — install HA via this guide, add the SmartThings integration in HA, and DAWN's `homeassistant` tool covers the same use cases (lights, switches, sensors, scenes, automations) through HA as the broker. The HA integration is more capable (covers Zigbee/Z-Wave/Matter/Bluetooth/Wi-Fi devices, not just cloud-routed SmartThings) and runs entirely locally with no cloud dependency on DAWN's side.

### Why Bare Metal

| Constraint | Docker | Bare Metal |
|---|---|---|
| CUDA passthrough | Requires `nvidia-container-runtime`, JetPack compat issues | Native — DAWN and HA share the host |
| Memory overhead | ~200MB container runtime | None |
| Startup time | Container pull + init | Direct `hass` launch |
| Complexity | Dockerfile, compose, volume mounts, networking | Python venv, systemd unit |

DAWN already runs as a native daemon on the Jetson. Adding a container runtime solely for HA introduces complexity with no benefit.

### Deprecation Status

HA Core bare-metal installation was deprecated in May 2025 (HA 2025.6). Official end-user support ended at HA 2025.12. In practice this means:

- The HA team removed the method from their docs and bug tracker
- The `homeassistant` pip package continues to receive monthly updates on PyPI
- The software continues to work — deprecation is a support-tier decision, not a technical one

**Maintenance impact:** Roughly once per year HA bumps its minimum Python version. Rebuild the venv against the new interpreter (see Section 12).

---

## 2. Platform Requirements

| Item | Value |
|---|---|
| Board | NVIDIA Jetson Orin (NX / AGX) |
| JetPack | 6.x (L4T R36.x) |
| OS | Ubuntu 22.04 LTS (Jammy) — aarch64 |
| System Python | 3.10 (too old for HA) |
| Required Python | 3.13 (HA Core 2025.2+) |
| HA interface | REST API + WebSocket |

---

## 3. System Prerequisites

```bash
sudo apt-get update
sudo apt-get install -y \
   software-properties-common \
   libffi-dev \
   libssl-dev \
   libjpeg-dev \
   zlib1g-dev \
   libopenjp2-7 \
   libtiff5 \
   libturbojpeg \
   ffmpeg \
   autoconf \
   build-essential \
   tzdata
```

> `libtiff5` is correct for Jammy. `libtiff6` is Noble (24.04) only.

---

## 4. Python 3.13 via deadsnakes PPA

Ubuntu 22.04 ships Python 3.10. Install 3.13 from the deadsnakes PPA (publishes aarch64 packages for Jammy):

```bash
sudo add-apt-repository ppa:deadsnakes/ppa
sudo apt-get update
sudo apt-get install -y python3.13 python3.13-dev python3.13-venv
python3.13 --version
```

> Never replace the system `python3` symlink. Ubuntu 22.04 uses `python3.10` internally for apt hooks. Always invoke explicitly as `python3.13`.

> Do not install `python3.13-distutils` — it conflicts on Jammy.

---

## 5. Dedicated System User

```bash
sudo useradd -r -m -s /bin/bash -G dialout,tty homeassistant
```

`dialout` and `tty` are for potential USB device access (Z-Wave, Zigbee). Drop them if serial devices are not used.

---

## 6. Virtual Environment and Install

```bash
sudo mkdir -p /srv/homeassistant
sudo chown homeassistant:homeassistant /srv/homeassistant

sudo -u homeassistant python3.13 -m venv /srv/homeassistant
sudo -u homeassistant /srv/homeassistant/bin/pip install --upgrade pip wheel
sudo -u homeassistant /srv/homeassistant/bin/pip install homeassistant

# Verify
sudo -u homeassistant /srv/homeassistant/bin/hass --version
```

---

## 7. Configuration

Create the config directory and `configuration.yaml` **before** first boot. If HA starts without one, it auto-generates `default_config:` which loads `recorder`, `history`, `logbook`, `energy`, and `media_source`. On Jammy, `recorder` fails immediately — system SQLite is 3.37.2 but HA requires 3.40.1+.

```bash
sudo -u homeassistant mkdir -p /home/homeassistant/.homeassistant
```

### configuration.yaml

```yaml
# DAWN — minimal HA Core config
# Avoids default_config: which loads recorder (requires SQLite 3.40.1+,
# Jammy ships 3.37.2). DAWN uses REST/WebSocket for current state only.

homeassistant:
   name: DAWN
   latitude: !secret latitude
   longitude: !secret longitude
   elevation: 0
   unit_system: metric
   time_zone: America/New_York

http:
   server_host: 0.0.0.0
   server_port: 8123

api:
frontend:
person:
sun:

# Required for cloud-based integrations (Nabu Casa, SmartThings, etc.).
# Add SmartThings via HA's Settings → Devices & Services → Add Integration
# to bring SmartThings devices into DAWN through Home Assistant.
cloud:

logger:
   default: warning
   logs:
      homeassistant.core: info
      homeassistant.bootstrap: info
      homeassistant.setup: info
```

### secrets.yaml

```yaml
latitude: "33.9"
longitude: "-84.0"
```

---

## 8. First Boot

Run interactively to let HA download and cache its dependency set (5-10 minutes on first run). HA uses `uv` internally — pass `UV_NO_CONFIG=1` to prevent it from leaking the invoking user's `~/uv.toml`:

```bash
sudo -u homeassistant -H \
   env UV_NO_CONFIG=1 HOME=/home/homeassistant \
   /srv/homeassistant/bin/hass \
   -c /home/homeassistant/.homeassistant
```

Startup is confirmed when the journal shows:

```
(MainThread) [homeassistant.bootstrap] Home Assistant initialized in Xs (load=Ys)
```

> First run will sit silently for several minutes while `uv` downloads packages. Verify the process is alive with `ps -p <pid> -o pid,stat,cputime,cmd` — advancing cputime means it is working.

---

## 9. Systemd Service

```bash
sudo tee /etc/systemd/system/homeassistant.service > /dev/null <<'EOF'
[Unit]
Description=Home Assistant Core (DAWN)
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
User=homeassistant
WorkingDirectory=/home/homeassistant
Environment=HOME=/home/homeassistant
Environment=UV_NO_CONFIG=1
ExecStart=/srv/homeassistant/bin/hass -c /home/homeassistant/.homeassistant
Restart=on-failure
RestartSec=5s
TimeoutStopSec=30

[Install]
WantedBy=multi-user.target
EOF

sudo systemctl daemon-reload
sudo systemctl enable homeassistant
sudo systemctl start homeassistant
```

Verify:

```bash
sudo systemctl status homeassistant
sudo journalctl -u homeassistant -f
```

Web UI available at `http://<orin-ip>:8123` once initialized.

---

## 10. Onboarding and Token Generation

### Onboarding Wizard

Navigate to `http://<orin-ip>:8123` to complete the one-time setup:

1. Create the owner account (username + password)
2. Confirm home location (should match `secrets.yaml`)
3. Skip device discovery (DAWN integrations are configured separately)

The owner account is permanent and cannot be deleted.

### Long-Lived Access Token

1. Log in at `http://<orin-ip>:8123`
2. Click username (bottom-left) → **Long-Lived Access Tokens** → **Create Token**
3. Name it `DAWN`, copy immediately (shown only once)

Store in DAWN's `secrets.toml`:

```toml
[secrets.homeassistant]
token = "<your-token>"
```

---

## 11. API Verification

### REST API

```bash
curl -s -H "Authorization: Bearer <token>" http://<orin-ip>:8123/api/
# Expected: {"message": "API running."}

curl -s -H "Authorization: Bearer <token>" http://<orin-ip>:8123/api/states | python3 -m json.tool
```

DAWN handles WebSocket communication with HA automatically — no manual verification needed. See the [HA WebSocket API docs](https://developers.home-assistant.io/docs/api/websocket/) if you want to explore further.

---

## 12. Maintenance

### Update HA Core

```bash
sudo systemctl stop homeassistant
sudo -u homeassistant /srv/homeassistant/bin/pip install --upgrade homeassistant
sudo systemctl start homeassistant
```

### Python Version Bump

When HA requires a newer Python than the current venv (roughly annual):

```bash
# Stop service
sudo systemctl stop homeassistant

# Install new interpreter (example: 3.14)
sudo apt-get update
sudo apt-get install -y python3.14 python3.14-dev python3.14-venv

# Back up and rebuild venv
sudo mv /srv/homeassistant /srv/homeassistant.bak
sudo mkdir -p /srv/homeassistant
sudo chown homeassistant:homeassistant /srv/homeassistant
sudo -u homeassistant python3.14 -m venv /srv/homeassistant
sudo -u homeassistant /srv/homeassistant/bin/pip install --upgrade pip wheel
sudo -u homeassistant /srv/homeassistant/bin/pip install homeassistant

# Config directory is untouched — restart
sudo systemctl start homeassistant
sudo systemctl status homeassistant

# Remove backup after confirming
sudo rm -rf /srv/homeassistant.bak
```

---

## 13. Troubleshooting

| Symptom | Cause | Fix |
|---|---|---|
| `failed to open file ~/uv.toml: Permission denied` | `uv` leaking invoking user's config | `UV_NO_CONFIG=1 HOME=/home/homeassistant` in run command or systemd `Environment=` |
| `SQLite 3.37.2 is not supported` | Jammy SQLite too old for recorder | Use explicit config without `default_config:` (Section 7) |
| `ffmpeg: No such file or directory` | Missing dependency | `apt-get install ffmpeg` |
| `Unable to locate turbojpeg library` | Missing dependency | `apt-get install libturbojpeg` |
| `pip install homeassistant` fails on C extension | Missing build deps | `apt-get install build-essential libffi-dev libssl-dev` |
| Web UI unreachable after start | HA still initializing | Wait 60s, check `journalctl -u homeassistant` |
| `libtiff.so.5: not found` | Missing dependency | `apt-get install libtiff5` |
| Auth token rejected | Token copied incorrectly | Regenerate in UI, check for trailing whitespace |
| WebSocket drops | Network instability or HA restart | DAWN service module handles reconnection automatically |
| Python version mismatch on pip | Wrong pip invoked | Always use full path: `/srv/homeassistant/bin/pip` |

---

## 14. Quick Reference

| Item | Value |
|---|---|
| HA venv | `/srv/homeassistant/` |
| Config directory | `/home/homeassistant/.homeassistant/` |
| Systemd service | `homeassistant.service` |
| Web UI | `http://<orin-ip>:8123` |
| REST API base | `http://<orin-ip>:8123/api/` |
| WebSocket | `ws://<orin-ip>:8123/api/websocket` |
| Logs | `journalctl -u homeassistant -f` |
| System user | `homeassistant` |
| Python | `/usr/bin/python3.13` |
| pip | `/srv/homeassistant/bin/pip` |
| DAWN config | `[homeassistant]` section in `dawn.toml` + `secrets.toml` |

---

## References

- [homeassistant on PyPI](https://pypi.org/project/homeassistant/)
- [HA REST API docs](https://developers.home-assistant.io/docs/api/rest/)
- [HA WebSocket API docs](https://developers.home-assistant.io/docs/api/websocket/)
- [deadsnakes PPA](https://launchpad.net/~deadsnakes/+archive/ubuntu/ppa)
- [HA deprecation announcement (2025.6)](https://www.home-assistant.io/blog/2025/05/22/deprecating-core-and-supervised-installation-methods-and-32-bit-systems/)
