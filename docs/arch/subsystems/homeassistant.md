# Home Assistant Subsystem

Source: `src/tools/homeassistant_service.c`, `src/tools/homeassistant_tool.c`, `src/webui/webui_homeassistant.c`

Part of the [D.A.W.N. architecture](../../../ARCHITECTURE.md) — see the main doc for layer rules, threading model, and lock ordering.

---

**Purpose**: Smart home control via Home Assistant REST API — lights, climate, locks, covers, media players, scenes, scripts, automations.

## Architecture: Service + Tool + WebUI Admin

```
┌───────────────────────────────────────────────────────────────────────┐
│                     LLM TOOL INTERFACE                                │
│  homeassistant_tool.c                                                │
│  16 actions: get_state, turn_on, turn_off, toggle, set_brightness,   │
│  set_color, set_color_temp, set_climate, lock, unlock, open_cover,   │
│  close_cover, media_play, activate_scene, trigger_script,            │
│  trigger_automation                                                  │
├───────────────────────────────────────────────────────────────────────┤
│                     SERVICE LAYER                                     │
│  homeassistant_service.c                                             │
│  → REST API via libcurl with Long-Lived Access Token                 │
│  → Entity cache with periodic refresh                                │
│  → Fuzzy name matching (Levenshtein + token overlap)                 │
│  → Generic call_service() dispatcher                                 │
├───────────────────────────────────────────────────────────────────────┤
│                     WEBUI ADMIN                                       │
│  webui_homeassistant.c + homeassistant.js                            │
│  → Entity browser, connection status, URL/token config               │
│  → Compile-time feature guard (DAWN_ENABLE_HOMEASSISTANT_TOOL)       │
│  → server_features WS message + CSS feature-flag visibility          │
└───────────────────────────────────────────────────────────────────────┘
```

## Key Design Points

- **Fuzzy matching**: "Turn on the living room light" works even if the HA entity name differs slightly.
- **Area-aware**: satellite user mapping injects `HomeAssistant_Area=[X]` into LLM system prompt.
- **Feature guard**: `DAWN_ENABLE_HOMEASSISTANT_TOOL` CMake option. (DAWN's standalone SmartThings tool was removed in May 2026 — its upstream OAuth flow was permanently broken by an AWS WAF rule. Install Home Assistant via `docs/HOMEASSISTANT_SETUP.md` and use HA's own SmartThings integration if you need SmartThings device coverage.)
- **Entity cache**: avoids per-request API calls; refreshed on configurable interval.
