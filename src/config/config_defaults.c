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
 * DAWN Configuration System - Default value initialization
 *
 * All default values match the compile-time constants in dawn.h and dawn.c
 */

#include <stdlib.h>
#include <string.h>

#include "config/dawn_config.h"

/* =============================================================================
 * Global Configuration Instances
 * ============================================================================= */
dawn_config_t g_config;
secrets_config_t g_secrets;

/* =============================================================================
 * Helper Macros
 * ============================================================================= */
#define SAFE_COPY(dst, src)                   \
   do {                                       \
      strncpy((dst), (src), sizeof(dst) - 1); \
      (dst)[sizeof(dst) - 1] = '\0';          \
   } while (0)

/* =============================================================================
 * Default Values - matching dawn.h and dawn.c
 * ============================================================================= */
void config_set_defaults(dawn_config_t *config) {
   if (!config)
      return;

   memset(config, 0, sizeof(*config));

   /* General */
   SAFE_COPY(config->general.ai_name, "friday");
   config->general.log_file[0] = '\0'; /* Empty = stdout */

   /* Persona - empty means use compile-time default from dawn.h */
   config->persona.description[0] = '\0';

   /* Localization */
   config->localization.location[0] = '\0'; /* No default location */
   config->localization.timezone[0] = '\0'; /* System default */
   SAFE_COPY(config->localization.units, "imperial");

   /* Audio */
   SAFE_COPY(config->audio.backend, "auto");
   SAFE_COPY(config->audio.capture_device, "default");
   SAFE_COPY(config->audio.playback_device, "default");
   config->audio.output_rate = 44100; /* CD quality, native for most music */
   config->audio.output_channels = 2; /* Stereo required for dmix compatibility */

   /* Audio barge-in */
   config->audio.bargein.enabled = true;
   config->audio.bargein.cooldown_ms = 1500;        /* VAD_TTS_COOLDOWN_MS */
   config->audio.bargein.startup_cooldown_ms = 300; /* VAD_TTS_STARTUP_COOLDOWN_MS */

   /* Audio named devices - empty by default (optional, configured per-user) */
   config->audio.named_device_count = 0;

   /* VAD - matching defines in dawn.c */
   config->vad.speech_threshold = 0.5f;        /* VAD_SPEECH_THRESHOLD */
   config->vad.speech_threshold_tts = 0.92f;   /* VAD_SPEECH_THRESHOLD_TTS */
   config->vad.silence_threshold = 0.3f;       /* VAD_SILENCE_THRESHOLD */
   config->vad.end_of_speech_duration = 1.2f;  /* VAD_END_OF_SPEECH_DURATION */
   config->vad.max_recording_duration = 30.0f; /* VAD_MAX_RECORDING_DURATION */
   config->vad.preroll_ms = 500;

   /* VAD Chunking - matching defines in dawn.c */
   config->vad.chunking.enabled = true;
   config->vad.chunking.pause_duration = 0.3f; /* VAD_CHUNK_PAUSE_DURATION */
   config->vad.chunking.min_duration = 1.0f;   /* VAD_MIN_CHUNK_DURATION */
   config->vad.chunking.max_duration = 10.0f;  /* VAD_MAX_CHUNK_DURATION */

   /* ASR */
   SAFE_COPY(config->asr.model, "base.en");
   SAFE_COPY(config->asr.models_path, "models/whisper.cpp");

   /* TTS */
   SAFE_COPY(config->tts.models_path, "models");
   SAFE_COPY(config->tts.voice_model, "en_GB-alba-medium");
   config->tts.length_scale = 0.85f;

   /* Commands */
   SAFE_COPY(config->commands.processing_mode, "direct_first");

   /* LLM */
   SAFE_COPY(config->llm.type, "cloud");
   config->llm.max_tokens = 8192; /* Higher default for SVG tool output */

   /* LLM Cloud */
   config->llm.cloud.provider[0] = '\0'; /* Empty = auto-detect from available API keys */
   config->llm.cloud.endpoint[0] = '\0'; /* Empty = use default */
   config->llm.cloud.vision_enabled = true;

   /* Default OpenAI model list (first entry is default) */
   config->llm.cloud.openai_models_count = 4;
   SAFE_COPY(config->llm.cloud.openai_models[0], LLM_DEFAULT_OPENAI_MODEL);
   SAFE_COPY(config->llm.cloud.openai_models[1], "gpt-5-mini");
   SAFE_COPY(config->llm.cloud.openai_models[2], "gpt-5-nano");
   SAFE_COPY(config->llm.cloud.openai_models[3], "o4-mini");
   config->llm.cloud.openai_default_model_idx = 0;

   /* Default Claude model list (first entry is default) */
   config->llm.cloud.claude_models_count = 3;
   SAFE_COPY(config->llm.cloud.claude_models[0], LLM_DEFAULT_CLAUDE_MODEL);
   SAFE_COPY(config->llm.cloud.claude_models[1], "claude-opus-4-6");
   SAFE_COPY(config->llm.cloud.claude_models[2], "claude-haiku-4-5");
   config->llm.cloud.claude_default_model_idx = 0;

   /* Default Gemini model list (first entry is default) */
   config->llm.cloud.gemini_models_count = 5;
   SAFE_COPY(config->llm.cloud.gemini_models[0], LLM_DEFAULT_GEMINI_MODEL);
   SAFE_COPY(config->llm.cloud.gemini_models[1], "gemini-2.5-pro");
   SAFE_COPY(config->llm.cloud.gemini_models[2], "gemini-2.5-flash-lite");
   SAFE_COPY(config->llm.cloud.gemini_models[3], "gemini-3-flash-preview");
   SAFE_COPY(config->llm.cloud.gemini_models[4], "gemini-3-pro-preview");
   config->llm.cloud.gemini_default_model_idx = 0;

   /* LLM Local */
   SAFE_COPY(config->llm.local.endpoint, "http://127.0.0.1:8080");
   config->llm.local.model[0] = '\0';             /* Server decides */
   config->llm.local.vision_enabled = false;      /* Most local models don't support vision */
   SAFE_COPY(config->llm.local.provider, "auto"); /* Auto-detect Ollama vs llama.cpp */

   /* LLM Tools */
   SAFE_COPY(config->llm.tools.mode, "native"); /* "native", "command_tags", or "disabled" */

   /* LLM Thinking/Reasoning */
   SAFE_COPY(config->llm.thinking.mode, "disabled");           /* "disabled", "enabled", "auto" */
   SAFE_COPY(config->llm.thinking.reasoning_effort, "medium"); /* Controls budget via dropdown */
   config->llm.thinking.budget_low = LLM_THINKING_BUDGET_LOW_DEFAULT;
   config->llm.thinking.budget_medium = LLM_THINKING_BUDGET_MEDIUM_DEFAULT;
   config->llm.thinking.budget_high = LLM_THINKING_BUDGET_HIGH_DEFAULT;

   /* LLM Context Management */
   config->llm.summarize_threshold = 0.80f;  /* Compact at 80% of context limit */
   config->llm.conversation_logging = false; /* Disabled: WebUI saves to DB, set true for debug */
   config->llm.rate_limit_enabled = true;    /* Throttle cloud API calls by default */
   config->llm.rate_limit_rpm = 40;          /* 20% headroom under typical 50 RPM limit */

   /* Search */
   SAFE_COPY(config->search.engine, "searxng");
   SAFE_COPY(config->search.endpoint, "http://127.0.0.1:8384");

   /* Search Summarizer */
   SAFE_COPY(config->search.summarizer.backend, "tfidf"); /* Fast local extractive summarization */
   config->search.summarizer.threshold_bytes = 4096;
   config->search.summarizer.target_words = 600;
   config->search.summarizer.target_ratio = 0.2f; /* Keep 20% of sentences for TF-IDF */

   /* Search Title Filters - exclude low-quality SEO spam from news results */
   SAFE_COPY(config->search.title_filters[0], "wordle");
   SAFE_COPY(config->search.title_filters[1], "connections hints");
   SAFE_COPY(config->search.title_filters[2], "connections answers");
   SAFE_COPY(config->search.title_filters[3], "nyt connections");
   SAFE_COPY(config->search.title_filters[4], "puzzle hints");
   SAFE_COPY(config->search.title_filters[5], "puzzle answers");
   config->search.title_filters_count = 6;

   /* URL Fetcher - whitelist is zeroed by memset */
   config->url_fetcher.whitelist_count = 0;

   /* FlareSolverr */
   config->url_fetcher.flaresolverr.enabled = false;
   SAFE_COPY(config->url_fetcher.flaresolverr.endpoint, "http://127.0.0.1:8191/v1");
   config->url_fetcher.flaresolverr.timeout_sec = 10;
   config->url_fetcher.flaresolverr.max_response_bytes = 4 * 1024 * 1024; /* 4MB */

   /* MQTT - matching dawn.h */
   config->mqtt.enabled = true;
   SAFE_COPY(config->mqtt.broker, "127.0.0.1"); /* MQTT_IP */
   config->mqtt.port = 1883;                    /* MQTT_PORT */
   config->mqtt.tls = false;
   config->mqtt.tls_ca_cert[0] = '\0';
   config->mqtt.tls_cert_path[0] = '\0';
   config->mqtt.tls_key_path[0] = '\0';

   /* Network */
   config->network.workers = 2;
   config->network.session_timeout_sec = 1800;         // 30 minutes
   config->network.llm_timeout_ms = 60000;             // 60 seconds for LLM requests
   config->network.summarization_timeout_ms = 180000;  // 3 minutes for context summarization

   /* TUI */
   config->tui.enabled = false;

   /* WebUI */
   config->webui.enabled = false;
   config->webui.port = 3000; /* "I love you 3000" */
   config->webui.max_clients = 4;
   config->webui.audio_chunk_ms = 100; /* 100ms chunks for lower VAD latency */
   SAFE_COPY(config->webui.www_path, "www");
   SAFE_COPY(config->webui.bind_address, "0.0.0.0");
   config->webui.https = false;
   config->webui.ssl_cert_path[0] = '\0';
   config->webui.ssl_key_path[0] = '\0';
   config->webui.export_max_messages = 5000;
   SAFE_COPY(config->webui.export_format, "json");

   /* Images - storage settings for vision uploads */
   config->images.retention_days = 0; /* 0 = never delete (user preference) */
   config->images.max_size_mb = 4;    /* 4MB max per image */
   config->images.max_per_user = 1000;
   config->images.cache_size_mb = 200; /* 200MB LRU cache cap */

   /* Documents - upload and extraction limits */
   config->documents.max_file_size_kb = 512;
   config->documents.max_documents = 5;
   config->documents.max_pages = 100;
   config->documents.max_extracted_size_kb = 1024;
   config->documents.max_index_size_kb = 2048;
   config->documents.max_indexed_documents = 50;

   /* Vision - per-upload image size and dimension limits */
   config->vision.max_image_size_kb = 4096;
   config->vision.max_dimension = 1024;
   config->vision.max_images = 5;

   /* Memory - persistent user memory system */
   config->memory.enabled = true;
   config->memory.context_budget_tokens = 800; /* ~3200 chars for memory context */
   SAFE_COPY(config->memory.extraction_provider, "local");
   SAFE_COPY(config->memory.extraction_model, "qwen2.5:7b");
   config->memory.extraction_timeout_ms = 120000; /* 2 minutes for fact extraction */
   config->memory.pruning_enabled = true;
   config->memory.prune_superseded_days = 30; /* Delete old superseded facts after 30 days */
   config->memory.prune_stale_days = 180; /* Delete unused low-confidence facts after 6 months */
   config->memory.prune_stale_min_confidence = 0.5f; /* Only prune facts below 50% confidence */
   config->memory.conversation_idle_timeout_min =
       15;                                   /* Auto-save voice conversations after 15 min */
   config->memory.default_voice_user_id = 1; /* Assign to first user (admin) by default */

   /* Memory decay (Phase 5) */
   config->memory.decay_enabled = true;
   config->memory.decay_hour = 2;                     /* Run at 2 AM */
   config->memory.decay_inferred_weekly = 0.95f;      /* Inferred facts lose 5%/week */
   config->memory.decay_explicit_weekly = 0.98f;      /* Explicit facts lose 2%/week */
   config->memory.decay_preference_weekly = 0.97f;    /* Preferences lose 3%/week */
   config->memory.decay_inferred_floor = 0.0f;        /* Inferred can decay to zero */
   config->memory.decay_explicit_floor = 0.50f;       /* Explicit never below 50% */
   config->memory.decay_preference_floor = 0.40f;     /* Preferences never below 40% */
   config->memory.decay_prune_threshold = 0.25f;      /* Prune facts below 25% */
   config->memory.summary_retention_days = 30;        /* Delete summaries after 30 days */
   config->memory.access_reinforcement_boost = 0.05f; /* +5% on access */

   /* Memory embeddings (semantic search) */
   SAFE_COPY(config->memory.embedding_provider, "onnx");
   /* embedding_model, embedding_endpoint: empty (memset zero) — ONNX needs neither */
   config->memory.embedding_keyword_weight = 0.3f;
   config->memory.embedding_vector_weight = 0.7f;
   config->memory.embedding_backfill_on_startup = true;
   /* Temporal scoring: +2.3pp R@10 / +0.8pp R@5 on LongMemEval temporal-reasoning
    * at weight=0.20; plateau at 0.30. Zero cost on queries without temporal
    * expressions (parser returns "not found", no boost applied).  Safe default. */
   config->memory.temporal_weight = 0.20f;

   /* Shutdown - disabled by default for security */
   config->shutdown.enabled = false;
   config->shutdown.passphrase[0] = '\0';

   /* Debug */
   config->debug.mic_record = false;
   config->debug.asr_record = false;
   config->debug.aec_record = false;
   SAFE_COPY(config->debug.record_path, "/tmp");

   /* Paths */
   SAFE_COPY(config->paths.data_dir, "~/.local/share/dawn"); /* Database storage directory */
   SAFE_COPY(config->paths.music_dir, "~/Music");

   /* Music - metadata indexing */
   config->music.scan_interval_minutes = 60; /* Rescan every hour */

   /* Music - Plex Media Server defaults */
   config->music.plex.host[0] = '\0';       /* Must be configured by user */
   config->music.plex.port = 32400;         /* Standard Plex port */
   config->music.plex.music_section_id = 0; /* 0 = auto-discover */
   config->music.plex.ssl = false;
   config->music.plex.ssl_verify = true;
   config->music.plex.client_identifier[0] = '\0'; /* Auto-generated on first use */

   /* Scheduler */
   config->scheduler.enabled = true;
   config->scheduler.default_snooze_minutes = 10;
   config->scheduler.max_snooze_count = 10;
   config->scheduler.max_events_per_user = 50;
   config->scheduler.max_events_total = 200;
   config->scheduler.missed_event_recovery = true;
   SAFE_COPY(config->scheduler.missed_task_policy, "skip");
   config->scheduler.missed_task_max_age_sec = 300;
   config->scheduler.alarm_timeout_sec = 60;
   config->scheduler.alarm_volume = 80;
   config->scheduler.event_retention_days = 30;

   /* Calendar */
   config->calendar.enabled = true;
   config->calendar.sync_interval_sec = 900;
   config->calendar.cache_past_days = 30;
   config->calendar.cache_future_days = 365;
   config->calendar.default_event_duration_min = 60;

   /* Music - streaming settings */
   config->music.streaming_enabled = true;
   strncpy(config->music.streaming_quality, "standard",
           sizeof(config->music.streaming_quality) - 1);
   strncpy(config->music.streaming_bitrate_mode, "vbr",
           sizeof(config->music.streaming_bitrate_mode) - 1);
}

void config_set_secrets_defaults(secrets_config_t *secrets) {
   if (!secrets)
      return;

   memset(secrets, 0, sizeof(*secrets));
   /* All secrets default to empty strings */
}

const dawn_config_t *config_get(void) {
   return &g_config;
}

const secrets_config_t *config_get_secrets(void) {
   return &g_secrets;
}

void config_cleanup(void) {
   /* Currently a no-op - all config uses static allocation */
   /* Reserved for future use if dynamic resources are added */
   (void)0;
}
