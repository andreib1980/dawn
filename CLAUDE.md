# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

D.A.W.N. (part of The OASIS Project) is a voice-controlled AI assistant system written in C/C++ that integrates:
- Speech recognition (Whisper with GPU acceleration, Vosk optional)
- Text-to-speech (Piper with ONNX runtime)
- LLM integration (OpenAI, Claude, Ollama, or llama.cpp local models)
- MQTT command/control system
- DAP2 satellite system for distributed voice assistants (Raspberry Pi + ESP32)
- Vision AI capabilities
- Extended thinking/reasoning mode support
- Scheduler system (timers, alarms, reminders, scheduled tool execution)
- Plan executor for multi-step tool orchestration (JSON DSL, conditional logic, loops)

The project is designed for embedded Linux systems (specifically targeting Jetson platforms with CUDA support).

## Important: Working with the Developer

When the developer asks questions, they are typically asking for feedback, analysis, or suggestions FIRST - not requesting immediate implementation. Always provide your thoughts, recommendations, and discuss trade-offs before taking action. Wait for explicit confirmation (e.g., "go ahead", "do it", "yes") before implementing changes.

**CRITICAL: NEVER delete files.** Always tell the developer which files should be deleted and let them do it manually. Files may contain secrets, credentials, or other data that cannot be recovered.

**CRITICAL: NEVER run `git add` or `git commit`.** Always tell the developer which files to add and suggest a commit message. Let them run the git commands manually.

## Building the Project

### Standard Build Process

```bash
# Configure with CMake preset (creates build directory automatically)
cmake --preset debug

# Build
make -C build-debug -j8

# Run from project root (requires LD_LIBRARY_PATH for local libs)
LD_LIBRARY_PATH=/usr/local/lib ./build-debug/dawn
```

### Dependencies
The project requires many external dependencies. See `README.md` for complete installation instructions including:
- CMake 3.21+ (3.28+ for building ONNX Runtime from source)
- spdlog
- espeak-ng
- ONNX Runtime (with CUDA support)
- piper-phonemize
- Kaldi and Vosk (for speech recognition)
- PulseAudio or ALSA
- MQTT (Mosquitto)
- CUDA libraries (cuSPARSE, cuBLAS, cuSOLVER, cuRAND)

### Command Line Options
The `dawn` executable accepts various options (check `dawn.c` main function for details):
- Command processing modes (direct, LLM, or hybrid)
- Audio device selection
- Configuration file paths

## Code Formatting

**MANDATORY**: All code MUST be formatted before committing.

### C/C++ Formatting (clang-format)

```bash
# Format all code (run from repository root)
./format_code.sh

# Check formatting without modifying files
./format_code.sh --dry-run

# Check if files are properly formatted (CI mode)
./format_code.sh --check
```

The `.clang-format` configuration enforces:
- 3-space indentation (no tabs)
- 100 character line limit
- K&R brace style
- Right-aligned pointers (`int *ptr`)
- Automatic include sorting

### JavaScript/CSS/HTML Formatting (Prettier)

For web file formatting, install the optional Prettier dependency:

```bash
npm install
```

Then `./format_code.sh` will format both C/C++ and web files automatically.

The `.prettierrc` configuration enforces:
- 3-space indentation (matches C/C++)
- 100 character line limit (matches C/C++)
- Single quotes for JavaScript
- Trailing commas in ES5 style

**Note:** Prettier is optional. If not installed, `format_code.sh` will warn but still format C/C++ files.

### Git Hooks
Install the pre-commit hook to automatically check formatting:

```bash
./install-git-hooks.sh
```

This ensures code is formatted before every commit.

## Architecture

### Main Components

**dawn.c**: Main application entry point and state machine
- Handles local microphone input via state machine (SILENCE → WAKEWORD_LISTEN → COMMAND_RECORDING → PROCESSING)
- Integrates all subsystems
- Manages conversation history
- Controls application lifecycle

**mosquitto_comms.c/h**: MQTT integration
- Publishes/subscribes to MQTT topics for device control
- Device callback system for handling commands
- Integrates with home automation systems

**LLM Integration** (`src/llm/`):
- `llm_openai.c/h`: OpenAI API and OpenAI-compatible endpoints (Ollama, llama.cpp)
- `llm_claude.c/h`: Claude API with extended thinking support
- `llm_local_provider.c/h`: Auto-detection for Ollama vs llama.cpp
- `llm_streaming.c/h`: Streaming response handler with thinking content capture
- `llm_tools.c/h`: Parallel tool execution with safety classification
- Conversation context management
- Vision API integration for image analysis

**text_to_speech.cpp/h**: TTS engine wrapper
- Uses Piper for high-quality speech synthesis
- Thread-safe with mutex protection (`tts_mutex`)
- Generates WAV output from text

