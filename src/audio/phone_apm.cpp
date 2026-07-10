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
#include <cstring>

/* Default near-end (uplink) config — live-tuned by ear on real 2-party calls
 * 2026-07-09 (echo cancel on; minimal fixed gain, let AGC2 do the work).  Named
 * constants, not literals.  The downlink derives from these with a few overrides
 * (see phone_audio_config_default). */
#define PHONE_APM_DEF_AGC_ENABLED true
#define PHONE_APM_DEF_FIXED_GAIN_DB 1.0f
#define PHONE_APM_DEF_RAMP_DB_PER_S 3.0f
#define PHONE_APM_DEF_MAX_NOISE_DBFS (-50.0f)
#define PHONE_APM_DEF_NS_LEVEL PHONE_NS_MODERATE
#define PHONE_APM_DEF_HIGH_PASS true
#define PHONE_APM_DEF_ECHO_CANCEL true

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

extern "C" bool phone_apm_available(void) {
#ifdef AEC_BACKEND_WEBRTC
   return true;
#else
   return false;
#endif
}

/* Clamp the tunable knobs to their valid ranges (pure; both builds).  Callers that
 * store/persist a config from user input should clamp first so the stored value
 * matches what the APM will actually apply (fixed_gain >= 50 would otherwise be
 * silently reverted).  build_apm_config also clamps at apply time (defence in
 * depth), using these same constants. */
extern "C" void phone_apm_clamp_config(phone_apm_config_t *cfg) {
   if (cfg == nullptr) {
      return;
   }
   cfg->fixed_gain_db = std::min(std::max(cfg->fixed_gain_db, PHONE_APM_FIXED_GAIN_MIN),
                                 PHONE_APM_FIXED_GAIN_MAX);
   cfg->max_gain_change_db_per_s = std::min(
       std::max(cfg->max_gain_change_db_per_s, PHONE_APM_RAMP_MIN), PHONE_APM_RAMP_MAX);
   cfg->max_output_noise_dbfs = std::min(
       std::max(cfg->max_output_noise_dbfs, PHONE_APM_NOISE_DBFS_MIN), PHONE_APM_NOISE_DBFS_MAX);
}

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

/* One APM instance + a cached StreamConfig (built once, not per frame) + the
 * last-applied config, kept so phone_apm_reconfigure can diff and apply only what
 * changed (some AGC2 params need a runtime setting, not ApplyConfig — see there). */
