# LLM Subsystem

Source: `src/llm/`, `include/llm/`. The sentence buffer (`common/src/utils/sentence_buffer.c`) lives in the common library because it is also used by Tier 1 satellites.

Part of the [D.A.W.N. architecture](../../../ARCHITECTURE.md) — see the main doc for layer rules, threading model, and lock ordering.

---

**Purpose**: Large Language Model integration with streaming support.

## Architecture Pattern: Strategy + Observer

- **Strategy**: multiple LLM providers (OpenAI, Claude, Gemini, OpenRouter, local) via a unified interface.
- **Observer**: streaming responses notify the sentence buffer for real-time TTS.

## Key Components

- **llm_interface.c/h**: LLM abstraction layer
   - `LLMContext` struct: provider-agnostic context
   - `llm_init()`: initialize selected provider
   - `llm_send_message()`: send message, get complete response (blocking)
   - `llm_send_message_streaming()`: send message, stream response chunks
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

- **`common/src/utils/sentence_buffer.c`, `common/include/utils/sentence_buffer.h`**: Sentence boundary detection (shared with satellite)
   - Buffers streaming text until complete sentence
   - Detects sentence boundaries (`.`, `!`, `?`), bullets, numbered lists, `:\n`, `\n\n`
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

## Prompt construction (two-segment, cache-aware)

The system prompt is assembled by `dawn_build_prompt()` (`src/webui/webui_auth_helpers.c:779`) into a `composed_prompt_t` (`include/core/session_manager.h`) with exactly two fields:

- **`stable_prefix`** — persona, rules, identity, memory, and surface context. Byte-identical across turns unless settings change. The Anthropic `cache_control: ephemeral` breakpoint attaches here (`src/llm/llm_claude_format.c:987`).
- **`volatile_block`** — per-turn retrievals (`[system_time]` + ranked focus candidates). Rebuilt every turn; never cache-eligible.

This split (commit `e4dc72f`, "cache the system prefix end-to-end across providers") lets the bulk of the prompt hit the provider cache while per-turn context refreshes for free. Session-stable content — USER MEMORY preferences + recent summaries — deliberately lives in the stable prefix, not the volatile block, so it costs nothing per turn after the first cache write.

> File:line references below are in `src/webui/webui_auth_helpers.c` unless another file is named.

### Segment order

The stable prefix is built first (`build_stable_segment`, `:435`), **then** satellite/messaging context is appended in `dawn_build_prompt` (`:812`–`815`), **then** the drift hash is taken — so the surface-context block lands *after* the tool-call footer, not before it.

**Stable prefix** (`messages[0]`, cached):

1. **Persona** — `get_persona_description()` (`src/llm/llm_command_parser.c`).
2. **System instructions** — `get_system_instructions(true)`: core rules + feature rules (vision, search, weather, …).
3. **TOOL DEFAULTS** — localization fallback (location/room/units/tz, `TOOL_DEFAULTS_HEADER_TEXT`). Emitted only for **unauthenticated** callers; `strip_tool_defaults()` (`:301`) removes it for authenticated users because the User Context block supersedes it.
4. **User Context** — persona traits + location + timezone + units from `auth_db_get_user_settings()`. "append" mode adds a `## User Context` block; "replace" mode substitutes a custom persona (`build_stable_segment`, `:435`–`559`).
5. **User Identity** — `## User Identity` (real name, preferred address, aliases) when set; no-ops otherwise (`build_identity_block`, `:139`).
6. **USER MEMORY** — preferences + recent conversation summaries via `memory_build_context()` (`src/memory/memory_context.c`); no-ops when empty.
7. **Memory instructions footer** — `k_memory_instructions_footer` (`:255`); appended only when a memory body was emitted, so its "the above is only a summary" referent stays valid.
8. **Tool-call discipline footer** — `k_tool_call_discipline_footer` (`:274`); always emitted (no auth/memory gate). See below.
9. **Surface context** — exactly one of: DAP2 satellite `Room=` / `HomeAssistant_Area=` (`append_satellite_context_to_stable`, `:727`) **or** messaging provider/channel (`append_messaging_context_to_stable`, `:648`). Mutually exclusive by session type; appended after the base segment (`:812`–`815`).