**text_to_command_nuevo.c/h**: Command parsing and execution
- Parses LLM responses for `<command>` JSON tags
- Routes commands to appropriate device callbacks
- Supports both direct pattern matching and LLM-based command processing

**llm_command_parser.c/h**: JSON command extraction
- Extracts and validates JSON commands from LLM responses
- Handles malformed JSON gracefully

**logging.c/h**: Centralized logging
- Use `LOG_INFO()`, `LOG_WARNING()`, `LOG_ERROR()` macros
- Provides consistent log formatting

**webui_server.c/h**: Web-based configuration interface
- WebSocket-based real-time communication
- Serves static files from `www/` directory
- Handles configuration get/set via JSON messages
- Dynamic model/interface discovery endpoints
- SSL/TLS support optional

### Key Configuration

**dawn.toml**: Primary runtime configuration file (TOML format)
- `[general]`: AI name, timezone, locale settings
- `[llm]`: Provider selection (`type = "cloud"` or `"local"`)
- `[llm.cloud]`: OpenAI/Claude settings, model arrays for runtime switching
- `[llm.local]`: Ollama/llama.cpp endpoint, model, provider auto-detection
- `[llm.thinking]`: Extended thinking settings (mode, budget_tokens, reasoning_effort)
- `[asr]`: Speech recognition settings, model path, language
- `[tts]`: Text-to-speech settings, voice model, sample rate
- `[audio]`: Backend (auto/pulse/alsa), device selection
- `[network]`: Network settings (session timeout, LLM timeout, worker count)
- `[webui]`: Web interface bind address, port, SSL settings
- `[scheduler]`: Timers/alarms settings (snooze, timeout, volume, limits)
- `[mqtt]`: MQTT broker connection settings

**dawn.h** contains compile-time defaults:
- `AI_NAME`: Default wake word ("friday")
- `AI_DESCRIPTION`: System prompt for LLM defining personality and behavior
- `DEFAULT_PCM_PLAYBACK_DEVICE` / `DEFAULT_PCM_CAPTURE_DEVICE`: Audio devices
- `MQTT_IP` / `MQTT_PORT`: MQTT broker configuration

**secrets.toml**: Runtime API keys and credentials (gitignored). Example:
```toml
openai_api_key = "sk-..."
claude_api_key = "sk-ant-..."

[secrets.google]
client_id = "123456789-abc.apps.googleusercontent.com"
client_secret = "GOCSPX-..."
redirect_url = "https://jetson.yourdomain.com:3000/oauth/callback"
```

### DAP2 Satellite System

DAP2 is the unified WebSocket protocol for all satellite devices ("JARVIS in every room"). All tiers connect via WebSocket on the same port as WebUI (default 3000):
- **Tier 1** (RPi): Text-first — local ASR/TTS, sends only transcribed text to daemon
- **Tier 2** (ESP32): Audio path — streams raw PCM, server handles ASR/TTS (reuses WebUI audio pipeline in `webui_audio.c`)
- Capability-based routing: daemon inspects registration capabilities to choose text vs audio path

**Satellite binary**: `dawn_satellite/` - standalone C application using libwebsockets

See `docs/WEBSOCKET_PROTOCOL.md` for the wire protocol reference and `docs/DAP2_SATELLITE.md` for Tier 1 build/config/deployment.

## Development Guidelines

### Coding Standards

Follow `CODING_STYLE_GUIDE.md` strictly:

**Naming**:
- Functions and variables: `snake_case`
- Constants and macros: `UPPER_CASE`
- Types: `typedef` with `_t` suffix (e.g., `device_type_t`)

**Error Handling**:
- Use `SUCCESS` (0) and `FAILURE` (1) for return values
- Define specific error codes > 1 for detailed errors
- **DO NOT** use negative return values (-1, negative errno)
- Always check return values from functions that can fail

**Memory Management**:
- Prefer static allocation for embedded systems
- Minimize dynamic allocation (malloc/calloc)
- Always check for NULL after allocation
- Free and NULL: `free(ptr); ptr = NULL;`

**Functions**:
- Soft target: < 50 lines
- No hard limits - clarity over arbitrary counts
- Inputs first, outputs last in parameters

**Comments**:
- Use Doxygen-style for public APIs
- Comment the "why" not the "what"
- File headers include GPL license block (see below)

**File Header (REQUIRED for all new .c/.cpp/.h files)**:
```c
/*
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 *
 * By contributing to this project, you agree to license your contributions
 * under the GPLv3 (or any later version) or any future licenses chosen by
 * the project author(s). Contributions include any modifications,
 * enhancements, or additions to the project. These contributions become
 * part of the project and are adopted by the project author(s).
 *
 * [Brief description of file purpose]
 */
```

