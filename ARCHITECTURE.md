# D.A.W.N. System Architecture

This document describes the architecture of the D.A.W.N. (Digital Assistant for Workflow Neural-inference) voice assistant system.

**D.A.W.N.** is the central intelligence layer of the OASIS ecosystem, responsible for interpreting user intent, fusing data from every subsystem, and routing commands. At its core, DAWN performs neural-inference to understand context and drive decision-making, acting as OASIS's orchestration hub for MIRAGE, AURA, SPARK, STAT, and any future modules.

**Last Updated**: March 26, 2026 (Visual rendering tool, two-step instruction loader, JSON URL fetching)

## Table of Contents

- [Directory Structure](#directory-structure)
- [High-Level Overview](#high-level-overview)
- [Subsystem Architecture](#subsystem-architecture)
- [Data Flow](#data-flow)
- [Threading Model](#threading-model)
- [State Machine](#state-machine)
- [Network Protocol](#network-protocol)
- [DAP2 Satellite Protocol](#dap2-satellite-protocol)
- [Command Processing Architecture](#command-processing-architecture)
- [Component Interactions](#component-interactions)
- [Memory Management](#memory-management)
- [Error Handling](#error-handling)
- [Performance Optimization](#performance-optimization)
- [Design Decisions](#design-decisions)

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
├── www/                    # Web UI static files (HTML, CSS, JS)
├── models/                 # ML models (TTS voices, VAD)
├── whisper.cpp/            # Whisper ASR engine (git submodule)
├── common/                 # Shared library (VAD, ASR, TTS, logging) for daemon + satellite
├── dawn_satellite/         # DAP2 Tier 1 satellite (Raspberry Pi, SDL2 UI)
├── dawn_satellite_arduino/ # DAP2 Tier 2 satellite (ESP32-S3, Arduino sketch)
├── services/               # Systemd service files
├── tests/                  # Test programs
├── llm_testing/            # LLM benchmarking tools
├── docs/                   # Additional documentation
│
├── dawn.toml.example       # Configuration template
├── secrets.toml.example    # API keys template
├── setup_models.sh         # Model download script
├── format_code.sh          # Code formatting script
├── generate_ssl_cert.sh    # SSL certificate generator
└── CMakeLists.txt          # Build configuration
```

---

## High-Level Overview

D.A.W.N. is a modular voice assistant system that processes voice commands through a pipeline of specialized subsystems:

```
┌─────────────────────────────────────────────────────────────┐
│                      DAWN Main Loop                         │
│  (src/dawn.c - State Machine: SILENCE → WAKEWORD → COMMAND  │
│   → PROCESSING)                                             │
└────────┬──────────────────────────────┬─────────────────────┘
         │                              │
         │ Local Audio                  │ WebSocket (WebUI + Satellites)
         │                              │
    ┌────▼─────────┐            ┌───────▼──────────┐
    │ Audio Capture│            │  WebUI Server    │
    │ Thread       │            │ (libwebsockets)  │
    │ + Ring Buffer│            │ Multi-client     │
    └────┬─────────┘            └───────┬──────────┘
         │                              │
    ┌────▼──────────┐           ┌───────▼──────────┐
    │  VAD (Silero) │           │ Session Manager  │
    │  Speech/Noise │           │ + Audio Workers  │
    └────┬──────────┘           └───────┬──────────┘
         │                              │
    ┌────▼──────────┐                   │
    │ ASR Interface │                   │
    │ (abstraction) │                   │
    └────┬──────────┘                   │
         │                              │
    ┌────▼──────────┐                   │
    │ Vosk | Whisper│                   │
    │ (+ GPU accel) │                   │
    └────┬──────────┘                   │
         │                              │
         └──────────┬───────────────────┘
                    │
            ┌───────▼──────────┐
            │  LLM Interface   │
            │  (abstraction)   │
            └───────┬──────────┘
                    │
         ┌──────────┼─────────┬──────────┐
         │          │         │          │
    ┌────▼───┐ ┌───▼────┐ ┌──▼───┐ ┌───▼──────┐
    │ OpenAI │ │ Claude │ │Gemini│ │ llama.cpp│
    │ GPT-5  │ │ 4.6    │ │ 2.5  │ │ (local)  │
    └────┬───┘ └───┬────┘ └──┬───┘ └───┬──────┘
         │         │         │         │
         └─────────┴────┬────┴─────────┘
                   │ (streaming)
           ┌───────▼──────────┐
           │  SSE Parser +    │
           │ Sentence Buffer  │
           └───────┬──────────┘
                   │
           ┌───────▼──────────┐
           │   TTS (Piper)    │
           │  + Preprocessing │
           └───────┬──────────┘
                   │
           ┌───────▼──────────┐
           │ ALSA/PulseAudio  │
           │   Playback       │
           └──────────────────┘
```

### Core Design Principles

1. **Modularity**: Each subsystem has a clear interface and can be replaced independently
2. **Performance**: GPU acceleration on Jetson, optimized local LLM inference
3. **Reliability**: Retry logic, checksums, error recovery in network protocol
4. **Flexibility**: Support for multiple ASR engines, LLM providers, and audio backends
5. **Embedded-First**: Designed for resource-constrained platforms (static allocation preferred)

---

## Subsystem Architecture

### 1. Core Subsystem (`src/` root)

**Purpose**: Main application logic, logging, utilities, MQTT integration

#### Key Components

- **dawn.c/h**: Main application entry point
   - State machine for local audio processing
   - Integration point for all subsystems
   - Conversation history management
   - Application lifecycle control

- **logging.c/h**: Centralized logging system
   - Macros: `LOG_INFO()`, `LOG_WARNING()`, `LOG_ERROR()`
   - Timestamp formatting
   - Consistent log formatting across all subsystems

- **mosquitto_comms.c/h**: MQTT integration
   - MQTT client for pub/sub messaging
   - Device callback registration system
   - Command routing to device handlers
   - Integration with other OASIS components or external systems

- **text_to_command_nuevo.c/h**: Command parsing and execution
   - Parses LLM responses for `<command>` JSON tags
   - Routes commands to appropriate device callbacks
   - Supports both direct pattern matching and LLM-based commands

- **word_to_number.c/h**: Natural language number parsing
   - Converts text numbers to integers ("twenty-three" → 23)
   - Used for command parsing

### 2. ASR Subsystem (`src/asr/`, `include/asr/`)

**Purpose**: Speech recognition with multiple engine support

#### Architecture Pattern: **Strategy Pattern**

The ASR subsystem uses an abstraction layer (`asr_interface`) to support multiple ASR engines (Whisper, Vosk) interchangeably.

#### Key Components

- **asr_interface.c/h**: ASR abstraction layer
   - `ASRContext` struct: Engine-agnostic context
   - `asr_init()`: Initialize selected ASR engine
   - `asr_process_audio()`: Process audio through selected engine
   - `asr_cleanup()`: Clean up resources
   - Engine selection based on compile-time flags (`ENABLE_VOSK`)

- **asr_whisper.c/h**: Whisper ASR implementation
   - Uses whisper.cpp library
   - GPU acceleration on Jetson (CUDA)
   - Support for multiple model sizes (tiny, base, small)
   - Recommended: **base.en** (best balance of speed/accuracy)
   - VAD-driven pause detection for natural speech boundaries

- **asr_vosk.c/h**: Vosk ASR implementation (optional, legacy)
   - Uses Vosk API with Kaldi backend
   - GPU-accelerated when available (via `vosk_gpu_init()`)
   - Smaller memory footprint than Whisper
   - Compiled only when `ENABLE_VOSK=ON`

- **vad_silero.c/h**: Voice Activity Detection
   - Uses Silero VAD ONNX model
   - Real-time speech/silence classification
   - Drives pause detection for chunking
   - Configurable sensitivity threshold

- **chunking_manager.c/h**: Long utterance handling
   - Manages multi-chunk speech sequences
   - VAD-driven pause detection
   - Assembles partial results into complete transcriptions
   - Prevents premature cutoff of long commands

#### Data Flow

```
Audio Input → VAD (Silero) → Chunking Manager → ASR Engine (Whisper/Vosk) → Transcript
                   ↓                                      ↑
              Speech/Silence                         GPU Acceleration
              Classification                         (Jetson + Vosk support)
```

#### Performance

| Model         | Platform   | RTF   | Speedup | Accuracy  |
| ------------- | ---------- | ----- | ------- | --------- |
| Whisper tiny  | Jetson GPU | 0.079 | 12.7x   | Good      |
| Whisper base  | Jetson GPU | 0.109 | 9.2x    | Excellent |
| Whisper small | Jetson GPU | 0.225 | 4.4x    | Best      |
| Vosk 0.22     | CPU/GPU    | ~0.15 | 6.7x    | Good      |

**RTF = Real-Time Factor** (lower is faster; 1.0 = realtime, <1.0 = faster than realtime)

### 3. LLM Subsystem (`src/llm/`, `include/llm/`)

**Purpose**: Large Language Model integration with streaming support

#### Architecture Pattern: **Strategy + Observer Patterns**

- **Strategy**: Multiple LLM providers (OpenAI, Claude, Gemini, local) via unified interface
- **Observer**: Streaming responses notify sentence buffer for real-time TTS

#### Key Components

- **llm_interface.c/h**: LLM abstraction layer
   - `LLMContext` struct: Provider-agnostic context
   - `llm_init()`: Initialize selected provider
   - `llm_send_message()`: Send message, get complete response (blocking)
   - `llm_send_message_streaming()`: Send message, stream response chunks
   - Provider selection based on configuration (`OPENAI_MODEL`, `ANTHROPIC_MODEL`)

- **llm_openai.c/h**: OpenAI API implementation
   - Supports GPT-5 series, GPT-4o, GPT-4
   - Supports llama.cpp local server (OpenAI-compatible endpoint)
   - Supports Ollama with runtime model switching
   - Supports Google Gemini (via OpenAI-compatible endpoint)
   - Both blocking and streaming modes
   - Conversation history management
   - Extended thinking support (reasoning_effort for OpenAI/Gemini models)

- **llm_claude.c/h**: Claude API implementation
   - Supports Claude 4.6 Opus/Sonnet, Claude 4.5 Sonnet
   - Streaming support
   - Different API format than OpenAI (Messages API)
   - Extended thinking support with configurable token budget
   - Full thinking content visibility (unlike OpenAI/Gemini)

- **llm_streaming.c/h**: Streaming response handler
   - Manages Server-Sent Events (SSE) connections
   - Buffers and parses incoming chunks
   - Notifies sentence buffer for TTS integration

- **sse_parser.c/h**: Server-Sent Events parser
   - Parses SSE format: `data: {...}\n\n`
   - Extracts JSON content from events
   - Handles partial events across network chunks

- **sentence_buffer.c/h**: Sentence boundary detection
   - Buffers streaming text until complete sentence
   - Detects sentence boundaries (`.`, `!`, `?`)
   - Sends complete sentences to TTS for natural phrasing
   - Reduces perceived latency (speak while generating)

- **llm_command_parser.c/h**: JSON command extraction
   - Extracts `<command>` JSON tags from LLM responses
   - Validates JSON structure
   - Handles malformed JSON gracefully

- **llm_rate_limit.c/h**: Cloud API rate limiter
   - Process-wide sliding window throttle (default 40 RPM, configurable)
   - Gates all cloud LLM call paths; local providers bypass
   - Interrupt-aware blocking (wakes on shutdown signal)

#### Data Flow (Streaming Mode)

```
User Query → LLM Provider (OpenAI/Claude/Local)
                    ↓ (SSE stream)
            SSE Parser → Streaming Handler
                    ↓ (text chunks)
            Sentence Buffer → TTS (as sentences complete)
                    ↓ (complete response)
            Command Parser → MQTT Commands
```

#### Performance Comparison

| Provider                | Quality | TTFT      | Latency | Cost          |
| ----------------------- | ------- | --------- | ------- | ------------- |
| OpenAI GPT-5            | 100%    | ~300ms    | ~3.1s   | ~$0.01/query  |
| Claude 4.6 Sonnet       | 92.4%   | ~400ms    | ~3.5s   | ~$0.015/query |
| Gemini 2.5 Flash        | ~90%    | ~250ms    | ~2.5s   | ~$0.002/query |
| llama.cpp (Qwen3-4B Q4) | 81.9%   | 116-138ms | ~1.5s   | FREE          |
| Ollama (Qwen3-4B Q4)    | 81.9%   | ~150ms    | ~1.6s   | FREE          |

**TTFT = Time To First Token** (lower = faster perceived response)

### 4. TTS Subsystem (`src/tts/`, `include/tts/`)

**Purpose**: High-quality text-to-speech synthesis

#### Key Components

- **text_to_speech.cpp/h**: TTS engine wrapper
   - Thread-safe interface with mutex protection (`tts_mutex`)
   - Converts text to WAV audio
   - Text preprocessing for natural phrasing (em-dash conversion, etc.)
   - Supports streaming integration with LLM sentence buffer

- **piper.cpp**: Piper TTS integration
   - Uses Piper library with ONNX Runtime
   - Phoneme-based synthesis
   - Multiple voice models supported
   - Recommended: `en_GB-alba-medium` (good quality, reasonable speed)

#### Data Flow

```
Text → Preprocessing → Piper Phonemization → ONNX Inference → WAV Audio → ALSA/Pulse
```

#### Thread Safety

TTS is protected by a global mutex (`tts_mutex`) to prevent concurrent access from:

- Main loop (local audio)
- Network server thread (remote audio)
- Streaming LLM sentence buffer

### 5. DAP2 Satellite Subsystem (`dawn_satellite/`, `common/`, `src/webui/webui_satellite.c`)

**Purpose**: WebSocket protocol for satellite devices (Raspberry Pi Tier 1, ESP32 Tier 2)

**References**: [WEBSOCKET_PROTOCOL.md](docs/WEBSOCKET_PROTOCOL.md) | [DAP2_SATELLITE.md](docs/DAP2_SATELLITE.md)

#### Architecture: **Local ASR/TTS + Remote LLM** (Tier 1)

DAP2 Tier 1 satellites handle speech recognition and text-to-speech locally and send only text to the daemon over WebSocket. Tier 2 satellites stream raw audio to the daemon for server-side processing.

```
┌─────────────────────────────────────────────────────────────┐
│                    DAWN Satellite (Tier 1)                    │
│                                                              │
│  Audio Capture → VAD (Silero) → Wake Word → ASR (Vosk)      │
│                                                   │ text     │
│  Audio Playback ← TTS (Piper) ← sentence_buffer ←│          │
│                                                              │
│  ┌────────────────────────────────────────────────────────┐  │
│  │  SDL2 Touchscreen UI (Optional, KMSDRM backend)       │  │
│  │  Orb Visualizer | Transcript | Music Panel | Settings  │  │
│  └────────────────────────────────────────────────────────┘  │
│                                                              │
│  ┌────────────────────────────────────────────────────────┐  │
│  │  Music Stream (separate WebSocket)                     │  │
│  │  Opus decode → ALSA playback → Goertzel FFT visualizer │  │
│  └────────────────────────────────────────────────────────┘  │
└──────────────────────────┬───────────────────────────────────┘
                           │ WebSocket JSON (control)
                           │ WebSocket binary (Opus music audio)
                           ▼
┌─────────────────────────────────────────────────────────────┐
│                       DAWN Daemon                            │
│  webui_satellite.c: register, query, streaming response      │
│  webui_music.c: music control, unified library browse, Opus stream   │
└─────────────────────────────────────────────────────────────┘
```

#### Key Components

**Satellite side** (`dawn_satellite/`):

- **ws_client.c/h**: WebSocket client for daemon communication
   - JSON message protocol over existing WebUI port
   - Reconnection with exponential backoff
   - App-level ping/pong keep-alive (10s interval)
   - Music subscribe/control/library/queue messages

- **voice_processing.c/h**: Local voice pipeline
   - VAD → wake word → ASR → send text query
   - Receive streaming response → sentence buffer → TTS
   - Producer-consumer TTS playback queue (synthesize N+1 while playing N)

- **music_stream.c/h**: Dedicated Opus audio WebSocket
   - Separate connection for music audio streaming
   - Opus decode → ALSA playback
   - Goertzel FFT analysis on live audio for visualizer

- **sdl_ui.c/h**: Touchscreen UI coordinator (SDL2 + KMSDRM)
   - Animated orb visualizer with state-driven colors
   - Scrollable markdown transcript with word-wrap
   - Touch gesture system (swipe, tap, long-press)
   - Status bar icon pattern for feature access (see below)

- **ui_music.c/h**: Music control panel
   - Three-tab layout: Playing / Queue / Library
   - Transport controls, seek bar, FFT visualizer
   - Paginated library browsing with artist/album drill-down

- **backlight.c/h**: Display brightness control
   - sysfs backlight control for DSI displays
   - Software dimming fallback for HDMI displays

**Daemon side** (`src/webui/`, `src/auth/`):

- **webui_satellite.c**: Satellite message handlers
   - Registration with DB lookup for persistent user mapping
   - Query routing, streaming response relay
   - Session management with UUID-based identification
   - HA area + room context injection into LLM system prompt
   - Satellite auth whitelist for music WebSocket messages

- **webui_admin_satellite.c**: Admin satellite management (CRUD)
   - List/update/delete satellite-to-user mappings (admin-only)
   - HA area assignment (dropdown from entity cache when HA enabled)
   - Force-disconnect on config change (satellite picks up new config on reconnect)

- **auth_db_satellite.c**: Satellite mapping persistence (`satellite_mappings` table)
   - Upsert, get, delete, update user/location, list with callback
   - Auto-registration on first connect (user_id=NULL, populated via admin panel)

**Shared** (`common/`):

- **common/src/asr/**: ASR engine abstraction (Whisper batch, Vosk streaming)
- **common/src/tts/**: Piper TTS with preprocessing and emoji stripping
- **common/src/vad/**: Silero VAD with ONNX runtime
- **common/src/logging/**: `DAWN_LOG_INFO/ERROR/WARNING` macros

#### DAP2 Protocol Messages

| Type                     | Direction          | Purpose                                  |
| ------------------------ | ------------------ | ---------------------------------------- |
| `satellite_register`     | Satellite → Daemon | Registration with UUID, name, location   |
| `satellite_register_ack` | Daemon → Satellite | Session ID, memory enabled flag          |
| `satellite_query`        | Satellite → Daemon | User's transcribed text                  |
| `stream_start`           | Daemon → Satellite | Streaming response begins                |
| `stream_delta`           | Daemon → Satellite | Partial response text                    |
| `stream_end`             | Daemon → Satellite | Response complete                        |
| `satellite_ping`         | Satellite → Daemon | App-level keep-alive                     |
| `list_satellites`        | Admin → Daemon     | Request all satellite mappings + status  |
| `update_satellite`       | Admin → Daemon     | Update user assignment or HA area        |
| `delete_satellite`       | Admin → Daemon     | Remove satellite mapping from DB         |
| `music_control`          | Satellite → Daemon | Play/pause/stop/next/prev/seek           |
| `music_library`          | Satellite → Daemon | Browse artists/albums/tracks (paginated) |
| `music_state`            | Daemon → Satellite | Playback state update                    |

#### Data Flow (Satellite Voice Command)

```
1. Satellite: Microphone → VAD → Wake Word Detection → ASR (Vosk streaming)
   ↓
2. satellite_query {text: "turn on the lights"} → WebSocket → Daemon
   ↓
3. Daemon: LLM processing → Tool execution → Streaming response
   ↓
4. stream_delta {delta: "I'll turn on"} → WebSocket → Satellite
   ↓
5. Satellite: Sentence buffer → TTS (Piper) → ALSA playback
   ↓
6. stream_end → Satellite marks response complete
```

#### Satellite UI Design Patterns

**Status Bar Icon Pattern**: Features are accessed via small icons in the
transcript status bar (top-right area), not via a dedicated quick actions panel.
This mirrors the WebUI's icon-bar approach.

- Icons sit inline in the existing status bar alongside WiFi and date/time
- Each icon is an 18x18 SDL primitive texture with a 48x48 hit area (Apple HIG
  minimum for touch)
- Default color: secondary text color; active state: cyan accent
- Rendering technique: build icon as a **white texture**, then tint at draw time
  via `SDL_SetTextureColorMod()` — one cached texture serves both idle and active
  color states without re-rendering
- Icons only appear when the feature is available (e.g., music icon requires
  Opus support). No placeholder or "coming soon" icons.
- Tapping an icon toggles its associated slide-in panel (e.g., music panel
  slides in from the right)

**Current icons**: Music (note glyph, toggles music panel)

**Panel system**: Two panel types remain:

- Settings panel: swipe down from top edge (hamburger indicator)
- Music panel: tap music icon in status bar (slides from right)

Swipe-up from the bottom edge is currently unassigned (reserved for future use).

### 6. Audio Subsystem (`src/audio/`, `include/audio/`)

**Purpose**: Audio capture, buffering, and playback

#### Key Components

- **audio_capture_thread.c/h**: Dedicated audio capture thread
   - Runs in separate thread to avoid blocking main loop
   - Continuous capture from ALSA/PulseAudio device
   - Writes to ring buffer for main loop consumption
   - Handles capture errors gracefully

- **ring_buffer.c/h**: Thread-safe circular buffer
   - Lock-free or mutex-protected (implementation dependent)
   - Fixed-size buffer for audio samples
   - Overwrite policy when buffer full
   - Used for smooth audio streaming between threads

- **flac_playback.c/h**: Music/audio file playback
   - Multi-format decoding (FLAC, MP3, Ogg Vorbis) via unified audio_decoder API
   - ALSA/PulseAudio output with automatic sample rate conversion
   - Used for notification sounds, music playback

- **music_db.c/h**: Unified music metadata database
   - SQLite-based cache for artist/title/album/genre tags with source tracking
   - Multi-source support (local files + Plex) via `music_source_t` enum
   - Priority-based deduplication (local wins over Plex for same artist+album+title)
   - COLLATE NOCASE matching on dedup index for cross-source consistency
   - Indexed search across metadata fields (including genre) with LIKE escaping
   - Incremental scanning with source-scoped stale deletion
   - Automatic cleanup of deleted files (per-source)

- **music_source.c/h**: Music source abstraction layer
   - `music_source_provider_t` callback interface for pluggable sources
   - Source name/prefix helpers (`music_source_name()`, `music_source_path_prefix()`)
   - Path-to-source detection (`music_source_from_path()`)

- **plex_db.c/h**: Plex-to-unified-DB sync layer
   - Fetches all Plex tracks via `plex_client_list_all_tracks()`
   - Inserts into unified `music_metadata` table with `source = MUSIC_SOURCE_PLEX`
   - Runs during music scanner cycle when Plex is configured

- **music_scanner.c/h**: Background music library scanner
   - Dedicated thread for non-blocking scans
   - Configurable scan interval (default: 60 minutes)
   - Scans local files and syncs Plex library in each cycle
   - Manual rescan trigger via admin socket
   - Mutex/condvar synchronization for thread safety

- **plex_client.c/h**: Plex Media Server REST API client
   - Authentication via X-Plex-Token header
   - `plex_client_list_all_tracks()` for bulk sync into unified DB
   - Stream URL construction for download-to-temp playback
   - Scrobble reporting on track completion
   - Server discovery and connection testing

- **http_download.c/h**: HTTP download-to-temp utility
   - libcurl-based download with 300s hard timeout
   - Used by Plex client to fetch tracks for audio_decoder

- **mic_passthrough.c/h**: Microphone passthrough
   - Direct microphone → speaker routing
   - Used for testing, debugging
   - Useful for verifying audio capture/playback setup

#### Threading Model

```
┌──────────────────┐
│ Capture Thread   │ (continuous capture)
│                  │
│ ALSA/Pulse → RB  │ (write to ring buffer)
└────────┬─────────┘
         │
    Ring Buffer (thread-safe)
         │
┌────────▼─────────┐
│   Main Thread    │ (state machine)
│                  │
│   RB → VAD → ASR │ (read from ring buffer)
└──────────────────┘
```

**RB = Ring Buffer**

### 7. WebUI Audio Subsystem

**Purpose**: Bidirectional audio streaming for WebUI browsers and Tier 2 (ESP32) satellites

This subsystem handles all server-side audio: decode incoming audio, run ASR, generate TTS, encode outgoing audio. The WebUI browser connects with **Opus** (48kHz); Tier 2 satellites connect with **raw PCM** (16kHz, skipping codec/resample). Both use the same binary WebSocket message types (0x01/0x02 audio in, 0x11/0x12 audio out) and the same worker thread pipeline in `webui_audio.c`. See [WEBSOCKET_PROTOCOL.md](docs/WEBSOCKET_PROTOCOL.md) for the binary message type reference.

#### Architecture: **Opus Codec + WebCodecs API** (WebUI Browser Path)

The WebUI uses Opus audio compression for efficient bidirectional audio streaming between browser and server.

```
┌───────────────────────────────────────────────────────────────────────┐
│                        Browser (WebUI)                                 │
│                                                                        │
│  ┌───────────────┐    ┌───────────────┐    ┌───────────────┐          │
│  │ getUserMedia  │───>│ Opus Encoder  │───>│  WebSocket    │──────────┼──>
│  │ (48kHz input) │    │ (Web Worker)  │    │ Binary Send   │          │
│  └───────────────┘    └───────────────┘    └───────────────┘          │
│                                                                        │
│  ┌───────────────┐    ┌───────────────┐    ┌───────────────┐          │
│  │ AudioContext  │<───│ Opus Decoder  │<───│  WebSocket    │<─────────┼──
│  │ (48kHz output)│    │ (Web Worker)  │    │ Binary Recv   │          │
│  └───────────────┘    └───────────────┘    └───────────────┘          │
└───────────────────────────────────────────────────────────────────────┘
                                │
                                │ Opus frames (length-prefixed)
                                │
                                ▼
┌───────────────────────────────────────────────────────────────────────┐
│                        DAWN Server                                     │
│                                                                        │
│  ┌───────────────┐    ┌───────────────┐    ┌───────────────┐          │
│  │ Opus Decoder  │───>│  Resampler    │───>│     ASR       │          │
│  │ (libopus)     │    │ 48kHz → 16kHz │    │   (Whisper)   │          │
│  └───────────────┘    └───────────────┘    └───────────────┘          │
│                                                                        │
│  ┌───────────────┐    ┌───────────────┐    ┌───────────────┐          │
│  │ Opus Encoder  │<───│  Resampler    │<───│     TTS       │          │
│  │ (libopus)     │    │ 22kHz → 48kHz │    │   (Piper)     │          │
│  └───────────────┘    └───────────────┘    └───────────────┘          │
└───────────────────────────────────────────────────────────────────────┘
```

#### Key Components

- **opus-worker.js**: Web Worker for encoding/decoding using WebCodecs API
   - Encodes browser microphone input (48kHz) to Opus frames
   - Decodes server TTS audio (Opus frames) to PCM for playback
   - Falls back to raw PCM if WebCodecs unavailable

- **Codec Configuration**:
   - Sample rate: 48kHz (Opus native rate)
   - Channels: Mono
   - Bitrate: Adaptive (typically 24-32 kbps for voice)
   - Frame size: 20ms (960 samples at 48kHz)

- **Capability Negotiation**:
   - Browser sends `audio_codecs: ["opus", "pcm"]` during WebSocket connect
   - Server selects best available codec
   - Graceful fallback to uncompressed PCM if Opus unavailable

#### Data Flow (Browser Voice Input)

```
1. getUserMedia() captures audio at 48kHz
   ↓
2. AudioWorklet sends PCM frames to Opus worker
   ↓
3. Worker encodes to Opus frames (length-prefixed)
   ↓
4. WebSocket sends binary data to server
   ↓
5. Server decodes Opus → resamples to 16kHz → ASR
   ↓
6. LLM processing → TTS generation
   ↓
7. Server encodes TTS audio to Opus → sends to browser
   ↓
8. Worker decodes Opus → AudioContext plays at 48kHz
```

#### Benefits of Opus Streaming

| Metric               | Raw PCM (16-bit) | Opus Compressed                     |
| -------------------- | ---------------- | ----------------------------------- |
| Bandwidth (1s audio) | ~192 KB          | ~3-4 KB                             |
| Latency              | Minimal          | +2-5ms encoding                     |
| Quality              | Lossless         | Near-lossless (voice optimized)     |
| Browser Support      | Universal        | WebCodecs (Chrome/Edge/Firefox 90+) |

### 7a. Always-On Voice Mode (`src/webui/webui_always_on.c`, `www/js/audio/always-on.js`)

**Purpose**: Continuous wake word listening via WebUI browser — local client parity

The always-on subsystem enables hands-free voice interaction from the browser, matching the local microphone's wake word detection without any browser-side AI models. The browser streams audio continuously; all VAD and wake word detection runs server-side.

#### State Machine

```
DISABLED → LISTENING → WAKE_CHECK → WAKE_PENDING → PROCESSING → LISTENING
                  ↑        ↓                              ↓
                  ↑   RECORDING → PROCESSING ─────────────┘
                  ↑        (wake word only, no inline command)
                  └────────────────────────────────────────┘
```

- **LISTENING**: VAD monitors for speech onset (Silero VAD, 32ms chunks at 16kHz)
- **WAKE_CHECK**: Speech detected — buffer audio, wait for 1.5s silence, then dispatch ASR
- **WAKE_PENDING**: ASR running on worker thread (async)
- **RECORDING**: Wake word confirmed but no inline command — record follow-up command
- **PROCESSING**: Command sent to LLM; audio muted to prevent TTS echo

#### Key Design Decisions

- **Server-side VAD + wake word**: No production voice assistant runs wake word detection in the browser. Server-side matches the proven local session architecture.
- **Shared `wake_word.c`**: Same wake word matching logic used by local mic (`dawn.c`) and satellites.
- **Audio chunk cadence**: Browser sends audio in configurable chunks (default 100ms via `audio_chunk_ms`). Smaller chunks reduce end-of-speech detection jitter.
- **TTS echo prevention**: Audio capture is muted during PROCESSING state and deferred until TTS playback completes.
- **Per-connection context**: Each WebSocket connection gets its own VAD, Opus decoder, resampler, and circular buffer. No shared state between clients.

#### Unified Action Button (Browser UI)

The WebUI uses a single split button with a dropdown for input mode selection:

| Mode | Button Behavior | Events |
|------|----------------|--------|
| **Send Text** (default) | Click sends text | `click` → `handleSend()` |
| **Hold to Talk** | Hold to record, release to send | `mousedown`/`mouseup` → start/stop capture |
| **Continuous Listening** | Click toggles always-on | `click` → `toggle()` |

A single `resolveButtonState()` function determines the button label with priority: Cancel (red, during processing) > PTT recording > text override > mode-specific label. Smart typing override temporarily shows "Send" when text is present in any voice mode.

### 7b. Visual Rendering Tool (`src/tools/render_visual_tool.c`, `www/js/ui/visual-render.js`)

**Purpose**: Inline SVG/HTML diagrams generated by the LLM during conversation

The visual rendering tool enables the LLM to generate flowcharts, architecture diagrams, data charts, interactive widgets, UI mockups, and illustrations directly in the conversation. It uses the **two-step instruction loader pattern**: the LLM loads design guidelines from markdown files on disk before generating visuals, keeping the system prompt lightweight.

#### Architecture

```
LLM calls render_visual_load_guidelines(modules)
    ↓
instruction_loader.c reads _core.md + module.md from tool_instructions/
    ↓
Guidelines returned to LLM context (~10KB of design rules)
    ↓
LLM calls render_visual(title, type, code)
    ↓
render_visual_tool.c wraps code in <dawn-visual> tag
    ↓
webui_server.c sends as role:"visual" + stashes pending_visual on session
    ↓
dawn.js renders immediately into streaming entry (splits text around visual)
    ↓
streaming.js finalize saves pre-visual + <dawn-visual> + post-visual to DB
    ↓
On replay, transcript.js extractVisuals splits and renders inline
```

#### Key Components

- **instruction_loader.c/h**: Generic two-step loader — reads `_core.md` + comma-separated modules from `tool_instructions/{tool}/`. Pre-scan allocation, path traversal sanitization, 128KB cap. Reusable by any future two-step tool.
- **render_visual_tool.c**: Two tool registrations (`render_visual_load_guidelines` + `render_visual`). Title sanitization, `</dawn-visual>` tag breakout prevention, JSON parsing of `details` parameter.
- **visual-render.js**: Sandboxed iframe rendering (`sandbox="allow-scripts"`), theme CSS injection from computed parent variables, 9 color ramps with automatic light/dark mode, vendor script inlining (Chart.js cached and embedded), ResizeObserver height auto-sizing via debounced postMessage, `sendPrompt()` bridge with WeakSet source validation, download button.
- **Guideline modules** (`tool_instructions/render_visual/`): `_core.md` (universal design system), `diagram.md`, `chart.md`, `interactive.md`, `art.md`, `mockup.md`.
- **History persistence**: Server stashes `pending_visual` on session (protected by `tools_mutex`), client interleaves visual content between pre/post text in streaming save, replay splits at tag boundary for inline positioning.

### 8. Vision/Image Subsystem (`src/image_store.c`, `src/webui/webui_images.c`, `www/js/ui/vision.js`)

**Purpose**: Image upload, storage, vision AI integration, and image search for the WebUI

#### Architecture: **Client-Side Compression + Server-Side Filesystem Storage**

```
┌───────────────────────────────────────────────────────────────────────┐
│                        Browser (WebUI)                                 │
│                                                                        │
│  ┌─────────────────────────────────────────────────────────────────┐  │
│  │                    Input Methods                                 │  │
│  │  ┌──────────┐  ┌──────────┐  ┌──────────┐  ┌──────────┐        │  │
│  │  │  File    │  │  Paste   │  │  Drag &  │  │  Camera  │        │  │
│  │  │  Upload  │  │  (Ctrl+V)│  │  Drop    │  │  Capture │        │  │
│  │  └────┬─────┘  └────┬─────┘  └────┬─────┘  └────┬─────┘        │  │
│  │       └─────────────┴─────────────┴─────────────┘               │  │
│  │                           │                                      │  │
│  │                    ┌──────▼──────┐                               │  │
│  │                    │ Compression │ (max 1024px, JPEG 85%)        │  │
│  │                    └──────┬──────┘                               │  │
│  └───────────────────────────┼─────────────────────────────────────┘  │
│                              │                                        │
│  ┌───────────────────────────▼─────────────────────────────────────┐  │
│  │ POST /api/images (multipart/form-data)                          │  │
│  └───────────────────────────┬─────────────────────────────────────┘  │
└──────────────────────────────┼────────────────────────────────────────┘
                               │
                               ▼
┌───────────────────────────────────────────────────────────────────────┐
│                        DAWN Server                                     │
│                                                                        │
│  ┌───────────────────────────────────────────────────────────────┐    │
│  │ webui_images.c - HTTP Endpoint Handler                        │    │
│  │                                                               │    │
│  │  POST /api/images  → Validate → Store → Return image ID       │    │
│  │  GET /api/images/:id → get_path() → lws_serve_http_file()     │    │
│  └───────────────────────────┬───────────────────────────────────┘    │
│                              │                                        │
│  ┌───────────────────────────▼───────────────────────────────────┐    │
│  │ image_store.c - Filesystem Storage + SQLite Metadata          │    │
│  │                                                               │    │
│  │  Files: <data_dir>/images/<id>.<ext>                          │    │
│  │  Table: images (id, user_id, source, retention_policy, ...)   │    │
│  │  Sources: upload, generated, search, MMS, document            │    │
│  │  Retention: default (age-based), permanent, cache (LRU)       │    │
│  │  - Thread-safe: file I/O outside mutex, metadata ops inside   │    │
│  │  - Atomic writes (tmp + fsync + rename + O_NOFOLLOW)          │    │
│  │  - Zero-copy HTTP serving via kernel sendfile                 │    │
│  └───────────────────────────────────────────────────────────────┘    │
│                                                                        │
│  ┌───────────────────────────────────────────────────────────────┐    │
│  │ LLM Vision Integration                                        │    │
│  │                                                               │    │
│  │  llm_openai.c: data:image/jpeg;base64,... format              │    │
│  │  llm_claude.c: source.type="base64", source.media_type=...    │    │
│  └───────────────────────────────────────────────────────────────┘    │
└───────────────────────────────────────────────────────────────────────┘
```

#### Key Components

- **vision.js**: Client-side image handling (1,400+ lines)
   - Input methods: file picker, clipboard paste, drag-and-drop, camera capture
   - Camera API with front/rear switching (`navigator.mediaDevices.getUserMedia`)
   - Client-side compression via Canvas API (configurable max dimension, default 1024px, JPEG 85%)
   - Multi-image support (configurable max per message, default 5)
   - LocalStorage caching of uploaded images by ID
   - Security: SVG explicitly excluded to prevent XSS attacks
   - Accessibility: ARIA announcements, keyboard navigation

- **image_store.c/h**: Server-side image storage
   - Filesystem-backed: images stored as files in `<data_dir>/images/`, SQLite holds metadata only
   - Image ID format: `img_` + 12 alphanumeric characters (getrandom)
   - Source tracking: `IMAGE_SOURCE_UPLOAD`, `_GENERATED`, `_SEARCH`, `_MMS`, `_DOCUMENT`
   - Retention policies: `IMAGE_RETAIN_DEFAULT` (age-based), `_PERMANENT` (never delete), `_CACHE` (LRU at size cap)
   - Atomic file writes: O_NOFOLLOW tmp + fsync + rename, file I/O outside auth_db mutex
   - Source-aware access control: UPLOAD/MMS require ownership, SEARCH/GENERATED/DOCUMENT accessible to any auth'd user
   - Configurable limits: max size, max per user, retention days, cache size (MB)

- **webui_images.c/h**: HTTP endpoint handlers
   - `POST /api/images`: Upload image, returns `{id, mime_type, size}`
   - `GET /api/images/:id`: Zero-copy file serving via `lws_serve_http_file()` (kernel sendfile)
   - Authentication required, source-aware access check
   - Security headers: Cache-Control, X-Content-Type-Options, Content-Disposition

- **image_search_tool.c/h**: Web image search via SearXNG
   - Queries `web_search_query_images_raw()` (shared SearXNG backend)
   - Concurrent image fetching via `curl_multi` (4 parallel connections, 10s wall-clock cap)
   - SSRF protection: DNS pinning via CURLOPT_RESOLVE, manual redirect-with-revalidation (1 hop max)
   - Magic byte validation (JPEG, PNG, GIF87a/89a, WebP) — Content-Type ignored
   - Cached as `IMAGE_SOURCE_SEARCH` + `IMAGE_RETAIN_CACHE` (LRU eviction at configurable cap)

#### Data Flow (Image Upload)

```
1. User selects/pastes/drops/captures image
   ↓
2. Browser validates type (JPEG, PNG, GIF, WebP - NO SVG)
   ↓
3. Canvas API compresses to max 1024px, JPEG 85%
   ↓
4. POST /api/images with multipart form data
   ↓
5. Server validates auth, writes file atomically to <data_dir>/images/, stores metadata in SQLite
   ↓
6. Server returns image ID (e.g., "img_a1b2c3d4e5f6")
   ↓
7. Browser caches in localStorage, shows preview
   ↓
8. On send: Full base64 data sent to LLM with message
   ↓
9. History stores image ID reference (not inline data)
```

#### Vision Model Support

The system auto-detects vision capability based on model name:

| Model Pattern                           | Provider  | Vision Support       |
| --------------------------------------- | --------- | -------------------- |
| `gpt-4o`, `gpt-4-vision`, `gpt-4-turbo` | OpenAI    | Yes                  |
| `claude-3-*`                            | Anthropic | Yes                  |
| `gemini-*`                              | Google    | Yes                  |
| `llava-*`, `qwen-vl-*`, `cogvlm-*`      | Local     | Yes                  |
| Other models                            | Various   | No (button disabled) |

#### Security Measures

- **SVG Exclusion**: SVG files explicitly blocked to prevent XSS via embedded scripts
- **Data URI Validation**: Only `data:image/{jpeg,png,gif,webp};base64,` prefixes accepted
- **Base64 Character Validation**: Only `[A-Za-z0-9+/=]` allowed in base64 portion
- **Authentication Required**: All image endpoints require valid session
- **Per-User Limits**: Configurable maximum images per user

### 9. Document Upload Subsystem (`src/webui/webui_documents.c`, `www/js/ui/documents.js`)

**Purpose**: Document upload, extraction, and attachment for LLM context — supports plain text, PDF (MuPDF), DOCX (libzip+libxml2), and HTML-to-markdown (html_parser.c)

#### Architecture: **Client-Side Read + Server-Side Extraction + Transcript Chip Display**

```
┌───────────────────────────────────────────────────────────────────────┐
│                        Browser (WebUI)                                 │
│                                                                        │
│  ┌─────────────────────────────────────────────────────────────────┐  │
│  │                    Input Methods                                 │  │
│  │  ┌──────────┐  ┌──────────┐  ┌──────────┐                       │  │
│  │  │  File    │  │  Drag &  │  │  Paste   │                       │  │
│  │  │  Button  │  │  Drop    │  │  (Ctrl+V)│                       │  │
│  │  └────┬─────┘  └────┬─────┘  └────┬─────┘                       │  │
│  │       └─────────────┴─────────────┘                              │  │
│  │                     │                                             │  │
│  │              ┌──────▼──────┐                                      │  │
│  │              │ Client-side │ (FileReader API, UTF-8 text)         │  │
│  │              │ text read   │                                      │  │
│  │              └──────┬──────┘                                      │  │
│  │                     │                                             │  │
│  │              ┌──────▼──────┐                                      │  │
│  │              │ Input chips │ (filename, size, remove button)      │  │
│  │              └──────┬──────┘                                      │  │
│  └─────────────────────┼─────────────────────────────────────────┘  │
│                        │ on send                                     │
│  ┌─────────────────────▼─────────────────────────────────────────┐  │
│  │ WebSocket: [ATTACHED DOCUMENT: file (N bytes)]...text...       │  │
│  │            [END DOCUMENT]                                      │  │
│  └─────────────────────┬─────────────────────────────────────────┘  │
│                        │                                             │
│  ┌─────────────────────▼─────────────────────────────────────────┐  │
│  │ Transcript: parseDocumentMarkers() strips markers → chips     │  │
│  │  ┌──────────────┐                                             │  │
│  │  │ [TXT] file ✕ │  ← clickable, opens viewer modal           │  │
│  │  └──────────────┘                                             │  │
│  └───────────────────────────────────────────────────────────────┘  │
└───────────────────────────────────────────────────────────────────────┘
                         │
                         ▼
┌───────────────────────────────────────────────────────────────────────┐
│                        DAWN Server                                     │
│                                                                        │
│  ┌───────────────────────────────────────────────────────────────┐    │
│  │ webui_documents.c - HTTP Endpoint Handler                     │    │
│  │                                                               │    │
│  │  POST /api/documents  → Validate ext → Extract → Return      │    │
│  │  POST /api/documents/summarize  → TF-IDF summarize           │    │
│  │  Supported: .txt, .md, .csv, .json, .xml, .yaml, .toml,     │    │
│  │             .c, .h, .py, .js, .html, .css, .sh, .log, etc.  │    │
│  │             .pdf (MuPDF), .docx (libzip+libxml2)             │    │
│  │  Configurable limits (default: 512 KB, up to 5 files)        │    │
│  └───────────────────────────────────────────────────────────────┘    │
│                                                                        │
│  Document text injected into LLM message as:                          │
│  [ATTACHED DOCUMENT: filename (N bytes)]\n...content...\n             │
│  [END DOCUMENT]                                                       │
│  Markers preserved in DB and LLM context; stripped client-side only   │
└───────────────────────────────────────────────────────────────────────┘
```

#### Key Components

- **documents.js**: Client-side document handling
   - Input methods: file picker button, drag-and-drop, cooperative with vision.js (images vs documents)
   - Client-side text extraction via FileReader API (no server round-trip for text files)
   - Input chip UI: filename, extension badge, size, remove button
   - `parseDocumentMarkers(text)`: Regex extraction of document markers from transcript text
   - `openDocumentViewer(filename, content)`: Modal viewer with focus trap, Escape/backdrop close
   - Allowed extensions: `.txt`, `.md`, `.csv`, `.json`, `.xml`, `.yaml`, `.yml`, `.toml`, `.ini`, `.cfg`, `.conf`, `.log`, `.c`, `.h`, `.cpp`, `.py`, `.js`, `.ts`, `.html`, `.css`, `.sh`, `.sql`, `.env`, `.pdf`, `.docx`
   - PDF and DOCX sent to server for extraction (no client-side read); text files read client-side
   - Toast feedback for unsupported file types on drop

- **webui_documents.c/h**: Server-side upload handling
   - `POST /api/documents`: Multipart form upload, extension validation, text extraction
      - Plain text/source files: UTF-8 read directly
      - PDF: MuPDF extraction (`fz_try`/`fz_catch` for error safety, page count cap)
      - DOCX: libzip + libxml2 parse of `word/document.xml` (XXE prevention, ZIP bomb limits)
      - HTML: routed through `html_parser.c` to markdown
   - `POST /api/documents/summarize`: TF-IDF auto-summarize for documents > 8,000 chars
   - Returns `{filename, content, size, estimated_tokens}` JSON response; large docs include `auto_summary`
   - Authentication required (uses session validation)

- **transcript.js** (document integration):
   - Document markers stripped BEFORE routing logic in `addTranscriptEntry()` to prevent misrouting when document content contains `<command>` tags
   - `createDocumentChips()`: Renders clickable chips in conversation entries
   - History replay: `prependTranscriptEntry()` also parses markers for saved conversations

- **documents.css**: Styling for input chips, transcript chips, and viewer modal
   - Transcript chips: squarer (6px radius) document-like appearance
   - Viewer modal: 700px panel, monospace `<pre>`, mobile full-screen at 480px

#### Security Measures

- **textContent only**: Document content rendered via `textContent`, never `innerHTML`
- **DOMPurify**: Markdown-rendered message text sanitized before DOM insertion
- **CSP header**: `script-src 'self'` blocks any inline scripts in document content
- **Extension allowlist**: Only known text extensions accepted; binary files rejected
- **Size limit**: Configurable per file (default 512 KB, max 10 MB) enforced at both client and server
- **PDF safety**: `fz_try`/`fz_catch` wraps all MuPDF calls; page count capped to prevent DoS on large documents
- **DOCX safety**: XXE prevention (no external entity resolution in libxml2); ZIP bomb protection via entry size limits
- **Magic bytes validation**: PDF (`%PDF-`) and DOCX (ZIP `PK\x03\x04`) headers verified before extraction

### 10. Memory Subsystem (`src/memory/`, `include/memory/`)

**Purpose**: Persistent memory system for user facts, preferences, conversation summaries, entity graph, and semantic embeddings

#### Architecture: **Sleep Consolidation Model + Entity Graph**

Memory extraction happens at session end, not during conversation. This adds zero latency to conversations while building a persistent user profile. The entity graph captures people, places, pets, projects, and their relationships.

```
┌───────────────────────────────────────────────────────────────────────┐
│                    DURING CONVERSATION                                 │
├───────────────────────────────────────────────────────────────────────┤
│  • Full conversation in LLM context window                            │
│  • Core facts + preferences + entity graph pre-loaded at session start│
│  • Memory tool available for explicit remember/search/forget          │
│  • Hybrid search: keyword + semantic similarity via embeddings        │
│  • Zero extraction overhead                                           │
└───────────────────────────────────────────────────────────────────────┘
                                │
                                │ Session ends (WebSocket disconnect/timeout)
                                ▼
┌───────────────────────────────────────────────────────────────────────┐
│                    SESSION END EXTRACTION                              │
├───────────────────────────────────────────────────────────────────────┤
│  • Load conversation messages from database                           │
│  • Build extraction prompt with transcript + existing profile         │
│  • Existing entities fed into prompt to prevent duplicates            │
│  • Call extraction LLM (can differ from conversation model)           │
│  • Parse JSON: facts, preferences, corrections, summary,             │
│    entities, and relations                                            │
│  • Store in SQLite (skip if conversation marked private)              │
│  • Generate embeddings for new facts and entities                     │
│  • Runs in background thread (non-blocking)                           │
└───────────────────────────────────────────────────────────────────────┘
```

#### Key Components

- **memory_types.h**: Data structures
   - `memory_fact_t`, `memory_preference_t`, `memory_summary_t`
   - `memory_entity_t` (name, type, canonical_name, mention_count, first/last_seen)
   - `memory_relation_t` (subject→relation→object with optional literal values)

- **memory_db.c/h**: SQLite CRUD operations
   - Prepared statements for all memory tables (facts, preferences, summaries, entities, relations)
   - Entity upsert with `RETURNING id` (SQLite 3.37.2+)
   - Relation creation with entity FK or literal value
   - Entity search by keyword (LIKE) and by ID
   - Bulk relation loading (`memory_db_relation_list_all_by_user()`)
   - Entity listing for extraction prompt dedup
   - Similarity detection for duplicate prevention
   - Access counting with time-gated confidence reinforcement
   - Atomic decay via custom SQLite `powf()` function (no row iteration)
   - Combined stats query (facts + preferences + summaries + entities in one SELECT)

- **memory_embeddings.c/h**: Semantic embedding system
   - Calls shared `embedding_engine` (see Section 11) for embed/cosine operations
   - Multi-provider support: Ollama, OpenAI, ONNX (configurable in `[memory.embeddings]`)
   - In-memory cache with mutex protection (facts: 1000 cap, entities: 500 cap)
   - Lazy cache loading on first search, invalidated after extraction
   - Cosine similarity search against cached embeddings
   - Hybrid search combining keyword and semantic results with configurable weights
   - Provider implementations: `memory_embed_ollama.c`, `memory_embed_openai.c`, `memory_embed_onnx.c`

- **memory_context.c/h**: Session start context builder
   - `memory_build_context()` builds ~800 token block
   - Loads preferences, top facts by confidence, recent summaries
   - Injected into LLM system prompt

- **memory_extraction.c/h**: Session end extraction
   - Triggered via `memory_trigger_extraction()`
   - Spawns background thread for non-blocking extraction
   - Parses LLM JSON response: facts, preferences, summaries, **entities, and relations**
   - Entity upsert with canonical name normalization
   - Embedding generation for newly created entities (skipped for existing)
   - Existing entity list fed into extraction prompt to prevent duplicate names
   - Respects conversation privacy flag

- **memory_callback.c**: Tool handler for `MEMORY` device type
   - `search`: Hybrid keyword + semantic search across all memory tables
   - `recent`: Time-based retrieval (e.g., "24h", "7d", "1w")
   - `remember`: Immediate fact storage with guardrails
   - `forget`: Delete matching facts
   - `merge_entities`: Combine duplicate entities (transfers relations, contacts, deduplicates)
   - `save_contact`, `find_contact`, `list_contacts`, `delete_contact`: Contact management
   - `append_graph_context()`: Entity graph results appended to search output

- **contacts_db.c/h**: Contacts database operations
   - Structured contact info (email, phone, address) linked to `memory_entities` via `entity_id`
   - CRUD: `contacts_add()`, `contacts_find()`, `contacts_update()`, `contacts_delete()`, `contacts_list()`
   - Case-insensitive search with LIKE escape

- **memory_db_entity_merge()**: Transactional entity merge
   - MERGE_EXEC macro for error-checked SQL within a transaction
   - Reassigns relations (both subject and object FKs) and contacts to target entity
   - Deletes self-referential relations created by reassignment
   - Deduplicates via ROW_NUMBER() window function
   - Absorbs mention count and time range from source entity

- **memory_maintenance.c/h**: Nightly decay orchestration
   - Called from auth maintenance thread (15-minute cycle)
   - Hour-gated with 20-hour double-execution guard
   - Per-user: decay facts → decay preferences → prune low-confidence → prune superseded → prune old summaries
   - Configurable rates, floors, and thresholds via `[memory.decay]`

#### Database Schema

Five tables in the auth database (`/var/lib/dawn/auth.db`):

```sql
-- Facts: discrete pieces of information
memory_facts (id, user_id, fact_text, confidence, source, created_at,
              last_accessed, access_count, superseded_by, embedding, embedding_norm)

-- Preferences: communication style preferences
memory_preferences (id, user_id, category, value, confidence, source,
                    created_at, updated_at, reinforcement_count)

-- Summaries: conversation digests
memory_summaries (id, user_id, session_id, summary, topics, sentiment,
                  created_at, message_count, duration_seconds, consolidated)

-- Entities: people, places, pets, projects, etc.
memory_entities (id, user_id, name, entity_type, canonical_name, mention_count,
                 first_seen, last_seen, embedding, embedding_norm)
   UNIQUE(user_id, canonical_name)

-- Relations: entity-to-entity or entity-to-literal relationships
memory_relations (id, user_id, subject_entity_id, relation, object_entity_id,
                  object_value, fact_id, confidence, created_at)
   FK subject_entity_id → memory_entities(id)
   FK object_entity_id → memory_entities(id) (nullable, literal if NULL)

-- Contacts: structured contact info linked to entities
contacts (id, user_id, entity_id, field_type, value, label, created_at)
   FK entity_id → memory_entities(id)
   field_type: "email", "phone", "address"
   label: "work", "personal", "mobile", "home", "other", NULL
```

#### Privacy Toggle

Users can mark conversations as private to skip memory extraction:

- `is_private` column in `conversations` table
- Set via WebSocket message or Ctrl+Shift+P keyboard shortcut
- Can be set before conversation starts (pending state)
- Visual badge in conversation history list

#### Security Guardrails

Memory content flows into future prompts, creating potential attack vectors:

```c
// Blocked patterns (hardcoded in memory_callback.c)
const char *MEMORY_BLOCKED_PATTERNS[] = {
   "whenever", "always", "you should", "you must",
   "ignore", "forget", "disregard", "pretend",
   "act as if", "system prompt", "instructions",
   "password", "api key", "token", "secret",
   NULL
};
```

#### Configuration

```toml
[memory]
enabled = true
context_budget_tokens = 800
session_timeout_minutes = 15

[memory.extraction]
provider = "local"        # "local", "openai", "claude", "ollama"
model = "qwen2.5:7b"      # Model for extraction

[memory.embeddings]
provider = "ollama"       # "ollama", "openai", "onnx"
model = "nomic-embed-text"  # Embedding model name
endpoint = "http://localhost:11434"  # Provider endpoint
dimensions = 768          # Embedding dimensions
keyword_weight = 0.4      # Hybrid search: keyword component weight (0.0-1.0)
semantic_weight = 0.6     # Hybrid search: semantic component weight (0.0-1.0)

[memory.decay]
enabled = true            # Enable nightly confidence decay
hour = 2                  # Run at 2 AM local time (0-23)
inferred_weekly = 0.95    # Inferred facts lose 5%/week
explicit_weekly = 0.98    # Explicit facts lose 2%/week
preference_weekly = 0.97  # Preferences lose 3%/week
inferred_floor = 0.0      # Inferred facts can decay to zero
explicit_floor = 0.50     # Explicit facts never below 50%
preference_floor = 0.40   # Preferences never below 40%
prune_threshold = 0.25    # Delete facts below this confidence
summary_retention_days = 30
access_reinforcement_boost = 0.05  # +5% on access (1-hour cooldown)
```

#### WebUI Memory Viewer

The memory viewer provides a browser-based interface for inspecting and managing all memory types:

- **Tabs**: Facts, Preferences, Summaries, Graph (entities), Contacts
- **Stats bar**: Real-time counts for each memory type including contacts
- **Search**: Filter memories by keyword across all tabs
- **Graph tab**: Entity cards with type badges, expandable relations (→ outgoing, ← incoming), contact count badge on person entities, two-click entity merge (select source → click target → confirm)
- **Contacts tab**: Contact cards with field_type/label badges, hover-reveal edit/delete, search, pagination. Add/edit modal with entity typeahead. Cross-linked from Graph tab person entities.
- **Delete**: Per-item delete with confirmation, bulk "Forget Everything"
- **Import / Export**: Transfer memories between DAWN instances or other AI assistants
- **Keyboard accessible**: tabindex, ARIA roles, Enter/Space activation
- **Endpoints**: `GET /api/memory/{facts,preferences,summaries,entities,stats}`, `DELETE /api/memory/{facts,preferences,summaries,entities}/:id`

#### Memory Import / Export

Users can export their memories for backup or transfer, and import memories from other AI assistants (Claude, ChatGPT) or from a previous DAWN export.

**Export formats:**
- **DAWN JSON** (`dawn_memory` format, version 1): Lossless export including facts, preferences, entities with relations, confidence scores, sources, and timestamps. Suitable for backup/restore between DAWN instances.
- **Human-readable text**: Markdown-formatted list of facts and preferences. Portable — can be pasted into any AI assistant.

**Import sources:**
- **DAWN JSON**: Direct restore from a previous export. Preserves metadata (confidence, source, timestamps).
- **Plain text**: One fact per line (bullets and markdown headers auto-stripped). Each line becomes a fact with `confidence=0.7`, `source="import"`. Supports paste or file upload.

**Deduplication**: Import uses a two-stage duplicate detection pipeline:
1. **Hash check**: `memory_normalize_and_hash()` for O(1) exact duplicate detection via FNV-1a hash
2. **Jaccard similarity**: Fuzzy matching (threshold 0.7) catches paraphrased duplicates

**Preview mode**: Import runs in preview-then-commit workflow. The first request (`commit=false`) returns a preview of what will be imported (new items, duplicates skipped). The user reviews and confirms before the second request (`commit=true`) writes to the database.

**WebSocket messages**: `export_memories` / `export_memories_response`, `import_memories` / `import_memories_response`

#### Data Flow (Memory Lifecycle)

```
1. Session Start (WebSocket connect)
   ↓
2. Load user profile: memory_build_context(user_id)
   ↓
3. Inject facts/preferences/entity names into LLM system prompt
   ↓
4. During conversation:
   - User: "Remember I'm vegetarian" → memory_remember() → immediate storage
   - User: "What do you know about me?" → hybrid_search() → keyword + semantic
   - Search also returns entity graph context (ENTITIES section)
   ↓
5. Session End (WebSocket disconnect/timeout)
   ↓
6. Check privacy flag: if private, skip extraction
   ↓
7. memory_trigger_extraction() → background thread
   ↓
8. Load conversation, build extraction prompt (includes existing entity names)
   ↓
9. Call extraction LLM, parse JSON response
   ↓
10. Store new facts, update preferences, save summary
   ↓
11. Upsert entities (canonical name dedup), create relations
   ↓
12. Generate embeddings for new facts and entities (provider-specific)
   ↓
13. Invalidate embedding caches (once, not per-item)

--- Nightly Maintenance (runs at configured hour) ---

14. memory_run_nightly_decay() called from auth maintenance thread
   ↓
15. For each user: apply confidence decay (atomic SQL with powf())
   ↓
16. Prune facts below threshold (audit logged), prune old summaries
   ↓
17. Accessed facts reinforced (+0.05, time-gated to 1-hour cooldown)
```

---

### 11. Document Search / RAG Subsystem (`src/tools/document_*.c`, `src/core/embedding_engine.c`)

**Purpose**: Retrieval-Augmented Generation — upload documents, chunk and embed them, then search or read them via LLM tools

#### Architecture: **Shared Embedding Engine + Per-Document Chunking + Dual Tool Interface**

```
┌───────────────────────────────────────────────────────────────────────┐
│                     DOCUMENT INGESTION                                 │
├───────────────────────────────────────────────────────────────────────┤
│  WebUI upload (drag-and-drop / file picker)                           │
│  → webui_doc_library.c receives file                                  │
│  → document_chunker.c: paragraph split → sentence split → overlap     │
│  → embedding_engine_embed() per chunk (ONNX/Ollama/OpenAI)           │
│  → document_db.c: store document + chunks + embeddings in SQLite      │
│  → SHA-256 dedup prevents re-indexing identical files                  │
└───────────────────────────────────────────────────────────────────────┘
                                │
                                ▼
┌───────────────────────────────────────────────────────────────────────┐
│                     LLM TOOL ACCESS                                    │
├───────────────────────────────────────────────────────────────────────┤
│                                                                        │
│  document_search(query)        document_read(document, start, count)  │
│  ├─ Embed query                ├─ Find document by name/ID            │
│  ├─ Load all user chunks       ├─ Load chunks in order (paginated)    │
│  ├─ Cosine similarity rank     ├─ Return text with pagination hint    │
│  ├─ Keyword boost (top 50)     └─ LLM calls again for next page      │
│  └─ Return top 5 with scores                                          │
│                                                                        │
└───────────────────────────────────────────────────────────────────────┘
```

#### Key Components

- **embedding_engine.c/h** (`src/core/`): Shared embedding infrastructure
   - Extracted from `memory_embeddings.c` — used by both memory and document systems
   - Multi-provider: ONNX local (`all-MiniLM-L6-v2`), Ollama, OpenAI-compatible
   - Thread-safe embed() with mutex protection
   - ARM NEON vectorized dot product for cosine similarity
   - `embedding_engine_init()`, `embedding_engine_embed()`, `embedding_engine_cosine()`

- **document_db.c/h** (`src/tools/`): SQLite CRUD for documents and chunks
   - Uses shared `auth.db` handle with prepared statements
   - `document_db_create()`, `document_db_delete()`, `document_db_list()`
   - `document_db_find_by_name()`: fuzzy name lookup with LIKE + exact-match priority
   - `document_db_chunk_create()`, `document_db_chunk_read()`, `document_db_chunk_search_load()`
   - Access control: queries filter by `user_id = ? OR is_global = 1`

- **document_chunker.c/h** (`src/tools/`): Text chunking for embedding
   - Paragraph-aware splitting (double newline boundaries)
   - Sentence boundary splitting for oversized paragraphs
   - Configurable target/max token sizes and overlap

- **document_search.c/h** (`src/tools/`): Semantic search tool
   - Two-pass scoring: cosine similarity first, keyword boost on top 50 only
   - Results formatted with source citations and relevance scores
   - Uses `result_extended` for results exceeding the 8KB fixed buffer

- **document_read.c/h** (`src/tools/`): Paginated document reader
   - Parameters: `document` (name), `start_chunk` (offset), `count` (page size, max 20)
   - Returns ordered chunk text with pagination hint for next page
   - Enables full-document summarization via iterative reading

- **webui_doc_library.c** (`src/webui/`): WebUI Document Library panel
   - Upload with file type validation (PDF, DOCX, TXT, MD)
   - Per-user document count limits (configurable)
   - List, delete, index, and toggle-global WebSocket endpoints
   - Admin "All Users" view with username resolution (JOIN on users table)
   - Global visibility toggle: owner or admin can share/unshare documents
   - Audit logging for admin cross-user operations
   - `conn_check_admin_quiet()` for soft-fallback admin checks (no error response)

- **doc-library.js / doc-library.css** (`www/`): WebUI frontend
   - Drag-and-drop upload with progress indicator
   - Document list with type badges, chunk counts, delete confirmation
   - Admin controls: "All Users" toggle, global upload checkbox, per-document globe toggle
   - Owner badge (username) on documents in admin "All Users" view
   - SVG globe icon with filled/stroke states for global visibility
   - Focus trap and keyboard focus-visible styles for accessibility
   - Mobile-friendly: bottom-sheet layout, 44px+ touch targets

#### Database Schema

Two tables in `auth.db`:

```sql
documents (id, user_id, filename, filepath, filetype, file_hash,
           num_chunks, is_global, created_at)

document_chunks (id, document_id, chunk_index, text, embedding, embedding_norm)
```

Indexes: `idx_doc_chunks_doc`, `idx_documents_user`, `idx_documents_hash`

#### Configuration

```toml
[documents]
chunk_target_tokens = 500
chunk_max_tokens = 1000
chunk_overlap_tokens = 50
max_search_results = 5
max_context_tokens = 2000
max_file_size_mb = 10
max_index_size_kb = 2048       # Per-document index size limit
max_indexed_documents = 50     # Per-user document count limit
```

---

### 12. CalDAV Calendar Subsystem (`src/tools/caldav_client.c`, `src/tools/calendar_db.c`, `src/tools/calendar_service.c`, `src/tools/calendar_tool.c`)

**Purpose**: CalDAV calendar integration — query, create, update, and delete calendar events across multiple providers (Google Calendar, iCloud, Nextcloud, Radicale) via the standard RFC 4791 protocol.

#### Architecture: **4-Layer Design**

```
┌───────────────────────────────────────────────────────────────────────┐
│                     LLM TOOL INTERFACE                                │
│  calendar_tool.c                                                      │
│  Actions: today | range | next | search | add | update | delete       │
│  → Registered via tool_registry, invoked by LLM tool loop             │
├───────────────────────────────────────────────────────────────────────┤
│                     BUSINESS LOGIC                                    │
│  calendar_service.c                                                   │
│  → Multi-account routing (personal, work, shared calendars)           │
│  → Background sync thread (configurable interval)                     │
│  → RRULE expansion via libical (pre-expanded occurrences in DB)       │
│  → Conflict detection and timezone normalization                      │
├───────────────────────────────────────────────────────────────────────┤
│                     SQLITE STORAGE                                    │
│  calendar_db.c                                                        │
│  Tables: caldav_accounts, calendars, events, event_occurrences        │
│  → Shares auth_db SQLite handle                                       │
│  → Pre-expanded occurrences for fast range queries                    │
│  → ctag/etag tracking for efficient sync                              │
├───────────────────────────────────────────────────────────────────────┤
│                     CALDAV PROTOCOL                                   │
│  caldav_client.c                                                      │
│  → RFC 4791 PROPFIND/REPORT/PUT/DELETE over HTTPS                    │
│  → Principal and calendar-home-set discovery                          │
│  → REPORT calendar-query with time-range filters                      │
│  → iCalendar (RFC 5545) parsing via libical                           │
└───────────────────────────────────────────────────────────────────────┘
```

#### Key Design Points

- **Multi-account**: Supports multiple CalDAV accounts simultaneously (e.g., personal Google + work Nextcloud)
- **Offline-first**: Events are cached locally in SQLite; queries hit the DB, not the network
- **RRULE expansion**: Recurring events are pre-expanded into `event_occurrences` so range queries are simple SQL
- **Background sync**: A dedicated thread periodically pulls changes from CalDAV servers using ctag/etag for efficiency
- **Provider compatibility**: Tested with Google Calendar, Apple iCloud, Nextcloud, and Radicale

---

### 13. Email Subsystem (`src/tools/email_*.c`, `src/webui/webui_email.c`)

**Purpose**: Multi-account email via IMAP/SMTP and Gmail REST API, with voice-controlled send, read, search, trash, and archive

#### Architecture: **Dual Backend + Service Router**

```
┌───────────────────────────────────────────────────────────────────────┐
│                     LLM TOOL INTERFACE                                │
│  email_tool.c                                                        │
│  Actions: recent | read | search | folders | send | confirm_send     │
│           | accounts | trash | confirm_trash | archive               │
│  → TOOL_CAP_DANGEROUS: compile-time + runtime gates                  │
├───────────────────────────────────────────────────────────────────────┤
│                     SERVICE LAYER                                     │
│  email_service.c                                                     │
│  → Multi-account routing (dispatches to correct backend per account) │
│  → Two-step confirmation for send and trash (draft → confirm)        │
│  → Per-account read-only flag                                        │
│  → Pagination for large result sets                                  │
├───────────────────────────────────────────────────────────────────────┤
│              BACKEND A                     BACKEND B                  │
│  email_client.c (IMAP/SMTP)    gmail_client.c (Gmail REST API)      │
│  → libcurl for IMAP/SMTP       → OAuth Bearer + XOAUTH2             │
│  → App password or XOAUTH2     → REST endpoints for all operations   │
│  → Any IMAP provider           → Google-specific (thread model)      │
├───────────────────────────────────────────────────────────────────────┤
│                     SQLITE STORAGE                                    │
│  email_db.c                                                          │
│  Tables: email_accounts (encrypted passwords via crypto_store)       │
│  → Shares auth_db SQLite handle                                      │
└───────────────────────────────────────────────────────────────────────┘
```

#### Key Design Points

- **Dual backend**: IMAP/SMTP for any provider, Gmail REST API for OAuth accounts (auto-selected per account)
- **Two-step confirmation**: Send and trash require a confirm step — the LLM drafts, then the user confirms
- **Contacts integration**: Recipient resolution via `contacts_find()` — "email Bob" resolves to Bob's stored email
- **Compile-time gate**: `DAWN_ENABLE_EMAIL_TOOL=ON` in CMake; runtime gate in `[email] enabled`
- **WebUI management**: Account CRUD via `webui_email.c`, Google OAuth connect flow

---

### 14. OAuth 2.0 / Crypto Subsystem (`src/tools/oauth_client.c`, `src/core/crypto_store.c`)

**Purpose**: Shared OAuth 2.0 authentication and encrypted credential storage

#### Key Components

- **oauth_client.c/h**: OAuth 2.0 client with PKCE S256
   - Provider-agnostic design (currently Google, extensible to Microsoft 365)
   - Authorization URL generation with PKCE challenge
   - Code exchange and token refresh
   - Token storage in `oauth_tokens` SQLite table (encrypted via crypto_store)
   - WebUI popup consent flow with `postMessage` origin validation

- **crypto_store.c/h**: Shared libsodium encryption module
   - `crypto_secretbox` (XSalsa20-Poly1305) for symmetric encryption
   - Key file: `dawn.key` (auto-generated on first use, 256-bit)
   - Used by OAuth (token encryption) and email (password encryption)
   - `crypto_store_encrypt()` / `crypto_store_decrypt()` API

#### OAuth Flow

```
WebUI → popup window → Google consent → redirect to /oauth/callback
  → callback page posts auth code via postMessage to opener
  → opener sends code to daemon via WebSocket
  → daemon exchanges code for tokens (PKCE verification)
  → tokens encrypted and stored in oauth_tokens table
  → automatic refresh on expiry
```

---

### 15. Scheduler Subsystem (`src/core/scheduler.c`, `src/core/scheduler_db.c`, `src/tools/scheduler_tool.c`)

**Purpose**: Timers, alarms, reminders, and scheduled tool execution with audible chimes and WebUI notifications

#### Architecture: **Background Thread + SQLite + Tool Interface**

```
┌───────────────────────────────────────────────────────────────────────┐
│                     LLM TOOL INTERFACE                                │
│  scheduler_tool.c                                                    │
│  Actions: create | list | cancel | query | snooze | dismiss          │
│  → "Set a 10 minute timer", "Remind me at 3pm", "Cancel all timers" │
├───────────────────────────────────────────────────────────────────────┤
│                     SCHEDULER ENGINE                                  │
│  scheduler.c                                                         │
│  → Background thread polls every second                              │
│  → Fires events when time arrives (chime audio + WebSocket notify)   │
│  → Recurrence: daily, weekdays, weekends, weekly, custom days        │
│  → Snooze (configurable duration) and dismiss                        │
│  → Scheduled tasks: execute any registered tool at a given time      │
├───────────────────────────────────────────────────────────────────────┤
│                     SQLITE STORAGE                                    │
│  scheduler_db.c                                                      │
│  Table: scheduler_events (user_id, type, label, fire_at, recurrence) │
│  → Shares auth_db SQLite handle                                      │
│  → Prepared statements for CRUD and time-range queries               │
└───────────────────────────────────────────────────────────────────────┘
```

#### Key Design Points

- **Event types**: timer (countdown), alarm (absolute time), reminder (absolute + label), task (absolute + tool invocation)
- **Chime audio**: Built-in chime WAV played via audio subsystem at configurable volume
- **WebUI notifications**: `scheduler_fire` WebSocket message triggers banner with snooze/dismiss buttons
- **Recurrence**: Events auto-reschedule after firing based on recurrence pattern
- **94 unit test assertions** across 16 tests in `tests/test_scheduler.c`

---

### 16. Home Assistant Subsystem (`src/tools/homeassistant_service.c`, `src/tools/homeassistant_tool.c`)

**Purpose**: Smart home control via Home Assistant REST API — lights, climate, locks, covers, media players, scenes, scripts, automations

#### Architecture: **Service + Tool + WebUI Admin**

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

#### Key Design Points

- **Fuzzy matching**: "Turn on the living room light" works even if the HA entity name differs slightly
- **Area-aware**: Satellite user mapping injects `HomeAssistant_Area=[X]` into LLM system prompt
- **Feature guard**: `DAWN_ENABLE_HOMEASSISTANT_TOOL` CMake option; mutually exclusive with SmartThings
- **Entity cache**: Avoids per-request API calls; refreshed on configurable interval

---

### 17. Per-User Settings (`src/auth/auth_db_settings.c`, `src/webui/webui_settings.c`)

**Purpose**: Per-user personalization — persona, location, timezone, units, theme

#### Key Components

- **auth_db_settings.c**: `user_settings` SQLite table with per-user preferences
   - Persona (append or replace mode), location, timezone, units, theme
   - CRUD via prepared statements on shared auth_db handle

- **webui_settings.c + my-settings.js**: "My Settings" WebUI panel
   - Per-user preferences editable from the browser
   - Theme selection syncs immediately

- **build_user_prompt()**: System prompt personalization
   - Injects user preferences (persona, location, timezone, units) into LLM system prompt at session start
   - Each session is personalized to the authenticated user

---

## Module Dependency Hierarchy

To prevent circular dependencies and maintain clean architecture, modules are organized into layers. **Modules may only depend on modules in lower layers.**

```
Layer 0 (Foundation)
├── logging.c/h           - Logging macros (no dependencies)
├── config/               - Configuration parsing and defaults
│   ├── dawn_config.h
│   ├── config_parser.c
│   └── config_defaults.c
└── tools/string_utils.c  - String helpers

Layer 1 (Core Infrastructure)
├── tool_registry.c/h     - Tool registration and lookup (depends: logging, config)
├── command_router.c/h    - Request/response routing (depends: logging)
├── session_manager.c/h   - Session lifecycle (depends: logging, config)
└── input_queue.c/h       - Thread-safe input queue (depends: logging)

Layer 2 (Services)
├── llm/                  - LLM providers and tools
│   ├── llm_interface.c   - Provider abstraction (depends: Layer 0-1)
│   ├── llm_openai.c      - OpenAI/Ollama/llama.cpp (depends: llm_interface)
│   ├── llm_claude.c      - Anthropic Claude (depends: llm_interface)
│   └── llm_tools.c       - Tool execution (depends: tool_registry)
├── core/embedding_engine.c - Shared embedding infrastructure (depends: Layer 0-1)
├── core/crypto_store.c   - Shared libsodium encryption (depends: Layer 0)
├── core/scheduler.c      - Scheduler engine + background thread (depends: Layer 0-1)
├── tts/                  - Text-to-speech (depends: Layer 0-1)
├── asr/                  - Speech recognition (depends: Layer 0-1)
├── mosquitto_comms.c     - MQTT integration (depends: Layer 0-1, tool_registry)
├── memory/               - Persistent memory + contacts (depends: Layer 0-1, embedding_engine)
└── auth/                 - User auth, settings, per-user prefs (depends: Layer 0-1)

Layer 3 (Tools)
├── tools/weather_tool.c      - Weather API (depends: Layer 0-2)
├── tools/music_tool.c        - Music playback (depends: Layer 0-2)
├── tools/search_tool.c       - Web search (depends: Layer 0-2)
├── tools/memory_tool.c       - Memory commands (depends: Layer 0-2, memory/)
├── tools/document_search.c   - RAG semantic search (depends: Layer 0-2, embedding_engine)
├── tools/document_read.c     - Paginated doc reader (depends: Layer 0-2, document_db)
├── tools/document_db.c       - Document SQLite CRUD (depends: Layer 0-1, auth_db)
├── tools/email_service.c     - Email routing + two-step confirm (depends: Layer 0-2, oauth_client)
├── tools/email_client.c      - IMAP/SMTP backend (depends: Layer 0-1, crypto_store)
├── tools/gmail_client.c      - Gmail REST API backend (depends: Layer 0-1, oauth_client)
├── tools/oauth_client.c      - OAuth 2.0 + PKCE (depends: Layer 0-1, crypto_store)
├── tools/homeassistant_service.c - HA REST API + entity cache (depends: Layer 0-1)
├── tools/calendar_service.c  - CalDAV business logic (depends: Layer 0-2, oauth_client)
└── tools/*.c                 - All other tools (depends: Layer 0-2)

Layer 4 (Application)
├── dawn.c                - Main entry point (depends: all layers)
├── worker_pool.c         - Concurrent processing (depends: Layer 0-3)
├── state_machine.c       - Voice state machine (depends: Layer 0-3)
└── webui/                - Web interface (depends: Layer 0-3)
```

### Dependency Rules

1. **Downward only**: A module may only `#include` headers from its own layer or lower layers
2. **No cycles**: If A depends on B, B must not depend on A (directly or transitively)
3. **Interface segregation**: Use forward declarations and callbacks to break potential cycles
4. **Same-layer allowed**: Modules in the same layer may depend on each other if acyclic

### Common Patterns to Avoid Cycles

**Callback registration** (Layer 2 → Layer 3 without direct dependency):

```c
// In tool_registry.h (Layer 1)
typedef char *(*tool_callback_t)(const char *action, char *value, int *should_respond);

// In weather_tool.c (Layer 3) - registers callback at init
tool_registry_register(&weather_metadata);  // Passes function pointer up
```

**Forward declarations** (when header inclusion would create cycle):

```c
// In llm_tools.h - avoid including full tool_registry.h
struct tool_metadata;  // Forward declaration
```

---

## Data Flow

### Local Voice Command Flow

```
1. Microphone Capture
   ↓
2. Ring Buffer (Capture Thread → Main Thread)
   ↓
3. VAD Detection (Silero) - Detect speech/silence
   ↓
4. State Machine Transition (SILENCE → WAKEWORD_LISTEN)
   ↓
5. Wake Word Detection (Whisper/Vosk) - "friday"
   ↓
6. State Machine Transition (WAKEWORD_LISTEN → COMMAND_RECORDING)
   ↓
7. Command Recording (with VAD monitoring for silence)
   ↓
8. ASR Processing (Whisper/Vosk with GPU acceleration)
   ↓
9. Transcript → LLM (OpenAI GPT-5/Claude 4.6/local with streaming)
   ↓
10. LLM Response Streaming
    ↓
11. Sentence Buffer (accumulate until sentence boundary)
    ↓
12. TTS Synthesis (Piper with preprocessing) - Per-sentence
    ↓
13. Audio Playback (ALSA/PulseAudio)
    ↓
14. Command Parsing (extract JSON commands)
    ↓
15. MQTT Publish (execute OASIS component commands)
    ↓
16. State Machine Transition (PROCESSING → SILENCE)
```

### Remote Voice Command Flow (WebSocket Clients)

```
1. Client connects via WebSocket (WebUI browser, Tier 1 or Tier 2 satellite)
   ↓
2. Session registration (satellite_register with capabilities)
   ↓
   ┌─────────── Tier 1 (Text Path) ────────────┐  ┌──── Tier 2 / WebUI (Audio Path) ────┐
   │ 3a. Local ASR on satellite                 │  │ 3b. Binary audio frames (0x01)       │
   │ 4a. satellite_query {text: "..."}          │  │ 4b. Audio end marker (0x02)          │
   │                                            │  │ 5b. Server-side ASR (Whisper/Vosk)   │
   └────────────────────┬───────────────────────┘  └──────────────────┬───────────────────┘
                        │                                             │
                        └──────────────┬──────────────────────────────┘
                                       ↓
   6. LLM Processing (same as local flow)
      ↓
   7. Streaming response (stream_start → stream_delta → stream_end)
      ↓
   ┌─────────── Tier 1 (Text Path) ────────────┐  ┌──── Tier 2 / WebUI (Audio Path) ────┐
   │ 8a. Satellite receives text deltas         │  │ 8b. Server-side TTS (Piper)          │
   │ 9a. Local TTS synthesis (Piper)            │  │ 9b. Binary audio frames (0x11/0x12)  │
   │ 10a. Local playback                        │  │ 10b. Client playback                 │
   └────────────────────────────────────────────┘  └──────────────────────────────────────┘
```

---

## Threading Model

### Current Threading Architecture

D.A.W.N. uses a **minimal threading model** for simplicity on embedded systems:

```
┌──────────────────────────────────────────────────────────┐
│                      Main Thread                         │
│                                                          │
│  - State machine (SILENCE → WAKEWORD → COMMAND → PROC)   │
│  - ASR processing (Whisper/Vosk)                         │
│  - LLM communication (blocking or streaming)             │
│  - TTS synthesis (mutex protected)                       │
│  - MQTT communication                                    │
│  - Network server (single client, blocks during proc)    │
└──────────────────────────────────────────────────────────┘

┌──────────────────────────────────────────────────────────┐
│                   Capture Thread                         │
│                                                          │
│  - Continuous audio capture (ALSA/PulseAudio)            │
│  - Write to ring buffer                                  │
│  - No processing logic                                   │
└──────────────────────────────────────────────────────────┘
```

### Thread Synchronization

- **Ring Buffer**: Thread-safe circular buffer for audio data
- **TTS Mutex** (`tts_mutex`): Protects TTS engine from concurrent access
- **No locks in main loop**: State machine is single-threaded

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

### State Descriptions

- **SILENCE**: Waiting for speech detection (VAD monitors audio)
- **WAKEWORD_LISTEN**: Detected speech, listening for wake word ("friday")
- **COMMAND_RECORDING**: Wake word confirmed, recording user command until silence
- **PROCESSING**: Command recorded, processing through ASR → LLM → TTS → MQTT pipeline

### State Transitions

| From State        | Event                 | To State          |
| ----------------- | --------------------- | ----------------- |
| SILENCE           | VAD detects speech    | WAKEWORD_LISTEN   |
| WAKEWORD_LISTEN   | Wake word detected    | COMMAND_RECORDING |
| WAKEWORD_LISTEN   | Timeout / false alarm | SILENCE           |
| COMMAND_RECORDING | VAD detects silence   | PROCESSING        |
| PROCESSING        | Pipeline complete     | SILENCE           |

---

## DAP2 Satellite Protocol

**Status**: ✅ **Implemented** (January–February 2026)

DAP2 is the unified WebSocket protocol for all remote access to the DAWN daemon. **A single WebSocket server on port 3000 serves all three client types** — browser WebUI, Tier 1 satellites (Raspberry Pi), and Tier 2 satellites (ESP32). There are no separate servers or additional ports. Each client connects to the same endpoint, registers its capabilities, and the daemon routes messages accordingly:

| Client     | Hardware | Transport                     | Server does     | Use Case                      |
| ---------- | -------- | ----------------------------- | --------------- | ----------------------------- |
| **WebUI**  | Browser  | Opus audio (48kHz) + JSON     | ASR + LLM + TTS | Browser voice/text interface  |
| **Tier 1** | RPi 4/5  | JSON text (`satellite_query`) | LLM only        | Hands-free (local ASR/TTS)    |
| **Tier 2** | ESP32-S3 | Binary PCM audio (16kHz)      | ASR + LLM + TTS | Push-to-talk (server ASR/TTS) |

This unified architecture means the session manager, response queue, LLM pipeline, tool system, and conversation history are shared infrastructure — adding a new client type requires only a registration handler and a routing decision, not a new server.

**Tier 2 audio reuses the WebUI audio subsystem** ([Section 7](#7-webui-audio-subsystem)) — same binary message types, same `webui_audio.c` worker threads, same ASR→LLM→TTS pipeline. The only difference is raw PCM at 16kHz instead of Opus at 48kHz (no codec or resample step on server). TTS audio is sent at native Piper rate (22050Hz); the ESP32 resamples to 48kHz for I2S output. Music streaming (Opus over a dedicated WebSocket) is Tier 1 only.

**Tier 2 implementation**: `dawn_satellite_arduino/` — Arduino sketch for Adafruit ESP32-S3 TFT Feather. Uses arduinoWebSockets (Links2004), power-of-two ring buffer in PSRAM with spinlock producer/consumer, NVS-persistent UUID and reconnect_secret, TFT status display, NeoPixel state feedback. Credentials in gitignored `arduino_secrets.h`.

For connection lifecycle diagrams, message format details, and the complete protocol specification, see:

- **[WEBSOCKET_PROTOCOL.md](docs/WEBSOCKET_PROTOCOL.md)** — Complete wire protocol reference (all message types and payloads)
- **[DAP2_SATELLITE.md](docs/DAP2_SATELLITE.md)** — Tier 1 build/config/deployment + Tier 2 quick reference
- **[dawn_satellite_arduino/README.md](dawn_satellite_arduino/README.md)** — Tier 2 Arduino sketch setup and usage

---

## Command Processing Architecture

DAWN supports three parallel command processing paths that all converge on a unified executor.

### Tool Registry System

The **tool_registry** (`src/tools/tool_registry.c`) is the primary mechanism for registering modular tools. Each tool is a self-contained module with its own metadata, parameters, and callback:

```c
static const tool_metadata_t my_tool_metadata = {
   .name = "my_tool",              // API name for LLM tool calls
   .device_string = "my device",   // Internal device identifier
   .description = "Tool description for LLM schema",
   .params = my_tool_params,       // Parameter definitions
   .param_count = 2,
   .device_type = TOOL_DEVICE_TYPE_GETTER,
   .capabilities = TOOL_CAP_NETWORK,
   .is_getter = true,
   .default_remote = true,
   .callback = my_tool_callback,
};
```

**Key Features:**

- **O(1) Lookup**: FNV-1a hash tables for name, device_string, and aliases
- **Self-Registration**: Each tool calls `tool_registry_register()` during init
- **LLM Schema Generation**: `tool_registry_generate_llm_tools()` builds provider-specific schemas
- **Capability Flags**: `TOOL_CAP_NETWORK`, `TOOL_CAP_DANGEROUS`, etc. for safety classification

**Registered Tools** (as of March 2026):

- audio_tools, calculator_tool, calendar_tool, datetime_tool, document_read_tool
- document_search_tool, email_tool, homeassistant_tool, hud_tools, llm_status_tool
- memory_tool, music_tool, plan_executor_tool, reset_conversation_tool, scheduler_tool
- search_tool, shutdown_tool, switch_llm_tool, url_tool, viewing_tool, volume_tool, weather_tool

### Command Flow

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                           USER INPUT (Voice/Text)                           │
└─────────────────────────────────────────────────────────────────────────────┘
                                      │
                                      ▼
┌─────────────────────────────────────────────────────────────────────────────┐
│                      PROCESSING MODE (dawn.toml: commands.processing_mode)  │
│                                                                             │
│   direct_only ──────► Pattern match only, no LLM                            │
│   llm_only ─────────► Send everything to LLM                                │
│   direct_first ─────► Try patterns, fallback to LLM                         │
└─────────────────────────────────────────────────────────────────────────────┘
                                      │
                    ┌─────────────────┴─────────────────┐
                    ▼                                   ▼
┌──────────────────────────────┐       ┌──────────────────────────────────────┐
│   PATH 1: DIRECT MATCHING    │       │         PATH 2: LLM INVOCATION       │
│   (text_to_command_nuevo.c)  │       │                                      │
│                              │       │    ┌────────────────────────────┐    │
│  Regex patterns from JSON:   │       │    │  native_enabled = true?    │    │
│  "turn on %device_name%"     │       │    └────────────┬───────────────┘    │
│  "play %value%"              │       │                 │                    │
│                              │       │     ┌───────────┴───────────┐        │
│  Extracts device/action/val  │       │     ▼                       ▼        │
└──────────────┬───────────────┘       │ ┌───────────┐       ┌─────────────┐  │
               │                       │ │PATH 2A:   │       │PATH 2B:     │  │
               │                       │ │NATIVE     │       │LEGACY       │  │
               │                       │ │TOOLS      │       │<command>    │  │
               │                       │ │           │       │TAGS         │  │
               │                       │ │LLM returns│       │             │  │
               │                       │ │structured │       │LLM returns  │  │
               │                       │ │tool_calls │       │<command>JSON│  │
               │                       │ └─────┬─────┘       └──────┬──────┘  │
               │                       │       │                    │         │
               │                       └───────┼────────────────────┼─────────┘
               │                               │                    │
               │                               ▼                    ▼
               │                       ┌─────────────┐      ┌─────────────────┐
               │                       │llm_tools_   │      │webui_process_   │
               │                       │execute()    │      │commands()       │
               │                       │             │      │                 │
               │                       │Parses tool  │      │Parses <command> │
               │                       │call struct  │      │tags from text   │
               │                       └─────┬───────┘      └────────┬────────┘
               │                             │                       │
               └─────────────────────────────┼───────────────────────┘
                                             │
                                             ▼
┌─────────────────────────────────────────────────────────────────────────────┐
│                    UNIFIED COMMAND EXECUTOR (command_executor.c)            │
│                                                                             │
│   command_execute(device, action, value, mosq, &result)                     │
│                                                                             │
│   1. Look up device in command_registry                                     │
│   2. If has_callback → invoke deviceCallbackArray[type].callback()          │
│   3. If mqtt_only → publish JSON to MQTT topic                              │
│   4. If sync_wait → use command_router for response (viewing)               │
└─────────────────────────────────────────────────────────────────────────────┘
                                             │
               ┌─────────────────────────────┼─────────────────────────────┐
               ▼                             ▼                             ▼
┌──────────────────────┐       ┌──────────────────────┐       ┌─────────────────┐
│ C CALLBACKS          │       │ MQTT-ONLY            │       │ SYNC WAIT       │
│ (mosquitto_comms.c)  │       │ (Hardware)           │       │ (viewing)       │
│                      │       │                      │       │                 │
│ deviceCallbackArray: │       │ Publish to topic:    │       │ Wait for MQTT   │
│ - weather → get_wea  │       │ - "hud" → helmet     │       │ response via    │
│ - music → play_music │       │ - "helmet" → helmet  │       │ command_router  │
│ - search → web_sear  │       │ - "smartthings"      │       │                 │
│ - date → get_date    │       │                      │       │                 │
└──────────────────────┘       └──────────────────────┘       └─────────────────┘
```

### Command Definition Sources

Commands are defined via the **modular tool_registry** system:

1. **Tool Registry** (`src/tools/*.c`)
   - Each tool is a self-contained module with `tool_metadata_t` struct
   - Registered via `tool_registry_register()` during `tools_register_all()`
   - O(1) lookup via FNV-1a hash tables for name, device_string, and aliases

2. **Legacy Device Callbacks** (`mosquitto_comms.c`)
   - `deviceCallbackArray[]` maps device types to C functions
   - Core system devices: weather, music, search, smartthings, etc.

### Native Tools vs Legacy `<command>` Tags

| Aspect         | Native Tools                              | Legacy `<command>` Tags             |
| -------------- | ----------------------------------------- | ----------------------------------- |
| **Definition** | command_registry → llm_tools              | Raw JSON in prompt                  |
| **Prompt**     | Minimal (tools sent as API params)        | Full `<command>` instructions       |
| **Response**   | Structured `tool_calls` array             | Text with `<command>JSON</command>` |
| **Filtering**  | `enabled_local`/`enabled_remote` per tool | Same flags                          |
| **Execution**  | `command_execute()`                       | `command_execute()`                 |

### Tool Enable/Disable

Tools can be enabled/disabled per session type (local vs remote):

- Settings UI provides per-tool toggles
- Legacy `<command>` prompt is filtered by the same enabled flags
- Disabled tools are omitted from both native tool schemas and legacy prompt

---

## Component Interactions

### ASR Engine Selection

```c
// src/asr/asr_interface.c
ASRContext *asr_init(const char *model_path) {
#ifdef ENABLE_VOSK
   return asr_vosk_init(model_path);  // Use Vosk if enabled
#else
   return asr_whisper_init(model_path);  // Otherwise use Whisper
#endif
}
```

### LLM Provider Selection

```c
// src/llm/llm_interface.c
LLMContext *llm_init() {
   if (config.llm.type == LLM_TYPE_CLOUD) {
      if (config.llm.cloud.provider == PROVIDER_OPENAI) {
         return llm_openai_init();  // OpenAI models (GPT-4o, etc.)
      } else if (config.llm.cloud.provider == PROVIDER_GEMINI) {
         return llm_openai_init();  // Gemini via OpenAI-compatible endpoint
      } else {
         return llm_claude_init();  // Claude models
      }
   } else {
      // Local: Both llama.cpp and Ollama use OpenAI-compatible endpoint
      return llm_openai_init();
   }
}
```

### TTS Thread Safety

```c
// src/tts/text_to_speech.cpp
void text_to_speech(const char *text) {
   pthread_mutex_lock(&tts_mutex);  // Protect TTS engine

   // Piper synthesis
   piper_synthesize(text);

   pthread_mutex_unlock(&tts_mutex);
}
```

### Streaming LLM → TTS Integration

```c
// src/llm/llm_streaming.c
void on_llm_chunk_received(const char *chunk) {
   sentence_buffer_append(chunk);  // Accumulate text

   if (sentence_buffer_has_complete_sentence()) {
      char *sentence = sentence_buffer_get_sentence();
      text_to_speech(sentence);  // Speak complete sentence immediately
      free(sentence);
   }
}
```

---

## Memory Management

### Design Principles

1. **Prefer Static Allocation**: Embedded systems benefit from predictable memory usage
2. **Minimize Dynamic Allocation**: Use malloc/calloc sparingly
3. **Always Check NULL**: Verify dynamic allocation succeeded
4. **Free and NULL**: Set pointers to NULL after freeing

### Memory Patterns

#### Static Buffers (Preferred)

```c
// Example: Audio buffer
#define AUDIO_BUFFER_SIZE 16000
static int16_t audio_buffer[AUDIO_BUFFER_SIZE];
```

#### Dynamic Allocation (When Necessary)

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

#### Ring Buffer (Lock-Free)

```c
// Circular buffer with atomic read/write pointers
typedef struct {
   int16_t *buffer;
   size_t size;
   volatile size_t read_idx;
   volatile size_t write_idx;
} RingBuffer;
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

**Total (Whisper)**: ~230 MB RAM minimum
**Total (Vosk)**: ~140 MB RAM minimum

---

## Error Handling

### Error Code Convention

```c
#define SUCCESS  0
#define FAILURE  1
// Additional specific errors > 1
#define ERROR_INVALID_FORMAT  2
#define ERROR_NETWORK_TIMEOUT 3
// ... etc
```

**IMPORTANT**: Do NOT use negative return values (-1, -errno). Use positive error codes only.

### Error Handling Patterns

#### Function Return Codes

```c
int asr_process_audio(ASRContext *ctx, int16_t *audio, size_t samples) {
   if (ctx == NULL || audio == NULL) {
      LOG_ERROR("Invalid parameters");
      return FAILURE;
   }

   // ... processing ...

   if (error_occurred) {
      LOG_ERROR("ASR processing failed: %s", error_msg);
      return FAILURE;
   }

   return SUCCESS;
}
```

#### Network Protocol Errors

```c
// Retry logic with exponential backoff
int retry_count = 0;
while (retry_count < MAX_RETRIES) {
   if (send_packet(packet) == SUCCESS) {
      break;
   }
   LOG_WARNING("Send failed, retry %d/%d", retry_count + 1, MAX_RETRIES);
   sleep(1 << retry_count);  // Exponential backoff: 1s, 2s, 4s
   retry_count++;
}
```

#### Graceful Degradation

```c
// Example: Fall back to simpler ASR if GPU unavailable
if (gpu_available) {
   ctx = asr_whisper_init(model_path);
} else {
   LOG_WARNING("GPU not available, using CPU-only ASR");
   ctx = asr_vosk_init(model_path);
}
```

---

## Performance Considerations

### GPU Acceleration (Jetson)

- **Automatic Detection**: CMake detects Jetson platform via `/etc/nv_tegra_release`
- **CUDA Libraries**: cuSPARSE, cuBLAS, cuSOLVER, cuRAND linked automatically
- **Whisper GPU**: Enabled with `GGML_CUDA=ON` for Whisper.cpp
- **Performance Gain**: 2.3x - 5.5x speedup over CPU

### Latency Optimization

**Total Perceived Latency** = ASR Time + TTFT + TTS Time

| Component          | Latency (Whisper base GPU) | Notes                     |
| ------------------ | -------------------------- | ------------------------- |
| ASR (Whisper base) | ~110 ms                    | GPU accelerated           |
| TTFT (Qwen3-4B)    | ~138 ms                    | Local LLM first token     |
| TTS (Piper)        | ~200 ms                    | First sentence            |
| **Total**          | **~448 ms**                | User hears first response |

**Streaming Advantage**: With streaming LLM + TTS, user hears response in <500ms instead of waiting for complete LLM response (~3s).

### Optimization Tips

1. **Use Whisper base model**: Best balance of speed/accuracy on Jetson
2. **Enable GPU acceleration**: Automatic on Jetson, verify in logs
3. **Streaming LLM + TTS**: Reduces perceived latency significantly
4. **Optimize batch size**: For local LLM, batch=768 critical for quality
5. **VAD tuning**: Adjust sensitivity to reduce false positives

---

## Platform Support

### NVIDIA Jetson

- **Auto-Detection**: CMake checks `/etc/nv_tegra_release`
- **GPU Acceleration**: Enabled automatically (CUDA 12.6)
- **Recommended Models**: Whisper base, Qwen3-4B Q4
- **Performance**: Excellent (GPU acceleration for ASR)

### Raspberry Pi

- **Auto-Detection**: CMake checks `/sys/firmware/devicetree/base/model`
- **CPU-Only**: No CUDA support
- **Recommended Models**: Vosk 0.22, smaller Whisper models
- **Performance**: Good (CPU-only, slower ASR)

### Generic ARM64

- **Fallback Detection**: CMake checks `CMAKE_SYSTEM_PROCESSOR`
- **Assumed Platform**: RPI (no GPU)
- **Performance**: Varies by hardware

### Platform Override

```bash
cmake -DPLATFORM=JETSON ..  # Force Jetson (enables CUDA)
cmake -DPLATFORM=RPI ..     # Force RPi (disables CUDA)
```

---

## File Organization Standards

### Size Limits

To prevent files from becoming unmaintainable monoliths, follow these limits:

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

The internal header contains:

- `extern` declarations for shared state (defined in `_core.c`)
- Internal helper function declarations
- Shared macros (e.g., locking patterns)

### When Adding New Features

1. **Check file size first** - If target file > 1,500 lines, consider creating a new file
2. **Group by feature** - Related functionality goes together in one module
3. **Use internal headers** - Share state via `*_internal.h` pattern
4. **Update build system** - Add new source files immediately

---

## Configuration Architecture

### Design Principles

1. **Config Files as Source of Truth**: All DAWN application settings are defined in `dawn.toml` (runtime configuration) or `secrets.toml` (credentials and API keys). The SQLite database is reserved for:
   - User authentication and sessions
   - Conversation history
   - Uploaded images
   - Other user-generated content

   **Settings are NEVER stored in the database.** This separation ensures configuration is portable, version-controllable (minus secrets), and easily inspectable.

2. **WebUI Settings Exposure**: All settings defined in `dawn.toml` are exposed in the WebUI settings panel unless explicitly excluded. Exclusions are limited to:
   - File system paths (security risk if editable remotely)
   - Internal debugging flags
   - Settings that require restart and have no runtime effect

3. **Secrets Isolation**: Credentials (`secrets.toml`) are kept separate from general configuration (`dawn.toml`) to allow:
   - Sharing `dawn.toml` without exposing API keys
   - Different secrets per deployment (dev/staging/prod)
   - Credential rotation without touching main config

4. **Compile-Time vs Runtime**: `dawn.h` provides compile-time defaults only. All user-configurable settings should be in TOML files, with `dawn.h` values serving as fallbacks when config is missing.

### Configuration File Hierarchy

```
~/.config/dawn/          # User-specific (highest priority)
├── dawn.toml
└── secrets.toml

./                       # Project root (fallback)
├── dawn.toml
└── secrets.toml

/etc/dawn/               # System-wide (lowest priority, future)
├── dawn.toml
└── secrets.toml
```

Settings are merged with higher-priority files overriding lower ones.

### Configuration Files

#### dawn.toml (Runtime Configuration)

Primary configuration file with sections for each subsystem:

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

#### secrets.toml (Credentials)

API keys and sensitive credentials (gitignored):

```toml
openai_api_key = "sk-..."
claude_api_key = "sk-ant-..."
gemini_api_key = "..."

[smartthings]
access_token = "..."
```

**Note**: Already in `.gitignore` - never commit API keys!

#### dawn.h (Compile-Time Defaults)

Fallback values when config files are missing:

- `AI_NAME`: Default wake word ("friday")
- `AI_DESCRIPTION`: System prompt for LLM
- `DEFAULT_PCM_PLAYBACK_DEVICE`: ALSA playback device
- `DEFAULT_PCM_CAPTURE_DEVICE`: ALSA capture device
- `MQTT_IP` / `MQTT_PORT`: MQTT broker defaults

#### Tool Registry (`src/tools/*.c`)

Tools are defined as compile-time `tool_metadata_t` structs (JSON config file was removed):

```c
static const tool_metadata_t weather_metadata = {
   .name = "get_weather",
   .device_string = "weather",
   .description = "Get current weather for a location",
   .params = weather_params,
   .param_count = 1,
   .device_type = TOOL_DEVICE_TYPE_GETTER,
   .callback = weather_callback,
};
```

Register in `src/tools/tools_init.c` via `tools_register_all()`.

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

**Implementation Note**: When adding new settings to `dawn.toml`, also add corresponding entries to `SETTINGS_SCHEMA` to expose them in the WebUI, unless they fall under the exclusion criteria above.

---

## Future Improvements

### Completed

1. ✅ **Multi-Client Network Server** — Worker thread pool, per-client sessions, non-blocking main loop
2. ✅ **Conversation Persistence** — SQLite-backed conversation history with search/rename/delete
3. ✅ **DAP2 Satellite System** — Text-first WebSocket protocol, Tier 1 RPi satellite with SDL2 UI, Tier 2 ESP32 satellite
4. ✅ **Music Streaming to Satellites** — Opus audio over dedicated WebSocket, Goertzel FFT visualizer
5. ✅ **Document Search (RAG)** — Upload documents, semantic search + paginated reading via LLM tools, shared embedding engine, admin document management with global toggle
6. ✅ **CalDAV Calendar** — Multi-account, multi-calendar, RFC 4791, Google OAuth, RRULE expansion, background sync
7. ✅ **Email Integration** — IMAP/SMTP + Gmail REST API, multi-account, two-step send/trash, contacts system
8. ✅ **OAuth 2.0 + Crypto** — PKCE S256, encrypted token storage, Google provider, WebUI popup flow
9. ✅ **Scheduler** — Timers, alarms, reminders, scheduled tool execution, recurrence, chime audio, WebUI notifications
10. ✅ **Home Assistant** — REST API, 16 actions, fuzzy name matching, entity cache, WebUI admin panel
11. ✅ **Persistent Memory** — Entity graph, semantic embeddings, contacts, entity merge, confidence decay, import/export
12. ✅ **Per-User Settings** — Persona, location, timezone, units, theme. System prompt personalization per session.
13. ✅ **Plex Music Source** — Unified music DB with local + Plex, priority-based dedup, source abstraction
14. ✅ **Security Hardening** — HTTP security headers, pentest suite (34 tests), private CA for TLS
15. ✅ **Modular Tool Registry** — O(1) hash lookups, parallel execution, capability flags, compile-time feature guards
16. ✅ **Plan Executor** — Multi-step tool orchestration DSL (call, if, loop, set, log), safety controls, 130 unit tests

### Planned Features

1. **CI/CD Pipeline**
   - GitHub Actions for automated builds
   - Regression testing
   - Code quality checks

2. **Enhanced Testing**
   - Integration tests (end-to-end)
   - Stress tests (concurrent clients, memory leaks)
   - Automated regression tests

---

## References

- **Piper TTS**: https://github.com/rhasspy/piper
- **Vosk ASR**: https://alphacephei.com/vosk/
- **Whisper**: https://github.com/ggerganov/whisper.cpp
- **Silero VAD**: https://github.com/snakers4/silero-vad
- **llama.cpp**: https://github.com/ggerganov/llama.cpp
- **ONNX Runtime**: https://github.com/microsoft/onnxruntime

---

**Document Version**: 3.1
**Last Updated**: March 19, 2026
**Reorganization Commit**: [Git SHA to be added after commit]

### LLM Threading Architecture (Post-Interrupt Implementation)

**Status**: ✅ **Implemented** (November 2025)

D.A.W.N. now uses **non-blocking LLM processing** via a dedicated worker thread:

```
┌───────────────────────────────────────────────────────────┐
│                      Main Thread                          │
│                                                           │
│  - State machine (NEVER blocks on LLM)                    │
│  - Audio capture + VAD  (continuous, 50ms intervals)      │
│  - ASR processing (Whisper/Vosk)                          │
│  - TTS synthesis (mutex protected)                        │
│  - LLM completion detection (polling llm_processing flag) │
│  - MQTT communication                                     │
└────────────┬──────────────────────────────────────────────┘
             │
             │ Spawns on-demand, max 1 concurrent
             ↓
┌───────────────────────────────────────────────────────────┐
│                   LLM Worker Thread                       │
│                                                           │
│  - Blocking CURL call to LLM API                          │
│  - CURL progress callback (checks interrupt flag)         │
│  - Returns response via shared buffer                     │
│  - Thread-safe via llm_mutex                              │
└───────────────────────────────────────────────────────────┘
```

#### Key Components

- `pthread_t llm_thread` - Worker thread handle
- `pthread_mutex_t llm_mutex` - Protects shared request/response buffers
- `volatile int llm_processing` - Atomic flag: 1 = running, 0 = idle
- `volatile sig_atomic_t llm_interrupt_requested` - Signal-safe interrupt flag

#### Memory Ownership Transfer

Request and response buffers use **ownership transfer** to prevent data races:

```c
// Main thread → Worker thread:
llm_request_text = command_text;
command_text = NULL;  // Ownership transferred

// Worker thread → Main thread:
char *response = llm_response_text;
llm_response_text = NULL;  // Ownership transferred back
```

**Rules**:

- Worker thread **owns** request buffer, frees after use
- Main thread **owns** response buffer, frees after processing
- Mutex held only during transfer, not during processing

#### LLM Interrupt Mechanism

**Purpose**: Allow users to interrupt ongoing LLM requests by saying the wake word.

**Implementation**:

- CURL progress callback checks `llm_interrupt_requested` flag periodically
- Wake word detection in main loop sets flag via `llm_request_interrupt()`
- Returns non-zero from callback to abort CURL transfer
- Main thread detects interrupt, discards partial response, rolls back conversation history

**See**: `LLM_INTERRUPT_IMPLEMENTATION.md` for complete implementation details.

---

## Mutex Lock Ordering Hierarchy

**CRITICAL**: To prevent deadlocks, always acquire mutexes in this order when multiple locks needed:

```
Level 1 (acquire first):   metrics_mutex       (TUI metrics - future)
Level 2 (acquire second):   llm_mutex           (LLM thread communication)
                            tts_mutex           (TTS playback state)
                            processing_mutex    (Network processing state)
Level 3 (acquire last):     network_processing_mutex  (Network PCM buffer)
```

### Lock Ordering Rules

1. **Never acquire a lower-level lock while holding a higher-level lock**
   - ❌ BAD: Hold `tts_mutex` → acquire `metrics_mutex` (2→1 violates order)
   - ✅ GOOD: Acquire `metrics_mutex` → then `tts_mutex` (1→2 correct)

2. **Never hold multiple level-2 locks simultaneously**
   - ❌ BAD: Hold `llm_mutex` → acquire `tts_mutex` (both level-2)
   - ✅ GOOD: Release `llm_mutex` before acquiring `tts_mutex`

3. **Keep critical sections minimal**
   - Copy data, release lock, **then** process data
   - Avoid I/O operations while holding locks

4. **Prefer lock-free patterns for high-frequency updates**
   - VAD probability: Use C11 atomics instead of mutex (future)
   - State flags: Use `volatile` types for simple booleans

### Testing Lock Discipline

Use **ThreadSanitizer** during development:

```bash
cd build
cmake -DCMAKE_C_FLAGS="-fsanitize=thread -g" ..
make
./dawn
```

ThreadSanitizer detects:

- Data races (unsynchronized shared variable access)
- Lock order inversions (potential deadlocks)
- Use-after-free in threaded code

---

## Architectural Recommendations

### High Priority (Before TUI Implementation)

1. ✅ **Document lock ordering** (completed in this file)
2. **ncurses non-blocking mode**: Set `nodelay(stdscr, TRUE)` to prevent keyboard blocking
3. **ThreadSanitizer testing**: Run with `-fsanitize=thread` during TUI development

### Medium Priority (During TUI Implementation)

4. **Lock-free VAD probability**: Use C11 `atomic_uint` for high-frequency updates
5. **Batch metrics updates**: Acquire `metrics_mutex` once for multiple changes

### Low Priority (Post-TUI)

6. **Session layer for multi-client**: Design per-client conversation history isolation
7. **SIGSEGV crash stats**: Add signal handler to export metrics on crash (best-effort)

### Performance Characteristics (Updated)

| Component         | CPU Impact | Memory Impact | Notes                 |
| ----------------- | ---------- | ------------- | --------------------- |
| Main audio loop   | 15-20%     | Varies        | VAD + ASR processing  |
| LLM worker thread | 0.01%      | ~8KB          | CURL callback polling |
| TTS worker thread | 5-10%      | ~8KB          | During synthesis      |

**LLM Threading Benefit**: Main audio loop **never blocks** during LLM processing, maintaining responsive wake word detection even during 10-15 second LLM calls.

---

## Performance Optimization

### ASR Performance Tips

- Use Whisper **base** model (best accuracy/speed tradeoff on Jetson GPU)
- GPU acceleration is automatic on Jetson (CUDA)
- Adjust VAD sensitivity in `include/asr/vad_silero.h`

| Model | Jetson GPU RTF | Speed |
|-------|---------------|-------|
| Whisper tiny | 0.079 | 12.7x faster than real-time |
| Whisper base | 0.109 | 9.2x faster than real-time |
| Whisper small | 0.225 | 4.4x faster than real-time |

### LLM Performance Tips

- For local LLM: Use batch size 768 and context 1024 (critical for quality)
- Temperature, top-k, top-p have minimal effect on quality
- See `llm_testing/scripts/model_configs.conf` for optimal settings
- Cloud providers: ~2–4s response latency depending on provider and model
- Local (llama.cpp): ~100–200ms time-to-first-token on Jetson with quantized models

### Latency Reduction

- Streaming LLM + TTS reduces perceived latency to ~1.3s
- GPU acceleration provides 2–5x speedup on ASR
- Use Whisper tiny for fastest response (slight accuracy tradeoff)

---

## Design Decisions

### MCP (Model Context Protocol) Not Supported

DAWN does not implement MCP. While MCP has become an industry standard for connecting LLMs to external tools in composable applications, DAWN's architecture serves different goals:

- **Native C/C++ implementation**: DAWN is implemented entirely in C/C++ — a deliberate choice for reliability, deterministic timing, and single-binary deployment. MCP's ecosystem (SDKs, servers, tooling) is built around TypeScript and Python. No C implementation exists, and integrating one would require either writing an MCP client from scratch or embedding a managed runtime, negating the architectural benefits.

- **Voice-first responsiveness**: MCP's process-per-server model with JSON-RPC communication introduces latency and unpredictability. Voice assistants require sub-second response times with consistent behavior. DAWN's direct function calls and shared-memory architecture eliminate IPC overhead entirely.

- **Integrated tool system**: DAWN's native tool execution provides parallel thread-pool execution, automatic schema generation for multiple LLM providers (OpenAI, Claude, llama.cpp), session-scoped filtering, and built-in iterative tool loops. MCP defines a transport protocol — these capabilities remain the host's responsibility.

- **Self-contained design**: DAWN is a complete voice assistant, not a plugin framework. All tools are local (MQTT devices, system commands, media control, vision) with no architectural need for external server composition.

---