— *cache boundary; drift hash computed here* —

**Volatile block** (`messages[1]`, never cached) — `build_volatile_segment()` (`:582`) wraps `build_focus_block()` (`src/webui/build_focus_block.c:246`):

10. `--- TURN CONTEXT ---` framing header.
11. **`[system_time]`** — fresh `time()`-derived line, prepended when a dispatch session is present and the focus result is non-empty (`build_focus_block.c:369`). Lives here, not in the cached prefix, so the cache key doesn't churn across day boundaries.
12. **Ranked focus candidates** — `[<source_id>] <text>` lines: memory facts/entities/relations/summaries, calendar events, document chunks, after per-session dedup.
13. `--- END TURN CONTEXT ---` framing footer.

### Drift hash and the cache boundary

`session_update_system_messages()` (`src/core/session_manager.c:1685`) pushes both segments into the conversation history and computes an FNV-1a hash of the stable prefix (`:1718`). If the hash changes mid-session it logs a cache-invalidation warning. **This is why satellite/messaging context is appended inside `dawn_build_prompt` before this point** (`:803`–`807`): a mid-session room or HA-area change would otherwise silently bust the Anthropic cache with no drift-log signal.

### Tool-call discipline footer

`k_tool_call_discipline_footer` (`:274`, appended via `append_tool_discipline_footer`, `:383`) is a **universal** anti-bluff rule: when a reply commits to an action ("I'll search…", "I'll send…", "let me look that up"), the corresponding tool call must be in the **same turn** — not promised for later, not narrated as if it already happened. Aspirational offers ("if you'd like, I can…") don't require a call until the user accepts. It applies to every action-bearing tool (scheduler, search, url_fetch, email, calendar, memory, messaging, home_assistant, music, weather, …); the originating failure was a verbal "I've scheduled that" with no `scheduler.create` call. It always emits and lives in the cached prefix, so it costs nothing per turn. The scheduler tool descriptor carries its own louder "NO VERBAL COMMITMENTS" clause; this footer is the general-purpose version for every other tool. (Messaging-surface context for this rule is described in `docs/MESSAGING_CHANNELS_DESIGN.md` §10.5.)

### Provider handling

Assembly is provider-agnostic — the two-segment `composed_prompt_t` is serialized per provider downstream: Claude attaches `cache_control` to the first system message (plus a second breakpoint on the final tool schema, `src/llm/llm_claude_format.c`), the OpenAI Responses API concatenates the two segments, and Chat Completions tolerates two consecutive system messages. Commit `e4dc72f` also unified cache-token accounting across providers (Claude `cache_creation`/`cache_read` tokens, Responses API usage struct). Gemini caching is documented as unreliable upstream — see the Gemini native-API notes in `docs/TODO.md`.

## OpenRouter Gateway Mode

OpenRouter (`https://openrouter.ai/api/v1`) is an OpenAI-wire-compatible gateway fronting
many vendors' models. DAWN integrates it as a **gateway toggle**, not a fourth sibling
provider: `[llm.cloud] use_openrouter = true` (key `openrouter_api_key` in `secrets.toml`,
or `OPENROUTER_API_KEY` env) routes **all** cloud traffic — the main chat path AND the
auxiliary memory-extraction / compaction / silent-observe / scheduler calls — through
OpenRouter with one key, regardless of the `provider` setting. Because OpenRouter is
OpenAI-compatible for every model it serves (including Anthropic ones), all calls reuse the
existing `llm_openai_*` request/SSE path (`CLOUD_PROVIDER_OPENROUTER` falls into the
OpenAI-compatible branch everywhere, never the native Claude path).