### Thread Safety

When working with shared resources:
- **ASR Model** (Whisper/Vosk): Read-only, create separate recognizers per thread
- **TTS Engine**: Already mutex-protected (`tts_mutex`)
- **LLM Endpoint**: Handles concurrent HTTP requests
- **Conversation History**: Needs mutex protection when implementing multi-client
- **Local Provider Detection**: Cached with mutex protection (5-minute TTL)

### File Size Monitoring

**Proactively warn** when files approach size limits:

- **1,500+ lines (C)** or **1,000+ lines (JS)**: Mention that the file is getting large
- **2,500+ lines**: Recommend splitting before adding more features
- **New feature in large file**: Suggest creating a separate module instead

**When asked to add features to large files**, propose creating a new module rather than expanding the existing file.

### Refactoring Large Files

If asked to refactor a large file:

1. **Never attempt full rewrites** - They frequently fail due to interconnected features
2. **Use incremental extraction** - One feature at a time
3. **Keep original working** - Extract into new file, import back, test
4. **Test after each extraction** - Don't batch multiple extractions

### Common Patterns

**Command Callbacks**:
```c
char *myCallback(const char *actionName, char *value, int *should_respond) {
   // should_respond: set to 1 to return data to AI, 0 to handle directly
   // Return: allocated string for AI (if should_respond=1), or NULL
}
```

**Logging**:
```c
LOG_INFO("System initialized");
LOG_WARNING("Battery voltage low: %.2fV", voltage);
LOG_ERROR("I2C communication failed: %d", error);
```

**Tool Registration** (modular approach - preferred for new tools):

Tools are self-contained modules in `src/tools/` with their own metadata, config, and callbacks:

```c
/* In src/tools/my_tool.c */
static const tool_metadata_t my_tool_metadata = {
   .name = "my_tool",
   .device_string = "my device",
   .description = "Tool description for LLM",
   .params = my_tool_params,
   .param_count = 2,
   .device_type = TOOL_DEVICE_TYPE_GETTER,
   .capabilities = TOOL_CAP_NETWORK,
   .is_getter = true,
   .callback = my_tool_callback,
};

int my_tool_register(void) {
   return tool_registry_register(&my_tool_metadata);
}
```

Register tools in `src/tools/tools_init.c` via `tools_register_all()`. The tool_registry provides O(1) lookup via FNV-1a hash tables for name, device_string, and aliases.

**Legacy Device Registration** (in mosquitto_comms.c - for core system devices):
```c
deviceCallback callbacks[] = {
   { DEVICE_TYPE, myCallback },
   // ...
};
```

## Satellite Development

**Tier 1 (Raspberry Pi)**: Full satellite binary in `dawn_satellite/`. See `docs/DAP2_SATELLITE.md`.

**Tier 2 (ESP32)**: Arduino-based satellite using arduinoWebSockets (Links2004) to connect via WebSocket. Streams raw PCM audio (16-bit, 16kHz, mono) using binary message types 0x01/0x02 (audio in) and receives TTS audio via 0x11/0x12 (audio out). See `docs/WEBSOCKET_PROTOCOL.md` for protocol details.

## Testing

Unit tests in `tests/` (standalone binaries, no framework):
- `test_scheduler` — Scheduler DB layer (94 assertions across 16 tests)
- `test_music_db` — Music DB search, dedup, browse, source abstraction (60 assertions)
- `test_document_chunker` — Text chunking for RAG (34 assertions, 10 tests)
- `test_document_db` — Document DB CRUD, isolation, global flag, chunks (65 assertions, 14 tests)
- `test_embedding_engine` — L2 norm, cosine similarity, edge cases (24 assertions, 9 tests)
- `test_memory_embeddings` — Memory embedding math utilities
- `test_sse_parser` — SSE stream parser
- `test_sentence_buffer` — Sentence boundary detection
- `test_session_commands` — Thread-local session context
- `test_plan_executor` — Plan executor DSL parsing, execution, safety limits (140 assertions, 30+ tests)
- `test_instruction_loader` — Two-step instruction loader file I/O, path traversal, edge cases (42 assertions)
- `test_wake_word` — Wake word matching, normalization, command extraction
- `test_document_extract` — Extension validation, Content-Type mapping, extraction, magic bytes, error codes (81 assertions)
- `test_phone_db` — Phone call/SMS log CRUD, user isolation, retention cleanup, edge cases (26 assertions)

Build and run: `make -C build-debug test_scheduler && ./build-debug/tests/test_scheduler`

Manual testing covers:
- Local microphone wake word detection
- Voice command processing
- WebUI and satellite connections
- MQTT command execution
- Vision AI processing

## Important Files to Know

