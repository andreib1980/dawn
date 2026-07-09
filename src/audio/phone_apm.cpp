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
 * the project author(s).
 *
 * phone_apm — near-end (uplink) audio processor for phone-call audio.
 *
 * Wraps one WebRTC Audio Processing Module (APM) instance configured for 16 kHz
 * mono: high-pass -> noise-suppression -> AGC2 (VAD-gated digital gain) ->
 * limiter, cleaning the local mic before it is written to the modem's raw USB
 * PCM tap (no modem-side gain touches that tap, so all level control is here).
 *
 * The whole TU is compiled unconditionally; the WebRTC machinery is fenced
 * behind AEC_BACKEND_WEBRTC so a build without WebRTC APM still links, with
 * phone_apm_create() returning NULL (caller falls back to a raw gain).  This is
 * a separate instance from src/audio/aec_webrtc.cpp — required, because that
 * 48 kHz singleton runs concurrently on the wake-word capture thread while this
 * runs on the phone bridge thread.
 */

#include "audio/phone_apm.h"

#include <algorithm>

/* Default near-end config (design §3.3) — starting points, tuned by ear on a
 * live 2-party call.  Named constants, not literals (no magic numbers). */
#define PHONE_APM_DEF_AGC_ENABLED true
#define PHONE_APM_DEF_FIXED_GAIN_DB 6.0f
#define PHONE_APM_DEF_RAMP_DB_PER_S 3.0f
#define PHONE_APM_DEF_MAX_NOISE_DBFS (-50.0f)
#define PHONE_APM_DEF_NS_LEVEL PHONE_NS_MODERATE
#define PHONE_APM_DEF_HIGH_PASS true
#define PHONE_APM_DEF_ECHO_CANCEL false

/* Vendored WebRTC v1.3 GainController2::Validate() requires fixed_digital.gain_db
 * in [0, 50): a value >=50 FAILS validation and ApplyConfig() silently reverts
 * the ENTIRE gain_controller2 block to defaults (enabled=false), disabling AGC2.
 * So clamp strictly below 50 (49.0 leaves margin). */
#define PHONE_APM_FIXED_GAIN_MIN 0.0f
#define PHONE_APM_FIXED_GAIN_MAX 49.0f
/* Adaptive knobs WebRTC does NOT validate — clamp to sane ranges ourselves so a
 * typo in dawn.toml can't feed a garbage value into the gain applier. */
#define PHONE_APM_RAMP_MIN 0.1f
#define PHONE_APM_RAMP_MAX 100.0f
#define PHONE_APM_NOISE_DBFS_MIN (-90.0f)
#define PHONE_APM_NOISE_DBFS_MAX 0.0f

/* phone_apm_default_config() is pure data and must exist in both builds. */
extern "C" phone_apm_config_t phone_apm_default_config(void) {
   phone_apm_config_t c;
   c.agc_enabled = PHONE_APM_DEF_AGC_ENABLED;
   c.fixed_gain_db = PHONE_APM_DEF_FIXED_GAIN_DB;
   c.max_gain_change_db_per_s = PHONE_APM_DEF_RAMP_DB_PER_S;
   c.max_output_noise_dbfs = PHONE_APM_DEF_MAX_NOISE_DBFS;
   c.ns_level = PHONE_APM_DEF_NS_LEVEL;
   c.high_pass = PHONE_APM_DEF_HIGH_PASS;
   c.echo_cancel = PHONE_APM_DEF_ECHO_CANCEL;
   return c;
}

#ifdef AEC_BACKEND_WEBRTC

/* WebRTC build configuration — must precede the WebRTC headers. */
#ifndef WEBRTC_POSIX
#define WEBRTC_POSIX 1
#endif
#ifndef WEBRTC_APM_DEBUG_DUMP
#define WEBRTC_APM_DEBUG_DUMP 0
#endif

#include <webrtc/modules/audio_processing/include/audio_processing.h>