**Single-authority rule (avoids inconsistent state):** exactly one place converts the
`use_openrouter` bool → the `CLOUD_PROVIDER_OPENROUTER` enum — the gateway short-circuit at
the top of `llm_init()` / `llm_refresh_providers()` (and `llm_get_default_config()` for
session defaults). Every other site treats the enum as ground truth and never re-reads the
bool to decide the provider. The string-keyed auxiliary resolvers (compaction in
`llm_context.c`, extraction in `memory_extraction.c`, silent-observe, scheduler) call the
shared helper `llm_apply_openrouter_gateway()`, which rewrites their resolved
provider/endpoint/api_key to OpenRouter while preserving the configured model string — so
`compact_model` / silent-observe model / `memory.extraction_model` must be OpenRouter
`vendor/model` IDs when the gateway is on. The scheduler picks from per-provider model lists,
so it selects the OpenRouter default model explicitly instead.

Other gateway specifics: OpenRouter never routes to `/v1/responses`
(`should_dispatch_to_responses_api` skips `openrouter.ai`); requests carry optional
`HTTP-Referer` + `X-Title` attribution headers; gateway-on-but-no-key falls back to local
rather than issuing a NULL-key request; and `get_context_size()` best-effort-strips the
`vendor/` prefix to reuse the known context-window tables, else a conservative 128K default.

> **Tech debt note:** OpenRouter adds a seventh per-provider→(URL, key) branch ladder across
> the resolvers (`llm_resolve_config`, `llm_chat_completion_with_config`,
> `build_compaction_config`, `memory_extraction_resolve_config`, silent-observe, scheduler).
> Consolidating these into one resolver is a worthwhile future cleanup.

## LLM Worker Thread

LLM processing is non-blocking — the main audio loop never waits on an API call.

```
┌───────────────────────────────────────────────────────────┐
│                      Main Thread                          │
│  - State machine (never blocks on LLM)                    │
│  - Audio capture + VAD (continuous, 50ms intervals)       │
│  - ASR processing (Whisper/Vosk)                          │
│  - TTS synthesis (mutex protected)                        │
│  - LLM completion detection (polling llm_processing flag) │
└────────────┬──────────────────────────────────────────────┘
             │ spawns on-demand, max 1 concurrent
             ▼
┌───────────────────────────────────────────────────────────┐
│                   LLM Worker Thread                       │
│  - Blocking CURL call to LLM API                          │
│  - CURL progress callback (checks interrupt flag)         │
│  - Returns response via shared buffer                     │
│  - Thread-safe via llm_mutex                              │
└───────────────────────────────────────────────────────────┘
```

Request and response buffers use **ownership transfer** to prevent data races. The mutex is held only during transfer, not during processing.

### Interrupt Mechanism

Users can interrupt an in-flight LLM call by saying the wake word. The CURL progress callback checks `llm_interrupt_requested` periodically; wake word detection sets the flag via `llm_request_interrupt()`, returning non-zero from the callback aborts the transfer, and the main thread discards the partial response and rolls back conversation history.

## Data Flow (Streaming Mode)

```
User Query → LLM Provider (OpenAI/Claude/Local)
                    ↓ (SSE stream)
            SSE Parser → Streaming Handler
                    ↓ (text chunks)
            Sentence Buffer → TTS (as sentences complete)
                    ↓ (complete response)
            Command Parser → MQTT Commands
```

## Performance Comparison

| Provider                | Quality | TTFT      | Latency | Cost          |
| ----------------------- | ------- | --------- | ------- | ------------- |
| OpenAI GPT-5            | 100%    | ~300ms    | ~3.1s   | ~$0.01/query  |
| Claude 4.6 Sonnet       | 92.4%   | ~400ms    | ~3.5s   | ~$0.015/query |
| Gemini 2.5 Flash        | ~90%    | ~250ms    | ~2.5s   | ~$0.002/query |
| llama.cpp (Qwen3-4B Q4) | 81.9%   | 116-138ms | ~1.5s   | FREE          |
| Ollama (Qwen3-4B Q4)    | 81.9%   | ~150ms    | ~1.6s   | FREE          |

**TTFT = Time To First Token** (lower = faster perceived response).