**Configuration:**
- `dawn.toml`: Runtime configuration file (TOML format)
- `secrets.toml`: API keys and credentials (gitignored)
**Code Formatting:**
- `.clang-format`: C/C++ formatting rules (3-space indent, 100 char lines)
- `.prettierrc`: JS/CSS/HTML formatting rules (matching style)
- `.prettierignore`: Files to exclude from Prettier
- `package.json`: npm config for Prettier (optional)
- `pre-commit.hook`: Git pre-commit hook for formatting

**Models:**
- `models/whisper.cpp/`: Whisper ASR models directory (primary)
- `vosk-model-en-us-0.22/`: Vosk ASR model (optional fallback)
- `models/*.onnx*`: TTS voice models

**Generated:**
- `version.h`: Auto-generated version info with git SHA

**WebUI:**
- `www/`: WebUI static files
- `www/css/main.css`: CSS entry point with @import statements (modular CSS)
- `www/js/core/`: Core JS modules (constants, websocket, audio)
- `www/js/ui/`: UI JS modules (settings, history, themes)

**Scheduler:**
- `include/core/scheduler.h`: Scheduler public API (init, shutdown, dismiss, snooze)
- `include/core/scheduler_db.h`: Database layer (CRUD, queries, string helpers)
- `src/core/scheduler.c`: Background thread, event firing, chime audio, recurrence
- `src/core/scheduler_db.c`: SQLite operations (uses auth_db shared handle)
- `src/tools/scheduler_tool.c`: LLM tool interface (create/list/cancel/query/snooze/dismiss)
- `www/js/ui/scheduler.js`: WebUI notification banners with dismiss/snooze
- `www/css/components/scheduler.css`: Notification banner styles
- `docs/SCHEDULER_DESIGN.md`: Full design document with automated test coverage

**Document Search / RAG:**
- `include/core/embedding_engine.h`: Shared embedding API (embed, cosine, norms)
- `src/core/embedding_engine.c`: Embedding engine (ONNX/Ollama/OpenAI, NEON vectorized)
- `include/tools/document_db.h`: Document DB types and CRUD declarations
- `src/tools/document_db.c`: SQLite operations for documents and chunks
- `src/tools/document_search.c`: Semantic search tool (hybrid cosine + keyword)
- `src/tools/document_read.c`: Paginated document reader tool
- `src/tools/document_index_tool.c`: URL-based document download and indexing tool
- `include/tools/document_extract.h`: Shared text extraction API (PDF, DOCX, HTML, plain text)
- `src/tools/document_extract.c`: Format-specific extraction (MuPDF, libzip+libxml2, html_parser)
- `include/tools/document_index_pipeline.h`: Shared indexing pipeline API (chunk, embed, store)
- `src/tools/document_index_pipeline.c`: SHA-256 dedup, chunking, embedding, DB storage
- `src/tools/document_chunker.c`: Text chunking for embedding
- `src/webui/webui_doc_library.c`: WebUI Document Library endpoints (delegates to shared pipeline)
- `www/js/ui/doc-library.js`: Document Library frontend
- `www/css/components/doc-library.css`: Document Library styles
- `docs/RAG_DESIGN.md`: Full design document with implementation notes

**Calendar (CalDAV):**
- `include/tools/caldav_client.h`: CalDAV protocol types and API
- `src/tools/caldav_client.c`: RFC 4791 protocol (discovery, REPORT, PUT/DELETE)
- `include/tools/calendar_db.h`: Calendar DB types (accounts, calendars, events, occurrences)
- `src/tools/calendar_db.c`: SQLite CRUD (uses auth_db shared handle, Pattern A)
- `include/tools/calendar_service.h`: Service API (lifecycle, queries, mutations)
- `src/tools/calendar_service.c`: Business logic (multi-account, background sync, RRULE expansion, Google-specific discovery)
- `include/tools/calendar_tool.h`: Tool registration header
- `src/tools/calendar_tool.c`: LLM tool interface (today/range/next/search/add/update/delete)

**OAuth 2.0 / Crypto:**
- `include/tools/oauth_client.h`: OAuth client API (auth URL, code exchange, token refresh, storage)
- `src/tools/oauth_client.c`: OAuth implementation (PKCE S256, Google provider, encrypted token DB)
- `include/core/crypto_store.h`: Shared libsodium encryption API
- `src/core/crypto_store.c`: Shared encryption module (crypto_secretbox, dawn.key)
- `www/js/ui/oauth.js`: OAuth popup handler (blocker mitigation, origin validation)
- `www/js/oauth-callback.js`: OAuth callback page script (postMessage to opener)
- `docs/GOOGLE_OAUTH_SETUP.md`: Google OAuth setup guide