extern "C" {
#include "logging.h"
}

/* One APM instance + a cached StreamConfig (built once, not per frame). */
struct phone_apm {
   webrtc::AudioProcessing *apm = nullptr;
   webrtc::StreamConfig stream_config{ PHONE_APM_RATE, 1 };
};

static webrtc::AudioProcessing::Config::NoiseSuppression::Level map_ns_level(phone_ns_level_t lvl) {
   using NS = webrtc::AudioProcessing::Config::NoiseSuppression;
   switch (lvl) {
      case PHONE_NS_LOW:
         return NS::kLow;
      case PHONE_NS_HIGH:
         return NS::kHigh;
      case PHONE_NS_VERYHIGH:
         return NS::kVeryHigh;
      case PHONE_NS_MODERATE:
      default:
         return NS::kModerate;
   }
}

extern "C" phone_apm_t *phone_apm_create(const phone_apm_config_t *cfg) {
   if (cfg == nullptr) {
      return nullptr;
   }

   phone_apm_t *a = new (std::nothrow) phone_apm();
   if (a == nullptr) {
      OLOG_ERROR("phone_apm: allocation failed");
      return nullptr;
   }

   webrtc::AudioProcessingBuilder builder;
   a->apm = builder.Create(); /* default echo factory — no custom AEC3 factory */
   if (a->apm == nullptr) {
      OLOG_ERROR("phone_apm: failed to create AudioProcessing instance");
      delete a;
      return nullptr;
   }

   /* All four streams at 16 kHz mono (near-end + reverse, in + out). */
   webrtc::ProcessingConfig pc;
   pc.input_stream().set_sample_rate_hz(PHONE_APM_RATE);
   pc.input_stream().set_num_channels(1);
   pc.output_stream().set_sample_rate_hz(PHONE_APM_RATE);
   pc.output_stream().set_num_channels(1);
   pc.reverse_input_stream().set_sample_rate_hz(PHONE_APM_RATE);
   pc.reverse_input_stream().set_num_channels(1);
   pc.reverse_output_stream().set_sample_rate_hz(PHONE_APM_RATE);
   pc.reverse_output_stream().set_num_channels(1);
   if (a->apm->Initialize(pc) != 0) {
      OLOG_ERROR("phone_apm: Initialize at %dHz failed", PHONE_APM_RATE);
      delete a->apm;
      delete a;
      return nullptr;
   }

   webrtc::AudioProcessing::Config apm_config;

   /* Echo canceller: off in Phase 1 (reverse stream is fed "dark" so enabling
    * this is a config flip).  When enabled use mobile_mode = AECM — AEC3 does
    * NOT cancel at 16 kHz (see aec_webrtc.cpp:82-90), AECM is the narrowband
    * canceller built for this rate and is more robust to delay error. */
   apm_config.echo_canceller.enabled = cfg->echo_cancel;
   apm_config.echo_canceller.mobile_mode = true;

   apm_config.high_pass_filter.enabled = cfg->high_pass;

   apm_config.noise_suppression.enabled = (cfg->ns_level != PHONE_NS_OFF);
   apm_config.noise_suppression.level = map_ns_level(cfg->ns_level);

   /* AGC2 (per-frame, VAD-gated digital gain) — the fix for the "threshold-y"
    * pumping; AGC1 stays off.  All three knobs are clamped: an out-of-range
    * fixed gain would silently disable the whole AGC2 block (see the MAX comment
    * above), and the adaptive knobs are unvalidated by WebRTC. */
   float fixed_gain = std::min(std::max(cfg->fixed_gain_db, PHONE_APM_FIXED_GAIN_MIN),
                               PHONE_APM_FIXED_GAIN_MAX);
   if (fixed_gain != cfg->fixed_gain_db) {
      OLOG_WARNING("phone_apm: fixed_gain_db %.1f out of range [%.0f,%.0f) — clamped to %.1f",
                   (double)cfg->fixed_gain_db, (double)PHONE_APM_FIXED_GAIN_MIN, 50.0,
                   (double)fixed_gain);
   }
   float ramp = std::min(std::max(cfg->max_gain_change_db_per_s, PHONE_APM_RAMP_MIN),
                         PHONE_APM_RAMP_MAX);
   float noise_floor = std::min(std::max(cfg->max_output_noise_dbfs, PHONE_APM_NOISE_DBFS_MIN),
                                PHONE_APM_NOISE_DBFS_MAX);

   apm_config.gain_controller1.enabled = false;
   apm_config.gain_controller2.enabled = cfg->agc_enabled;
   apm_config.gain_controller2.fixed_digital.gain_db = fixed_gain;
   apm_config.gain_controller2.adaptive_digital.enabled = cfg->agc_enabled;
   apm_config.gain_controller2.adaptive_digital.max_gain_change_db_per_second = ramp;
   apm_config.gain_controller2.adaptive_digital.max_output_noise_level_dbfs = noise_floor;

   /* Near-free; exposes output_rms_dbfs for the objective level trace. */
   apm_config.level_estimation.enabled = true;

   a->apm->ApplyConfig(apm_config);

   OLOG_INFO("phone_apm: 16 kHz near-end active (agc=%d ns=%d hpf=%d aec=%d)", cfg->agc_enabled,
             (int)cfg->ns_level, cfg->high_pass, cfg->echo_cancel);
   return a;
}