struct phone_apm {
   webrtc::AudioProcessing *apm = nullptr;
   webrtc::StreamConfig stream_config{ PHONE_APM_RATE, 1 };
   phone_apm_config_t cur{};
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

/* AGC2 fixed gain, clamped strictly below 50 (>=50 fails v1.3 validation and
 * silently reverts the whole AGC2 block to disabled). */
static float clamped_fixed_gain(const phone_apm_config_t *cfg) {
   return std::min(std::max(cfg->fixed_gain_db, PHONE_APM_FIXED_GAIN_MIN),
                   PHONE_APM_FIXED_GAIN_MAX);
}

/* Build the WebRTC config from our config, applying ALL clamps.  Shared by
 * phone_apm_create and phone_apm_reconfigure so the fixed_gain<50 guard and the
 * adaptive-knob clamps can never drift between the two paths (a divergence would
 * let a mid-call save silently disable AGC2). */
static webrtc::AudioProcessing::Config build_apm_config(const phone_apm_config_t *cfg,
                                                        bool warn_on_clamp) {
   webrtc::AudioProcessing::Config c;

   /* Echo canceller: off in Phase 1 (reverse stream fed "dark").  When enabled,
    * mobile_mode = AECM — AEC3 does NOT cancel at 16 kHz (see aec_webrtc.cpp:82-90). */
   c.echo_canceller.enabled = cfg->echo_cancel;
   c.echo_canceller.mobile_mode = true;

   c.high_pass_filter.enabled = cfg->high_pass;

   c.noise_suppression.enabled = (cfg->ns_level != PHONE_NS_OFF);
   c.noise_suppression.level = map_ns_level(cfg->ns_level);

   float fixed_gain = clamped_fixed_gain(cfg);
   if (warn_on_clamp && fixed_gain != cfg->fixed_gain_db) {
      OLOG_WARNING("phone_apm: fixed_gain_db %.1f out of range [%.0f,%.0f) — clamped to %.1f",
                   (double)cfg->fixed_gain_db, (double)PHONE_APM_FIXED_GAIN_MIN, 50.0,
                   (double)fixed_gain);
   }
   float ramp = std::min(std::max(cfg->max_gain_change_db_per_s, PHONE_APM_RAMP_MIN),
                         PHONE_APM_RAMP_MAX);
   float noise_floor = std::min(std::max(cfg->max_output_noise_dbfs, PHONE_APM_NOISE_DBFS_MIN),
                                PHONE_APM_NOISE_DBFS_MAX);

   c.gain_controller1.enabled = false;
   c.gain_controller2.enabled = cfg->agc_enabled;
   c.gain_controller2.fixed_digital.gain_db = fixed_gain;
   c.gain_controller2.adaptive_digital.enabled = cfg->agc_enabled;
   c.gain_controller2.adaptive_digital.max_gain_change_db_per_second = ramp;
   c.gain_controller2.adaptive_digital.max_output_noise_level_dbfs = noise_floor;
   return c;
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

   a->apm->ApplyConfig(build_apm_config(cfg, /*warn_on_clamp=*/true));
   a->cur = *cfg;

   OLOG_INFO("phone_apm: 16 kHz near-end active (agc=%d ns=%d hpf=%d aec=%d)", cfg->agc_enabled,
             (int)cfg->ns_level, cfg->high_pass, cfg->echo_cancel);
   return a;
}

extern "C" void phone_apm_reconfigure(phone_apm_t *a, const phone_apm_config_t *cfg) {
   if (a == nullptr || a->apm == nullptr || cfg == nullptr) {
      return;
   }
   /* Nothing to do if this instance's config is unchanged (e.g. a save that only
    * touched the other direction or the soft-limiter gain).  memcmp is
    * conservative — mismatched struct padding at worst does a redundant (cheap)
    * ApplyConfig, never skips a real change. */
   if (memcmp(&a->cur, cfg, sizeof(*cfg)) == 0) {
      return;
   }
   const phone_apm_config_t old = a->cur;
   webrtc::AudioProcessing::Config c = build_apm_config(cfg, /*warn_on_clamp=*/true);

   /* ApplyConfig reinits AGC2 ONLY on an enabled flip (v1.3
    * audio_processing_impl.cc:574 compares only .enabled), so with AGC staying on
    * a fixed_gain/ramp/noise change would be a silent no-op.  NS(level)/HPF/echo/
    * AGC-enable-toggle all reinit correctly through ApplyConfig. */
   const bool agc_stays_on = old.agc_enabled && cfg->agc_enabled;
   const bool ramp_or_noise_changed = cfg->max_gain_change_db_per_s !=
                                          old.max_gain_change_db_per_s ||
                                      cfg->max_output_noise_dbfs != old.max_output_noise_dbfs;

   if (agc_stays_on && ramp_or_noise_changed) {
      /* Force an AGC2 reinit to push the new adaptive params (this also re-pushes
       * fixed gain): disable then re-enable.  One-cycle dip, acceptable for a rare
       * tuning change; runs on the bridge thread pre-capture (spike absorbed). */
      webrtc::AudioProcessing::Config off = c;
      off.gain_controller2.enabled = false;
      a->apm->ApplyConfig(off);
      a->apm->ApplyConfig(c);
   } else {
      a->apm->ApplyConfig(c);
      /* Fixed-gain change with AGC staying on: the top-level ApplyConfig won't
       * apply it (enabled unchanged) — push it via the runtime setting.  In v1.3
       * this is applied on the NEXT ProcessStream and its handler rebuilds
       * AdaptiveAgc (a small mid-cycle alloc + adaptive-state reset — the same dip
       * as the reinit path, just deferred); fine for a rare save.  Clamp first: the
       * runtime path has only an RTC_DCHECK, not the top-level <50 revert-guard. */
      if (agc_stays_on && cfg->fixed_gain_db != old.fixed_gain_db) {
         a->apm->SetRuntimeSetting(
             webrtc::AudioProcessing::RuntimeSetting::CreateCaptureFixedPostGain(
                 clamped_fixed_gain(cfg)));
      }
   }
   a->cur = *cfg;
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

extern "C" void phone_apm_destroy(phone_apm_t *a) {
   if (a == nullptr) {
      return;
   }
   delete a->apm;
   delete a;
}

#else /* !AEC_BACKEND_WEBRTC — stubs so the bridge links and falls back to raw gain */

extern "C" phone_apm_t *phone_apm_create(const phone_apm_config_t *cfg) {
   (void)cfg;
   return nullptr;
}
extern "C" void phone_apm_reconfigure(phone_apm_t *a, const phone_apm_config_t *cfg) {
   (void)a;
   (void)cfg;
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