**Memory System:**
- `include/memory/memory_db.h`: Entity/relation CRUD, entity merge API, entity photo get/set
- `src/memory/memory_db.c`: SQLite operations, transactional entity merge with dedup
- `src/memory/memory_callback.c`: LLM tool callback (9 actions incl. merge_entities)
- `include/memory/contacts_db.h`: Contacts CRUD API (find, add, update, delete, list), photo_id in results
- `src/memory/contacts_db.c`: Contacts SQLite operations with LIKE escape
- `include/webui/webui_contacts.h`: Contacts WebSocket handler declarations
- `src/webui/webui_contacts.c`: Contacts WebSocket handlers (list, add, update, delete, search, set_photo, entity_ensure)
- `www/js/ui/contacts.js`: Contacts tab UI + add/edit modal + photo upload/thumbnails
- `www/css/components/contacts.css`: Contact card, modal, and photo styles
- `docs/MEMORY_SYSTEM_DESIGN.md`: Full design document (Phases 1-6.5, S4, entity merge)

**Email (IMAP/SMTP):**
- `src/tools/email_tool.c`: LLM tool interface (10 actions)
- `src/tools/email_service.c`: Multi-account routing, drafts, trash, auth dispatch
- `src/tools/email_client.c`: IMAP/SMTP via libcurl
- `src/tools/gmail_client.c`: Gmail REST API backend (OAuth accounts)
- `src/tools/email_db.c`: Email account DB operations
- `src/webui/webui_email.c`: WebSocket handlers for account CRUD
- `www/js/ui/email-accounts.js`: Email account management UI

**Plan Executor:**
- `include/tools/plan_executor.h`: Public API, constants, data structures
- `src/tools/plan_executor.c`: Plan parser, executor, condition evaluator, arg builder (~580 lines)
- `src/tools/plan_executor_tool.c`: Tool registry metadata and callback wrapper
- `tests/test_plan_executor.c`: Unit tests (130 assertions, 30 tests)
- `docs/TOOL_PLAN_EXECUTOR_DESIGN.md`: Full design document with DSL spec

**Always-On Voice (WebUI):**
- `src/webui/webui_always_on.c`: Server-side state machine, VAD, wake word dispatch
- `include/webui/webui_always_on.h`: Always-on context, state enum, timeouts
- `src/core/wake_word.c`: Shared wake word matching (used by local mic, always-on, satellite)
- `www/js/audio/always-on.js`: Browser coordinator (mode management, resolveButtonState, dropdown, text override)
- `www/js/ui/user-badge.js`: User badge dropdown (extracted from dawn.js)

