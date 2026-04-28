# D.A.W.N. System Architecture

**D.A.W.N.** (Digital Assistant for Workflow Neural-inference) is the central intelligence layer of the OASIS ecosystem. It interprets user intent, fuses data from every subsystem, and routes commands. DAWN acts as OASIS's orchestration hub for MIRAGE, AURA, SPARK, STAT, and any future modules.

This document is the **architectural map**: directory layout, the cross-cutting rules every subsystem obeys (layering, threading, lock ordering, error handling, configuration), and a one-paragraph summary of each subsystem with a link to its detail doc. For the internals of any single subsystem — its components, data flow, DB schema, and tuning — open the linked file in [`docs/arch/subsystems/`](docs/arch/subsystems/).

**Last updated**: April 2026.

## Table of Contents

- [Directory Structure](#directory-structure)
- [High-Level Overview](#high-level-overview)
- [Subsystem Index](#subsystem-index)
- [Module Dependency Hierarchy](#module-dependency-hierarchy)
- [Threading Model](#threading-model)
- [State Machine](#state-machine)
- [Mutex Lock Ordering Hierarchy](#mutex-lock-ordering-hierarchy)
- [Memory Management](#memory-management)
- [Error Handling](#error-handling)
- [File Organization Standards](#file-organization-standards)
- [Configuration Architecture](#configuration-architecture)
- [Performance Considerations](#performance-considerations)
- [DAP2 Satellite Protocol](#dap2-satellite-protocol)
- [Command Processing](#command-processing)
- [References](#references)

---

## Directory Structure

```
dawn/
├── src/                    # C/C++ source files
│   ├── asr/                # Speech recognition (Whisper, Vosk, VAD)
│   ├── llm/                # LLM integration (OpenAI, Claude, Gemini, local)
│   ├── memory/             # Persistent memory system
│   ├── tts/                # Text-to-speech (Piper)
│   ├── audio/              # Audio capture, playback, music
│   ├── core/               # Session manager, scheduler, embedding engine, crypto
│   ├── auth/               # User auth, per-user settings, satellite mappings
│   ├── tools/              # Modular LLM tools (search, weather, calendar, email, scheduler, etc.)
│   └── webui/              # Web UI server
│
├── include/                # Header files (mirrors src/)
├── common/                 # Shared library (VAD, ASR, TTS, logging, sentence buffer) for daemon + satellite
├── www/                    # Web UI static files (HTML, CSS, JS)
├── models/                 # ML models (TTS voices, VAD)
├── sound_assets/           # Notification chimes, ringtones, SFX
├── tool_instructions/      # Two-step instruction loader content (render_visual guidelines)
├── whisper.cpp/            # Whisper ASR engine (git submodule)
├── dawn_satellite/         # DAP2 Tier 1 satellite (Raspberry Pi, SDL2 UI)
├── dawn_satellite_arduino/ # DAP2 Tier 2 satellite (ESP32-S3, Arduino sketch)
├── dawn-admin/             # Admin CLI (socket client to daemon)
├── services/               # Systemd service files
├── scripts/                # Utility scripts (setup, tooling)
├── tests/                  # Unit and integration tests
├── benchmarks/             # Retrieval benchmark harness (LongMemEval, LoCoMo, ConvoMem)
├── llm_testing/            # LLM quality/latency benchmarking
├── docs/                   # Additional documentation
│   └── arch/               # Architecture detail docs (per-subsystem)
│
├── dawn.toml.example       # Configuration template
├── secrets.toml.example    # API keys template
└── CMakeLists.txt          # Build configuration
```

---

## High-Level Overview

DAWN is a modular voice assistant. A voice command flows through a pipeline of specialized subsystems:

```
┌─────────────────────────────────────────────────────────────┐
│                      DAWN Main Loop                         │
│  (src/dawn.c — State Machine: SILENCE → WAKEWORD → COMMAND  │
│   → PROCESSING)                                             │
└────────┬──────────────────────────────┬─────────────────────┘
         │ Local Audio                  │ WebSocket (WebUI + Satellites)
    ┌────▼─────────┐            ┌───────▼──────────┐
    │ Audio Capture│            │  WebUI Server    │
    │ Thread + RB  │            │ (libwebsockets)  │
    └────┬─────────┘            └───────┬──────────┘
         │                              │
    ┌────▼──────────┐           ┌───────▼──────────┐
    │  VAD (Silero) │           │ Session Manager  │
    └────┬──────────┘           │ + Audio Workers  │
         │                      └───────┬──────────┘
    ┌────▼──────────┐                   │
    │ ASR Interface │                   │
    │ (Vosk|Whisper)│                   │
    └────┬──────────┘                   │
         └───────────┬──────────────────┘
                     ▼
            ┌────────────────┐
            │ LLM Interface  │───► OpenAI / Claude / Gemini / llama.cpp
            └────────┬───────┘     (streaming)
                     │
            ┌────────▼────────┐
            │ SSE Parser +    │
            │ Sentence Buffer │
            └────────┬────────┘
                     │
            ┌────────▼────────┐
            │  TTS (Piper)    │───► ALSA / PulseAudio
            └─────────────────┘
```

### Core Design Principles

1. **Modularity**: each subsystem has a clear interface and can be replaced independently.
2. **Performance**: GPU acceleration on Jetson; optimized local LLM inference.
3. **Reliability**: retry logic, checksums, error recovery in network protocol.
4. **Flexibility**: multiple ASR engines, LLM providers, and audio backends.
5. **Embedded-first**: designed for resource-constrained platforms (static allocation preferred).

---

## Subsystem Index

Each row points to a detail doc in [`docs/arch/subsystems/`](docs/arch/subsystems/) that covers components, data flow, schemas, and tuning.

| Subsystem | Role | Detail doc |
|---|---|---|
| **Core** (`src/` root + `src/core/`) | Main entry, MQTT integration, legacy command parsing. `src/dawn.c` hosts the state machine; `src/mosquitto_comms.c/h` wires MQTT; `src/text_to_command_nuevo.c/h` extracts `<command>` tags from LLM output; `src/word_to_number.c/h` converts "twenty-three" → 23; `src/core/` contains the session manager, scheduler, command executor/router, worker pool, and wake-word detector. Logging macros (`LOG_INFO/WARNING/ERROR`) come from `common/include/logging.h`, shared with the satellite. | *(inlined above)* |
| **ASR** | Speech recognition abstraction (Strategy pattern) over Whisper and Vosk, plus Silero VAD and chunking for long utterances. Whisper on Jetson GPU is the default; Vosk is retained for CPU-only builds. | [asr.md](docs/arch/subsystems/asr.md) |
| **LLM** | Unified interface for OpenAI, Claude, Gemini, and local (llama.cpp/Ollama). Streaming via SSE feeds a sentence buffer that hands complete sentences to TTS while the response is still generating. Runs on a dedicated worker thread so the main audio loop never blocks; wake-word interrupts abort in-flight API calls. | [llm.md](docs/arch/subsystems/llm.md) |
| **TTS** | Piper + ONNX Runtime with preprocessing for natural phrasing. Mutex-protected so the main loop, network server, and streaming buffer can all synthesize safely. | [tts.md](docs/arch/subsystems/tts.md) |
| **DAP2 Satellite** | WebSocket protocol for all remote clients: WebUI browser (Opus), Tier 1 Raspberry Pi (local ASR/TTS, text-only), and Tier 2 ESP32 (raw PCM). A single server on port 3000 serves all three — adding a new client type means a new registration handler, not a new server. | [satellite.md](docs/arch/subsystems/satellite.md) |
| **Audio** | Capture thread + thread-safe ring buffer, multi-format playback (FLAC/MP3/Ogg), and the unified music DB (local files + Plex) with source-aware dedup and background scanner. | [audio.md](docs/arch/subsystems/audio.md) |
| **WebUI Audio** | Browser-side Opus streaming via WebCodecs + server-side decode/resample/ASR/TTS/encode pipeline. Also hosts **always-on voice mode** (server-side VAD + wake word, no browser AI) and the **visual rendering tool** (inline SVG/HTML/Chart.js diagrams). | [webui-audio.md](docs/arch/subsystems/webui-audio.md) |
| **Vision & Documents** | Image upload (client compression, server filesystem storage with source/retention policies, zero-copy HTTP serving) and document upload (PDF via MuPDF, DOCX via libzip+libxml2, plain text client-side). | [vision-documents.md](docs/arch/subsystems/vision-documents.md) |
| **Memory** | Persistent user profile built by a **sleep-consolidation model**: extraction runs at session end, not during conversation, so chat latency is unchanged. Facts, preferences, summaries, entity graph, and contacts; hybrid keyword + semantic search via embeddings; nightly confidence decay. | [memory.md](docs/arch/subsystems/memory.md) |
| **Document Search / RAG** | Upload → chunk → embed → search or paginated read via LLM tools. Shares `embedding_engine.c` with the memory subsystem; supports ONNX (local), Ollama, and OpenAI-compatible embedding providers. | [rag.md](docs/arch/subsystems/rag.md) |
| **CalDAV Calendar** | Multi-account RFC 4791 client with offline-first SQLite cache, pre-expanded RRULE occurrences, and background sync. Tested with Google, iCloud, Nextcloud, Radicale. | [calendar.md](docs/arch/subsystems/calendar.md) |
| **Email** | Dual backend — IMAP/SMTP for anything, Gmail REST API for OAuth accounts. Two-step confirmation on send and trash. Recipients resolved against the contacts system. | [email.md](docs/arch/subsystems/email.md) |
| **OAuth 2.0 & Crypto** | Shared OAuth client with PKCE S256 and `crypto_store.c` (libsodium `crypto_secretbox`) for encrypted token and password storage. Used by email and calendar. | [oauth-crypto.md](docs/arch/subsystems/oauth-crypto.md) |
| **Scheduler** | Timers, alarms, reminders, and scheduled tool execution. Background thread polls every second, fires with chime audio + WebUI banner notifications, supports recurrence and snooze/dismiss. | [scheduler.md](docs/arch/subsystems/scheduler.md) |
| **Home Assistant** | REST API client with entity cache, fuzzy name matching, and satellite area-awareness (`HomeAssistant_Area=[X]` injected into the LLM system prompt). 16 tool actions spanning lights, climate, locks, covers, media, scenes, scripts, automations. | [homeassistant.md](docs/arch/subsystems/homeassistant.md) |
| **Per-User Settings** | Persona, location, timezone, units, theme — stored in `user_settings` and injected into the LLM system prompt at session start so every session is personalized to the authenticated user. | [user-settings.md](docs/arch/subsystems/user-settings.md) |

---

## Module Dependency Hierarchy

To prevent circular dependencies and maintain clean architecture, modules are organized into layers. **Modules may only depend on modules in lower layers.**

```
Layer 0 (Foundation)
├── common/src/logging.c           - Logging macros (shared with satellite, no deps)
├── include/dawn_error.h           - SUCCESS/FAILURE return codes (no deps)
├── src/config/                    - Configuration parsing and defaults
│   ├── config_parser.c
│   ├── config_defaults.c
│   ├── config_env.c
│   └── config_validate.c
└── include/config/dawn_config.h   - Config struct definitions

Layer 1 (Core Infrastructure)
├── src/tools/tool_registry.c/h    - Tool registration and lookup (deps: logging, config)
├── src/core/command_router.c/h    - Request/response routing (deps: logging)
├── src/core/command_executor.c/h  - Unified command executor (deps: tool_registry)
├── src/core/session_manager.c/h   - Session lifecycle (deps: logging, config)
├── src/core/worker_pool.c/h       - Concurrent tool execution (deps: logging)
├── src/core/wake_word.c/h         - Wake-word matching (shared daemon + satellites)
├── src/core/time_query_parser.c/h - Stateless temporal-expression recognizer (deps: libc, math)
└── src/input_queue.c/h            - Thread-safe input queue (deps: logging)

Layer 2 (Services)
├── src/llm/                       - LLM providers and tools
│   ├── llm_interface.c            - Provider abstraction (deps: Layer 0-1)
│   ├── llm_openai.c               - OpenAI/Ollama/llama.cpp (deps: llm_interface)
│   ├── llm_claude.c               - Anthropic Claude (deps: llm_interface)
│   └── llm_tools.c                - Tool execution (deps: tool_registry)
├── src/core/embedding_engine.c    - Shared embedding infrastructure (deps: Layer 0-1)
├── src/core/crypto_store.c        - Shared libsodium encryption (deps: Layer 0)
├── src/core/scheduler.c           - Scheduler engine + background thread (deps: Layer 0-1)
├── src/tts/                       - Text-to-speech (deps: Layer 0-1)
├── src/asr/                       - Daemon-side ASR interface, Vosk, chunking (deps: Layer 0-1)
├── common/src/asr/                - Shared ASR engines (Whisper, VAD) used by daemon + satellite
├── src/mosquitto_comms.c          - MQTT integration (deps: Layer 0-1, tool_registry)
├── src/memory/                    - Persistent memory + contacts (deps: Layer 0-1, embedding_engine)
└── src/auth/                      - User auth, settings, per-user prefs (deps: Layer 0-1)

Layer 3 (Tools)
├── src/tools/weather_tool.c           - Weather API (deps: Layer 0-2)
├── src/tools/music_tool.c             - Music playback (deps: Layer 0-2)
├── src/tools/search_tool.c            - Web search (deps: Layer 0-2)
├── src/tools/memory_tool.c            - Memory commands (deps: Layer 0-2, memory/)
├── src/tools/document_search.c        - RAG semantic search (deps: Layer 0-2, embedding_engine)
├── src/tools/document_read.c          - Paginated doc reader (deps: Layer 0-2, document_db)
├── src/tools/document_db.c            - Document SQLite CRUD (deps: Layer 0-1, auth_db)
├── src/tools/email_service.c          - Email routing + two-step confirm (deps: Layer 0-2, oauth_client)
├── src/tools/email_client.c           - IMAP/SMTP backend (deps: Layer 0-1, crypto_store)
├── src/tools/gmail_client.c           - Gmail REST API backend (deps: Layer 0-1, oauth_client)
├── src/tools/oauth_client.c           - OAuth 2.0 + PKCE (deps: Layer 0-1, crypto_store)
├── src/tools/homeassistant_service.c  - HA REST API + entity cache (deps: Layer 0-1)
├── src/tools/calendar_service.c       - CalDAV business logic (deps: Layer 0-2, oauth_client)
└── src/tools/*.c                      - All other tools (deps: Layer 0-2)

Layer 4 (Application)
├── src/dawn.c                     - Main entry + voice state machine (deps: all layers)
└── src/webui/                     - Web interface + WebSocket server (deps: Layer 0-3)
```

### Dependency Rules

1. **Downward only**: a module may only `#include` headers from its own layer or lower.
2. **No cycles**: if A depends on B, B must not depend on A (directly or transitively).
3. **Interface segregation**: use forward declarations and callbacks to break potential cycles.
4. **Same-layer allowed**: modules in the same layer may depend on each other if acyclic.

### Common Patterns to Avoid Cycles

**Callback registration** (Layer 2 → Layer 3 without direct dependency):

```c
// In tool_registry.h (Layer 1)
typedef char *(*tool_callback_t)(const char *action, char *value, int *should_respond);

// In weather_tool.c (Layer 3) — registers callback at init
tool_registry_register(&weather_metadata);  // Passes function pointer up
```

**Forward declarations** (when header inclusion would create a cycle):

```c
// In llm_tools.h — avoid including full tool_registry.h
struct tool_metadata;  // Forward declaration
```

---

## Threading Model

DAWN keeps the thread count small. The main thread owns the voice state machine, ASR, TTS invocation, and MQTT. Dedicated worker threads handle anything that would otherwise block the audio loop.

```
┌────────────────────────────────────────────────────────┐
│                      Main Thread                       │
│  - State machine (SILENCE → WAKEWORD → COMMAND → PROC) │
│  - VAD + ASR processing                                │
│  - TTS synthesis (mutex-protected)                     │
│  - MQTT, session management                            │
└────────┬───────────────────────────────────────────────┘
         │
         │ spawns per-task workers as needed
         ▼
┌────────────────────────────────────────────────────────┐
│  Capture thread  — continuous ALSA → ring buffer       │
│  LLM worker      — blocking HTTP + interrupt polling   │
│  Memory extract  — session-end, background             │
│  Music scanner   — periodic local + Plex sync          │
│  Scheduler       — 1-second polling loop               │
│  CalDAV sync     — background event pull               │
│  WebUI audio     — per-connection ASR/TTS pipeline     │
└────────────────────────────────────────────────────────┘
```

**Synchronization primitives**:

- **Ring buffer**: thread-safe circular buffer for audio data (lock-free read/write pointers).
- **TTS mutex** (`tts_mutex`): protects Piper from concurrent access.
- **LLM mutex** (`llm_mutex`): guards request/response ownership transfer between main and worker.
- **Auth DB mutex**: serializes SQLite writes against the shared `auth.db` handle.
- **Embedding cache mutexes**: protect in-memory fact and entity embedding caches (see [memory.md](docs/arch/subsystems/memory.md)).

See [Mutex Lock Ordering Hierarchy](#mutex-lock-ordering-hierarchy) below for the acquire-order invariants.

---

## State Machine

The main application (`src/dawn.c`) implements a state machine for local voice processing:

```
                    ┌─────────────┐
                    │   SILENCE   │ (Listening for wake word)
                    └──────┬──────┘
                           │ VAD detects speech
                           ↓
                    ┌─────────────────────┐
                    │  WAKEWORD_LISTEN    │ (Detecting wake word)
                    └──────┬──────────────┘
                           │ Wake word detected ("friday")
                           ↓
                    ┌─────────────────────┐
                    │ COMMAND_RECORDING   │ (Recording user command)
                    └──────┬──────────────┘
                           │ VAD detects silence (end of command)
                           ↓
                    ┌─────────────────────┐
                    │    PROCESSING       │ (ASR → LLM → TTS → MQTT)
                    └──────┬──────────────┘
                           │ Processing complete
                           ↓
                    ┌─────────────┐
                    │   SILENCE   │ (Return to listening)
                    └─────────────┘
```

| From State        | Event                 | To State          |
| ----------------- | --------------------- | ----------------- |
| SILENCE           | VAD detects speech    | WAKEWORD_LISTEN   |
| WAKEWORD_LISTEN   | Wake word detected    | COMMAND_RECORDING |
| WAKEWORD_LISTEN   | Timeout / false alarm | SILENCE           |
| COMMAND_RECORDING | VAD detects silence   | PROCESSING        |
| PROCESSING        | Pipeline complete     | SILENCE           |

During PROCESSING the LLM call runs on a worker thread. The main thread continues to service audio; a wake word during LLM inference triggers `llm_request_interrupt()`, which aborts the CURL transfer and rolls back conversation history.

---

## Mutex Lock Ordering Hierarchy

**CRITICAL**: to prevent deadlocks, the codebase follows a strict acquisition order when multiple locks are needed. Mutexes fall into three categories by scope:

```
Global daemon locks (src/dawn.c):
  llm_mutex              — LLM worker thread ↔ main thread buffer transfer
  tts_mutex              — TTS engine (Piper) serialization
  conversation_mutex     — conversation history list
  direct_mode_prompt_mutex — direct-mode prompt reload

Per-session locks (src/core/session_manager.c):
  session->history_mutex    — session conversation history
  session->metrics_mutex    — session-scoped metrics (tokens, timings)
  session->fd_mutex         — WebSocket file-descriptor state
  session->ref_mutex        — session reference counting
  session->llm_config_mutex — per-session LLM config overrides

Per-module locks (scoped to a single subsystem):
  auth_db mutex (src/auth/auth_db_core.c)         — SQLite serialization
  tool_registry::s_registry_mutex                 — tool lookup table
  embedding_engine::s_embed_mutex                 — embed provider serialization
  scheduler_mutex, ringing_mutex (scheduler.c)    — scheduler event queue
  worker_pool::pool_mutex                         — worker thread pool
  command_router::registry_mutex                  — request/response routing
  ...and similar per-tool mutexes in src/tools/*.c
```

### Lock Ordering Rules

1. **Global locks are acquired before per-session locks, which are acquired before per-module locks.** Never acquire a higher-scope lock while holding a lower-scope one.

2. **Never hold two global locks simultaneously.** Release one before acquiring another. The main thread holds at most one of `tts_mutex`, `llm_mutex`, `conversation_mutex` at a time.

3. **The `auth_db` mutex is a leaf lock** (no other locks held during SQLite writes). Copy data out, release, then continue.

4. **Keep critical sections minimal.** Copy data, release the lock, *then* process. Avoid I/O while holding locks.

5. **Prefer lock-free patterns for high-frequency updates.** The audio ring buffer uses volatile read/write pointers; state flags use `volatile` booleans or C11 atomics; `llm_processing` and `llm_interrupt_requested` are `volatile sig_atomic_t`.

### Testing Lock Discipline

Use **ThreadSanitizer** during development:

```bash
cd build
cmake -DCMAKE_C_FLAGS="-fsanitize=thread -g" ..
make
./dawn
```

ThreadSanitizer detects data races, lock order inversions, and use-after-free in threaded code.

---

## Memory Management

### Design Principles

1. **Prefer static allocation**: embedded systems benefit from predictable memory usage.
2. **Minimize dynamic allocation**: use `malloc`/`calloc` sparingly.
3. **Always check NULL**: verify dynamic allocation succeeded.
4. **Free and NULL**: set pointers to NULL after freeing.

### Memory Patterns

**Static buffers** (preferred):

```c
#define AUDIO_BUFFER_SIZE 16000
static int16_t audio_buffer[AUDIO_BUFFER_SIZE];
```

**Dynamic allocation** (when necessary):

```c
char *response = malloc(response_len);
if (response == NULL) {
   LOG_ERROR("Failed to allocate response buffer");
   return FAILURE;
}
// ... use response ...
free(response);
response = NULL;
```

### Memory Usage Estimates

| Component    | Memory Usage | Notes                      |
| ------------ | ------------ | -------------------------- |
| Whisper base | ~140 MB      | Model weights + context    |
| Vosk 0.22    | ~50 MB       | Smaller footprint          |
| Silero VAD   | ~2 MB        | Tiny ONNX model            |
| Piper TTS    | ~30 MB       | Voice model + ONNX runtime |
| Ring Buffer  | ~256 KB      | 16kHz × 16-bit × 8s buffer |
| Conversation | ~10 KB       | History for LLM context    |

**Total (Whisper)**: ~230 MB RAM minimum. **Total (Vosk)**: ~140 MB RAM minimum.

---

## Error Handling

### Error Code Convention

Central definitions in `include/dawn_error.h`:

```c
#define SUCCESS  0
#define FAILURE  1
```

Modules define specific error codes > 1 in their own headers (e.g., `AUTH_DB_FAILURE`, `MEMORY_DB_NOT_FOUND`, `SCHED_DB_USER_LIMIT`). Functions that return counts or IDs use an output parameter (`int *count_out`, `int64_t *id_out`) and return `SUCCESS`/`FAILURE`.

**IMPORTANT**: do NOT use negative return values (`-1`, `-errno`). Use positive error codes only. The sole exception is `LWS_CLOSE_CONNECTION` (-1) in lws callback functions, per the libwebsockets API contract.

### Patterns

**Function return codes**:

```c
int asr_process_audio(ASRContext *ctx, int16_t *audio, size_t samples) {
   if (ctx == NULL || audio == NULL) {
      LOG_ERROR("Invalid parameters");
      return FAILURE;
   }
   // ... processing ...
   return SUCCESS;
}
```

**Retry with exponential backoff** (network I/O):

```c
int retry_count = 0;
while (retry_count < MAX_RETRIES) {
   if (send_packet(packet) == SUCCESS) break;
   LOG_WARNING("Send failed, retry %d/%d", retry_count + 1, MAX_RETRIES);
   sleep(1 << retry_count);  // 1s, 2s, 4s
   retry_count++;
}
```

**Graceful degradation** (feature availability):

```c
if (gpu_available) {
   ctx = asr_whisper_init(model_path);
} else {
   LOG_WARNING("GPU not available, using CPU-only ASR");
   ctx = asr_vosk_init(model_path);
}
```

---

## File Organization Standards

### Size Limits

| File Type  | Soft Limit  | Hard Limit  |
| ---------- | ----------- | ----------- |
| C source   | 1,500 lines | 2,500 lines |
| JavaScript | 1,000 lines | 1,500 lines |
| CSS        | 1,000 lines | 2,000 lines |

### Module Split Pattern (C)

When a C file exceeds limits, split by feature using an internal header:

```
src/subsystem/
├── subsystem_core.c       # Init, shutdown, shared state
├── subsystem_feature1.c   # Feature area 1
├── subsystem_feature2.c   # Feature area 2
└── ...

include/subsystem/
├── subsystem.h            # Public API (unchanged)
└── subsystem_internal.h   # Shared state, internal helpers
```

The internal header contains `extern` declarations for shared state (defined in `_core.c`), internal helper function declarations, and shared macros (e.g., locking patterns).

### When Adding New Features

1. **Check file size first** — if the target file > 1,500 lines, consider creating a new file.
2. **Group by feature** — related functionality goes together in one module.
3. **Use internal headers** — share state via the `*_internal.h` pattern.
4. **Update build system** — add new source files immediately.

---

## Configuration Architecture

### Design Principles

1. **Config files as source of truth**: all DAWN application settings live in `dawn.toml` (runtime) or `secrets.toml` (credentials). The SQLite database is reserved for user-generated content — authentication, sessions, conversations, uploaded images. **Settings are never stored in the database.** This keeps configuration portable, version-controllable (minus secrets), and inspectable.

2. **WebUI settings exposure**: every setting in `dawn.toml` is surfaced in the WebUI settings panel unless explicitly excluded. Exclusions are limited to file system paths (security), internal debug flags, and restart-only settings that have no runtime effect.

3. **Secrets isolation**: credentials in `secrets.toml` stay separate from general config so `dawn.toml` can be shared safely, per-deployment secrets can differ, and credentials rotate without touching the main config.

4. **Compile-time vs runtime**: `dawn.h` provides compile-time defaults only. All user-configurable settings belong in TOML; `dawn.h` values serve as fallbacks when config is missing.

### Configuration File Hierarchy

```
~/.config/dawn/     # User-specific (highest priority)
./                  # Project root (fallback)
/etc/dawn/          # System-wide (lowest priority, future)
```

Higher-priority files override lower.

### Configuration Files

**`dawn.toml`** — runtime configuration, one section per subsystem:

```toml
[general]
ai_name = "friday"
timezone = "America/New_York"

[llm]
type = "cloud"                    # "cloud" or "local"

[llm.cloud]
provider = "openai"               # "openai", "anthropic", "gemini"
model = "gpt-4o"

[llm.local]
endpoint = "http://localhost:8080"
model = "qwen3-4b"

[asr]
model_path = "models/whisper.cpp/ggml-base.en.bin"
language = "en"

[tts]
model_path = "models/en_GB-alba-medium.onnx"
sample_rate = 22050

[webui]
bind_address = "0.0.0.0"
port = 3000
```

**`secrets.toml`** — API keys and sensitive credentials (gitignored):

```toml
openai_api_key = "sk-..."
claude_api_key = "sk-ant-..."
gemini_api_key = "..."
```

**`dawn.h`** — compile-time fallbacks: `AI_NAME`, `AI_DESCRIPTION`, `DEFAULT_PCM_PLAYBACK_DEVICE`, `DEFAULT_PCM_CAPTURE_DEVICE`, `MQTT_IP`, `MQTT_PORT`.

**Tool registry** — tools are defined as compile-time `tool_metadata_t` structs in `src/tools/*.c` and registered in `src/tools/tools_init.c` via `tools_register_all()`. See [command-processing.md](docs/arch/command-processing.md).

### WebUI Settings Panel Mapping

The WebUI settings panel (`www/js/ui/settings.js`) defines a `SETTINGS_SCHEMA` that maps to `dawn.toml` sections:

| WebUI Section      | Config Section                        | Notes                                           |
| ------------------ | ------------------------------------- | ----------------------------------------------- |
| Language Model     | `[llm]`, `[llm.cloud]`, `[llm.local]` | Provider, model selection                       |
| Speech Recognition | `[asr]`                               | Model, language                                 |
| Text-to-Speech     | `[tts]`                               | Voice model, rate                               |
| Audio              | `[audio]`                             | Backend, devices                                |
| Tool Calling       | `[llm.tools]`                         | Mode, per-tool toggles                          |
| Network            | `[webui]`, `[dap]`, `[mqtt]`          | Ports, addresses                                |
| Images & Vision    | `[images]`, `[vision]`                | Storage retention, upload size/dimension limits |
| Documents          | `[documents]`                         | Upload size, page limits, index limits, chunking |

When adding new settings to `dawn.toml`, also add corresponding entries to `SETTINGS_SCHEMA` to expose them in the WebUI, unless they fall under the exclusion criteria above.

---

## Performance Considerations

### GPU Acceleration (Jetson)

- Automatic detection via `/etc/nv_tegra_release` in CMake.
- CUDA libraries (cuSPARSE, cuBLAS, cuSOLVER, cuRAND) linked automatically.
- Whisper GPU enabled with `GGML_CUDA=ON`; 2.3x–5.5x speedup over CPU.

### Perceived Latency

**Total** = ASR time + TTFT + TTS time.

| Component          | Latency (Whisper base GPU) | Notes                     |
| ------------------ | -------------------------- | ------------------------- |
| ASR (Whisper base) | ~110 ms                    | GPU accelerated           |
| TTFT (Qwen3-4B)    | ~138 ms                    | Local LLM first token     |
| TTS (Piper)        | ~200 ms                    | First sentence            |
| **Total**          | **~448 ms**                | User hears first response |

**Streaming advantage**: with streaming LLM + TTS, the user hears a response in <500ms instead of waiting for the complete LLM response (~3s).

### Platform Override

CMake auto-detects Jetson, Raspberry Pi, and generic ARM64. Force with:

```bash
cmake -DPLATFORM=JETSON ..  # Force Jetson (enables CUDA)
cmake -DPLATFORM=RPI ..     # Force RPi (disables CUDA)
```

---

## DAP2 Satellite Protocol

DAP2 is the unified WebSocket protocol for all remote access to the DAWN daemon. **A single WebSocket server on port 3000 serves all three client types**: browser WebUI, Tier 1 satellites (Raspberry Pi), and Tier 2 satellites (ESP32). There are no separate servers or ports — each client registers its capabilities and the daemon routes messages accordingly.

| Client     | Hardware | Transport                     | Server does     | Use Case                      |
| ---------- | -------- | ----------------------------- | --------------- | ----------------------------- |
| **WebUI**  | Browser  | Opus audio (48kHz) + JSON     | ASR + LLM + TTS | Browser voice/text interface  |
| **Tier 1** | RPi 4/5  | JSON text (`satellite_query`) | LLM only        | Hands-free (local ASR/TTS)    |
| **Tier 2** | ESP32-S3 | Binary PCM audio (16kHz)      | ASR + LLM + TTS | Push-to-talk (server ASR/TTS) |

The session manager, response queue, LLM pipeline, tool system, and conversation history are shared infrastructure. Adding a new client type needs only a registration handler and a routing decision — not a new server.

**Full details**: message types, connection lifecycle, UI patterns, satellite registration, and music streaming all live in [satellite.md](docs/arch/subsystems/satellite.md). The wire protocol itself is specified in [WEBSOCKET_PROTOCOL.md](docs/WEBSOCKET_PROTOCOL.md).

---

## Command Processing

DAWN supports three parallel command-processing paths — direct regex matching, native LLM tool calls, and legacy `<command>` tags — that all converge on a single unified executor (`command_execute()`).

- **Tool registry** (`src/tools/tool_registry.c`): self-registration with FNV-1a hash tables for O(1) lookup, automatic schema generation for multiple LLM providers, and capability flags (`TOOL_CAP_NETWORK`, `TOOL_CAP_DANGEROUS`).
- **Processing mode** is selected in `dawn.toml`: `direct_only`, `llm_only`, or `direct_first`.
- **Native tools** vs. **legacy `<command>` tags** use the same enable/disable flags and the same executor; only the transport differs.

**Full flowchart, tool list, and definition sources**: see [command-processing.md](docs/arch/command-processing.md).

---

## References

- **Piper TTS**: https://github.com/rhasspy/piper
- **Vosk ASR**: https://alphacephei.com/vosk/
- **Whisper**: https://github.com/ggerganov/whisper.cpp
- **Silero VAD**: https://github.com/snakers4/silero-vad
- **llama.cpp**: https://github.com/ggerganov/llama.cpp
- **ONNX Runtime**: https://github.com/microsoft/onnxruntime
