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
 * A single WebRTC Audio Processing Module (APM) instance configured for 16 kHz
 * mono, cleaning the local mic before it goes to the modem: high-pass -> noise
 * suppression -> AGC2 (per-frame, VAD-gated digital gain) -> limiter.  This is
 * the fix for "microphone-y / threshold-y / pumping" uplink audio — no modem
 * gain touches the raw USB PCM tap, so all level control is host-side.
 *
 * Distinct from src/audio/aec_webrtc.cpp: that is a 48 kHz singleton owned by
 * the wake-word capture path (AEC vs the TTS reference).  This is a separate,
 * concurrent instance on the phone bridge thread — a second instance is
 * REQUIRED, not merely convenient, because the two run at the same time on
 * different threads (see aec_webrtc.cpp:494-500 single-thread warning).
 *
 * Availability: the implementation is compiled unconditionally, but the WebRTC
 * machinery is only present when the daemon is built with WebRTC APM
 * (AEC_BACKEND_WEBRTC).  On builds without it, phone_apm_create() returns NULL
 * and the caller falls back to a raw gain.
 */

#ifndef PHONE_APM_H
#define PHONE_APM_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* WebRTC APM processes a fixed 10 ms frame; at 16 kHz that is 160 samples. */
#define PHONE_APM_RATE 16000
#define PHONE_APM_FRAME 160

/* Noise-suppression aggressiveness (maps to WebRTC NS levels). */
typedef enum {
   PHONE_NS_OFF = 0,
   PHONE_NS_LOW,
   PHONE_NS_MODERATE,
   PHONE_NS_HIGH,
   PHONE_NS_VERYHIGH
} phone_ns_level_t;

/* Near-end processing configuration.  See phone_apm_default_config() for the
 * starting-point values (tuned by ear on a live 2-party call). */
typedef struct {
   bool agc_enabled;               /* AGC2 adaptive_digital on/off */
   float fixed_gain_db;            /* AGC2 fixed_digital.gain_db (pre-gain); clamped to [0,50) */
   float max_gain_change_db_per_s; /* adaptive ramp rate (clamped >0) — lower = less pumping */
   float max_output_noise_dbfs;    /* adaptive noise ceiling (clamped [-90,0]) */
   phone_ns_level_t ns_level;      /* noise suppression aggressiveness */
   bool high_pass;                 /* DC/rumble removal */
   bool echo_cancel;               /* Phase 2: AECM on the reverse stream (default false) */
} phone_apm_config_t;

/* Sensible defaults for the phone uplink. */
phone_apm_config_t phone_apm_default_config(void);

/** @brief True if WebRTC APM is compiled in (else all APM controls are inert and
 *         the bridge uses the raw soft-limiter). */
bool phone_apm_available(void);

/** @brief Clamp tunable knobs (fixed_gain, ramp, noise floor) to valid ranges.
 *         Clamp user-supplied config before storing so it matches what's applied. */
void phone_apm_clamp_config(phone_apm_config_t *cfg);

/**
 * @brief Create a near-end processor.
 * @return Handle, or NULL if WebRTC APM is not compiled in or init failed — the
 *         caller MUST treat NULL as "no processing available" and fall back to a
 *         raw gain.
 */
struct phone_apm;
typedef struct phone_apm phone_apm_t;
phone_apm_t *phone_apm_create(const phone_apm_config_t *cfg);

/**
 * @brief Apply a new config to a live processor (no realloc for value changes).
 *
 * Handles the vendored WebRTC quirk that `ApplyConfig` reinits AGC2 only when its
 * enabled flag flips: fixed-gain changes go via a runtime setting (applied on the
 * next ProcessStream); ramp/noise changes force a brief AGC2 reinit; NS/HPF/echo/
 * enable-toggle apply directly.  MUST run on the bridge thread (single-owner), and
 * — because a submodule toggle allocates + resets — BEFORE the blocking capture
 * read so the capture ring absorbs the spike.  No-op when @p a is NULL or the
 * config is unchanged.
 */
void phone_apm_reconfigure(phone_apm_t *a, const phone_apm_config_t *cfg);

/**
 * @brief Feed the downlink about to be played as the echo reference.
 *
 * Plumbed every cycle even when echo_cancel is false ("dark"), so enabling
 * echo cancellation later is a config flip rather than a code change.  @p frame
 * must be PHONE_APM_FRAME samples.  No-op when @p a is NULL.
 */
void phone_apm_reverse_10ms(phone_apm_t *a, const int16_t *frame160);

/**
 * @brief Process exactly PHONE_APM_FRAME uplink samples in place.  No-op when
 *        @p a is NULL.
 */
void phone_apm_process_10ms(phone_apm_t *a, int16_t *frame160);

/** @brief Destroy a processor.  Safe on NULL. */
void phone_apm_destroy(phone_apm_t *a);

#ifdef __cplusplus
}
#endif

#endif /* PHONE_APM_H */