extern "C" void phone_apm_reverse_10ms(phone_apm_t *a, const int16_t *frame160) {
   if (a == nullptr || a->apm == nullptr || frame160 == nullptr) {
      return;
   }
   /* The int16 ProcessReverseStream takes a const src but writes the render
    * pass-through to dst; give it a throwaway scratch dst so the caller's
    * reference buffer is never mutated (keeps @p frame160 honestly const). */
   int16_t scratch[PHONE_APM_FRAME];
   a->apm->ProcessReverseStream(frame160, a->stream_config, a->stream_config, scratch);
}

extern "C" void phone_apm_process_10ms(phone_apm_t *a, int16_t *frame160) {
   if (a == nullptr || a->apm == nullptr || frame160 == nullptr) {
      return;
   }
   /* In-place is safe: the int16 ProcessStream copies src into an internal
    * AudioBuffer before writing dst (verified against vendored v1.3). */
   a->apm->ProcessStream(frame160, a->stream_config, a->stream_config, frame160);
}

extern "C" bool phone_apm_output_rms_dbfs(phone_apm_t *a, int *out_dbfs) {
   if (a == nullptr || a->apm == nullptr || out_dbfs == nullptr) {
      return false;
   }
   webrtc::AudioProcessingStats stats = a->apm->GetStatistics();
   if (!stats.output_rms_dbfs.has_value()) {
      return false;
   }
   *out_dbfs = *stats.output_rms_dbfs;
   return true;
}

extern "C" void phone_apm_destroy(phone_apm_t *a) {
   if (a == nullptr) {
      return;
   }
   delete a->apm;
   delete a;
}

#else /* !AEC_BACKEND_WEBRTC — stubs so the bridge links and falls back to raw gain */

extern "C" bool phone_apm_output_rms_dbfs(phone_apm_t *a, int *out_dbfs) {
   (void)a;
   (void)out_dbfs;
   return false;
}

extern "C" phone_apm_t *phone_apm_create(const phone_apm_config_t *cfg) {
   (void)cfg;
   return nullptr;
}
extern "C" void phone_apm_reverse_10ms(phone_apm_t *a, const int16_t *frame160) {
   (void)a;
   (void)frame160;
}
extern "C" void phone_apm_process_10ms(phone_apm_t *a, int16_t *frame160) {
   (void)a;
   (void)frame160;
}
extern "C" void phone_apm_destroy(phone_apm_t *a) {
   (void)a;
}

#endif /* AEC_BACKEND_WEBRTC */