**Visual Rendering:**
- `src/tools/render_visual_tool.c`: Two-step tool (load guidelines + render SVG/HTML)
- `src/tools/instruction_loader.c`: Generic markdown file loader for two-step pattern
- `include/tools/instruction_loader.h`: Instruction loader API
- `www/js/ui/visual-render.js`: Sandboxed iframe renderer, theme injection, color ramps, download button
- `www/css/components/visual-render.css`: Visual container and download button styles
- `tool_instructions/render_visual/`: Design guideline modules (_core, diagram, chart, interactive, art, mockup)
- `www/js/vendor/chart.umd.js`: Bundled Chart.js 4.4.1 (MIT) for offline chart rendering
- Two-step pattern design: archived to [atlas](https://github.com/The-OASIS-Project/atlas/blob/main/dawn/archive/TWO_STEP_TOOL_PATTERN.md)
- `docs/VISUAL_RENDERING_TOOL.md`: Visual rendering design document

**SFX (Sound Effects):**
- `include/tools/sfx_tool.h`: SFX tool registration header
- `src/tools/sfx_tool.c`: Sound effect playback (4 concurrent slots, per-slot stop, path validation, rate limiting)
- `include/audio/audio_utils.h`: Shared audio_apply_volume() inline function
- `sound_assets/`: Sound effect files (ogg) played by DAWN on behalf of SPARK/MIRAGE

**Phone (Call & SMS via ECHO modem daemon):**
- `include/tools/phone_tool.h`: Phone tool registration header
- `src/tools/phone_tool.c`: LLM tool interface (10 actions, two-step confirmation, contact resolution)
- `include/tools/phone_service.h`: Phone service API (state machine, events, MQTT, config with user_id)
- `src/tools/phone_service.c`: Call state machine, ECHO MQTT event handling, TTS, HUD with contact photos (base64), rate limiting
- `include/tools/phone_db.h`: Phone DB types (call_log, sms_log) and CRUD declarations
- `src/tools/phone_db.c`: SQLite operations for call/SMS logs (Pattern A, shared auth_db handle)
- `tests/test_phone_db.c`: Phone DB unit tests (26 assertions)
- `docs/PHONE_SMS_DESIGN.md`: Full design document (ECHO + DAWN integration, Phases 1-6)

**Image Store + Image Search:**
- `include/image_store.h`: Image store API (save/save_ex/get_path/delete/update_retention, source/retention enums)
- `src/image_store.c`: Filesystem-backed storage, atomic writes, LRU cache cleanup, retention policies, retention lifecycle
- `src/webui/webui_images.c`: HTTP endpoints (upload + zero-copy download via lws_serve_http_file, Bearer token auth for service access)
- `include/tools/image_search_tool.h`: Image search tool registration header
- `src/tools/image_search_tool.c`: SearXNG image search, curl_multi fetch, SSRF DNS pinning, redirect-with-revalidation, magic byte validation
- `docs/UNIFIED_IMAGE_STORE_DESIGN.md`: Full design document (Phases 1-6)
- `docs/WEB_IMAGE_SEARCH.md`: Image search tool design document

**Satellite (DAP2):**
- `dawn_satellite/`: Standalone satellite binary for Raspberry Pi
- `dawn_satellite/config/satellite.toml`: Default satellite configuration
- `dawn_satellite/src/ws_client.c`: WebSocket client for daemon communication
- `dawn_satellite/src/satellite_config.c`: TOML configuration loader
- `src/webui/webui_satellite.c`: Daemon-side satellite message handlers
- `docs/DAP2_SATELLITE.md`: Satellite architecture and deployment guide

## Known Issues and TODOs

1. SmartThings OAuth blocked at AWS WAF level (403 Forbidden)

**Recently Completed:**
- Google OAuth 2.0 for CalDAV (shared oauth_client + crypto_store modules, PKCE S256, WebUI popup flow)
- CalDAV calendar integration (multi-account, RFC 4791 discovery, Google OAuth, RRULE expansion, background sync, LLM tool)
- Document search/RAG system with semantic search, paginated document reader, WebUI Document Library, shared embedding engine, admin document management (global toggle, all-users view, username resolution)
- Scheduler system (timers, alarms, reminders, scheduled tool execution, briefings) with audible chimes, WebUI notifications, recurrence, snooze/dismiss
- Satellite registration key (pre-shared key authentication for satellite registration)
- Private CA for TLS validation (all client types: ESP32, RPi, browser)
- LLM playlist builder (add/remove/clear_queue actions) with genre search
- Genre extraction and DB indexing for music library
- Music/TTS race condition fixes (pause daemon during TTS)
- Server-side shuffle/repeat for music playback
- Tier 2 ESP32 satellite (Arduino-based)
- SDL UI themes and screensaver
- DAP1 removal and common library consolidation
- Modular tool registry system with O(1) hash lookups
- Parallel tool execution for concurrent API calls
- Extended thinking/reasoning mode (Claude, OpenAI, local models)
- Email integration with contacts system (IMAP/SMTP, Gmail REST API, multi-account, WebUI management)
- Contacts WebUI (5th Memory tab, WebSocket CRUD, modal add/edit, entity cross-linking from Graph tab)
- Entity merge (transactional SQL, relation/contact dedup, LLM tool + WebUI two-click merge)
- OAuth multi-account fix (always launch popup for account picker, button state reset on success)
- Plan executor (multi-step tool orchestration DSL with conditional logic, loops, variable binding, safety controls)
- Session lifecycle hardening (auto-create on expiry, race-safe conversation restore)
- Memory forget-by-ID (exact text/ID match replaces index guessing)
- Orchestrator UI (WebUI plan execution debug display)
- Always-on continuous voice listening via WebUI (server-side VAD, wake word detection, TTS echo prevention, per-connection context)
- x86_64 server mode with CUDA auto-detect and cross-platform build fixes
- Unified action button (Send/Mic/Listen merged into single split button with dropdown mode selection, smart typing override, cancel state)
- Shared CSS split-button primitives (`.dawn-split-btn` / `.dawn-split-chevron` / `.dawn-split-menu` in `components.css`)
- User badge module extraction (`www/js/ui/user-badge.js` from `dawn.js`)
- Two-step instruction loader pattern (generic markdown file reader, path traversal sanitization, 42 unit tests)
- Visual rendering tool Phase 1 (inline SVG diagrams via LLM tool calling, sandboxed iframe, theme CSS, color ramps, design guidelines on disk)
- LLM_TOOLS_ARGS_LEN bumped from 4KB to 16KB for large tool arguments (SVG code)
- Visual rendering Phase 2: persistence, interactivity, and review fixes
- Shared CSS split-button primitives (`.dawn-split-btn` / `.dawn-split-chevron` / `.dawn-split-menu` in `components.css`)
- User badge module extraction (`www/js/ui/user-badge.js` from `dawn.js`)
- Chart.js bundled locally (`www/js/vendor/chart.umd.js`, MIT license)
- All 5 visual guideline modules: diagram, chart, interactive, art, mockup
- Visual download button (SVG/HTML export matching copy-btn pattern)
- Inline visual positioning (text → diagram → text within one message, persists on reload)
- Fix: thinking content leak into chat (tool-use fallback + raw `<thinking>` tag stripping)
- URL fetcher: JSON content type support (`application/json`), summarizer bypass for JSON
- url_fetch whitelisted for plan executor (`TOOL_CAP_SCHEDULABLE`)
- RAG self-population: `document_index` LLM tool (URL download, PDF/DOCX/HTML/text extraction, FlareSolverr fallback, SSRF-safe DNS pinning, rate limiting, 81 unit tests)
- Shared document_extract module (refactored from webui_documents.c) and document_index_pipeline (refactored from webui_doc_library.c)
- URL fetcher: HTTP/2 FlareSolverr fallback, IPv4-mapped IPv6 SSRF fix
- Plan executor: sleep step (1-300s), configurable timeout via dawn.toml
- Automated install script overhaul: cmake --install for deploy (proper RPATH), settings cache with --fresh, random admin passwords, set -e safety fixes, FHS service layout (/var/lib/dawn/db)
- Scheduler briefings: SCHED_EVENT_BRIEFING (tool execution → LLM summarization → persistent conversation → TTS + WebUI notification with View button)
- DANGEROUS tools allowed in scheduler (user authorized at scheduling time), search + email now TOOL_CAP_SCHEDULABLE
- DANGEROUS tools default to enabled in registry (config `enabled = false` to opt out, no silent disable)
- Recurring TASK events fixed (schedule_next_occurrence was missing from TASK fire path)
- SCHED_TOOL_VALUE_MAX increased 256→2048 with creation-time overflow rejection
- Claude thinking signature: fixed 4KB buffer → dynamic realloc (8KB initial, grows as needed)
- Source-aware scheduler notification routing: WebUI events play TTS in browser (not daemon speaker), satellite fallback to user's other sessions, source_client_type tracking (v28 DB migration), per-session TTS with state bracketing, announce_all dedup fix
- Docker multi-arch image (amd64 + arm64): multi-stage Dockerfile (deps → builder → runtime), debian:bookworm-slim, non-root user, SHA256-verified ONNX, pinned from-source deps, 297MB runtime
- GitHub Actions CI pipeline: format-check (clang-format-14 + Prettier) → unit-tests (ci preset, 13 tests) + docker-build (full image + smoke test), SHA-pinned actions, least-privilege permissions
- CUDA Docker variant (Dockerfile.cuda): nvidia/cuda base, ONNX Runtime from source with --use_cuda, cuDNN, memory-safe build (--parallel 2 --nvcc_threads 1), 3.55GB runtime
- SFX tool: concurrent sound effect playback (4 slots, path traversal validation, rate limiting, list action) via MQTT from MIRAGE/SPARK
- Shared audio_apply_volume() extracted to include/audio/audio_utils.h
- Audio backend playback handle pool increased from 2 to 6 (ALSA + PulseAudio)
- Fix: skip_followup tools (SFX, TTS, reset_conversation) no longer trigger false "unavailable" error on voice pipeline (thread-local flag in llm_tool_loop)
- Server-side TTS audio pacing: sleep-based real-time delivery prevents mobile WebUI audio skips, syncs text + audio output (1s lookahead, per-connection state, stream reset detection)
- TTS preprocessing: strip markdown header markers (# through ######) so headings are spoken as plain text
- URL fetch timeout defaults reduced (document_index 30s→10s, FlareSolverr 60s→10s)
- Phone call & SMS system: ECHO modem daemon (standalone, ~/code/The-OASIS-Project/echo/) + DAWN phone_tool.c, phone_service.c, phone_db.c
- Phone tool: 10 actions (call, confirm_call, answer, hang_up, send_sms, confirm_sms, read_sms, call_log, sms_log, status), two-step confirmation, contact resolution, rate limiting
- Phone DB: call_log + sms_log tables (schema v29), Pattern A shared auth_db handle, 26-assertion unit test
- UCS2/emoji SMS: modem kept in UCS2 charset with AT+CSMP DCS=8, UTF-8↔UCS2 hex encoding in ECHO
- Contacts validation: email format check, phone E.164 normalization (10-digit US → +1), client + server validation
- Entity search fix: memory_db_entity_search missing OFFSET bind on shared prepared statement
- Image store filesystem migration: BLOB→filesystem, zero-copy HTTP serving via lws_serve_http_file(), source/retention enums (upload/generated/search/MMS/document × default/permanent/cache), schema v30, atomic writes (tmp+fsync+rename), LRU cache eviction, rate-limited last_accessed
- Image search tool: SearXNG image search via `web_search_query_images_raw()`, curl_multi concurrent fetch (4 parallel connections, 10s wall-clock cap), SSRF DNS pinning + manual redirect-with-revalidation, magic byte validation (JPEG/PNG/GIF/WebP), 1MB fetch cap, image store caching (SOURCE_SEARCH + RETAIN_CACHE)
- WebUI image lightbox: click-to-enlarge overlay with blur backdrop, Escape/backdrop/X close, focus trap, ARIA dialog, prefers-reduced-motion, WCAG touch target
- Contact photos: schema v31 (photo_id on memory_entities), entity_set_photo/entity_ensure WebSocket handlers, WebUI upload with compression (256px, JPEG 85%), circular thumbnails in contact cards, photo-only save flow, image retention lifecycle (PERMANENT when bound, DEFAULT when replaced), `image_store_update_retention()` API
- Phone HUD contact photos: base64-encoded photo in MQTT HUD messages (incoming_call, call_active, sms_received), entity_id tracking in call state machine, `phone_service_config_t.user_id` replacing hardcoded user IDs
- Contacts modal accessibility: keyboard focus trap (Tab/Shift+Tab wrap), focus-visible outlines, ARIA roles, mobile touch targets, image error fallbacks
- Service token Bearer auth: machine-to-machine image API access for MIRAGE, constant-time `sodium_memcmp` over padded buffer, rate-limit-before-auth (120/min, 32 IP slots), scoped access (blocks private uploads/MMS), min 32-char token, TLS warning

## Development Lifecycle

The full cycle for implementing features in this project. Steps may repeat — testing often reveals adjacent bugs that loop back to implement/review.

### 1. Plan (if non-trivial)
- Use plan mode for features that touch multiple subsystems
- Launch Explore agents to understand existing code, then Plan agent to design the approach
- Run architecture-reviewer and/or ui-design-architect on the plan before implementing
- Incorporate agent feedback into the plan before exiting plan mode

### 2. Implement
- Build from an existing plan if one exists (check `~/.claude/plans/`)
- Use task tracking for multi-step work
- Build and format check after each logical chunk: `make -C build-debug -j8` + `./format_code.sh --check`
- Run relevant unit tests: `./build-debug/tests/test_<name>`

### 3. Review
- Run all relevant review agents in parallel on the diff
- Synthesize findings into a consolidated table with severity and action (Fix/Skip/Ask)
- Triage: fix real bugs, skip nitpicks, ask about anything that would change public APIs
- Apply fixes, rebuild, re-verify format and tests
- For critical fixes: have the architecture-reviewer verify the fix is correct

### 4. Test
- Developer tests manually and reports results (logs, behavior observations)
- Fix issues found in testing — these often reveal adjacent bugs worth fixing
- Adjacent bugs may require their own mini review cycle
- Iterate until the developer is satisfied

### 5. Document
- Update the atlas design doc if one exists (e.g., `~/code/The-OASIS-Project/atlas/dawn/archive/`)
- Have the architecture-reviewer verify the design doc against the actual code
- Fix any discrepancies found

### 6. Update Planning Docs
- **CLAUDE.md**: Add to "Recently Completed" list
- **docs/TODO.md**: Move item from active to shipped, update shipped list. If the item had a §N detail section, remove it.
- **NEVER commit**: `docs/TODO.md` (developer-maintained)

### 7. Commit
- Run `./format_code.sh --check` one final time
- Provide a single `git add` command with all relevant files
- Suggest a commit message (present tense, summary line + bullet details)
- **NEVER run `git add`, `git commit`, or `git push`** — the developer does this
- Wait for the developer to confirm before moving on

## Code Review Workflow

When the developer requests a "code review" (or similar phrasing like "review my changes", "run the agents on this"):

1. **Gather changes**: Run `git status` and `git diff` to capture all current uncommitted changes
2. **Launch agents in parallel**: Feed the diff output to the appropriate review agents:
   - **"Code review"** or **"run the big three"**: Launch architecture-reviewer, embedded-efficiency-reviewer, and security-auditor in parallel
   - **"Full review"** or **"run all four"**: Also include ui-design-architect (if UI changes are present)
3. **Synthesize results**: After all agents complete, provide a consolidated summary of findings

**Trigger phrases:**
- "code review", "review my changes", "review this code"
- "run the agents", "run the big three", "run all my agents"
- "what do the agents think?"

## Agent Terminology

When the developer refers to review agents:
- **"The big three"** or **"the main three"**: architecture-reviewer, embedded-efficiency-reviewer, security-auditor
- **"All four"** or **"all my agents"**: The above three plus ui-design-architect

## License

GPLv3 or later. All source files include GPL header block.
