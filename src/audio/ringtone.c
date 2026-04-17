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
 * Iron Man helmet ringtone — NES 2A03-style pulse synthesis.
 *
 * Melody: chromatic descent in pairs (C5 C5 B4 B4 A#4 A#4 A4 A4 ...)
 * followed by G#4 sixteenth and held F4. Two pulse channels: 25% duty
 * lead + 50% duty octave-up voice. PolyBLEP anti-aliasing.
 *
 * Uses the audio_backend abstraction for portable playback across
 * ALSA and PulseAudio backends.
 */

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "audio/audio_backend.h"
#include "config/dawn_config.h"
#include "logging.h"

/* =============================================================================
 * Constants
 * ============================================================================= */

#define RINGTONE_SAMPLE_RATE 44100
#define RINGTONE_CHANNELS 1

/* NES-style envelope: fast attack/release to suppress DC click artifacts. */
#define RINGTONE_ATTACK_MS 1
#define RINGTONE_RELEASE_MS 3

/* Duty cycles: 25% lead + 50% octave-up (classic NES pairing). */
#define PULSE1_DUTY 0.25
#define PULSE2_DUTY 0.50

/* Mix levels and master output. */
#define PULSE1_GAIN 0.5
#define PULSE2_GAIN 0.2
#define MASTER_GAIN 0.15

/* =============================================================================
 * Melody Data
 * ============================================================================= */

typedef struct {
   uint8_t midi;
   uint16_t duration_ms;
} ringtone_note_t;

static const ringtone_note_t s_melody[] = {
   { 72, 220 }, /* C5  */
   { 72, 220 }, /* C5  */
   { 71, 220 }, /* B4  */
   { 71, 220 }, /* B4  */
   { 70, 220 }, /* A#4 */
   { 70, 220 }, /* A#4 */
   { 69, 220 }, /* A4  */
   { 69, 220 }, /* A4  */
   { 72, 220 }, /* C5  */
   { 72, 220 }, /* C5  */
   { 71, 220 }, /* B4  */
   { 71, 220 }, /* B4  */
   { 70, 220 }, /* A#4 */
   { 68, 110 }, /* G#4 (16th note)      */
   { 65, 550 }, /* F4  (dotted quarter) */
};

static const size_t s_melody_len = sizeof(s_melody) / sizeof(s_melody[0]);

/* =============================================================================
 * Synthesis
 * ============================================================================= */

static double midi_to_hz(double midi) {
   return 440.0 * pow(2.0, (midi - 69.0) / 12.0);
}

/* PolyBLEP correction for sawtooth phase discontinuity. */
static double polyblep(double t, double dt) {
   if (t < dt) {
      t /= dt;
      return t + t - t * t - 1.0;
   } else if (t > 1.0 - dt) {
      t = (t - 1.0) / dt;
      return t * t + t + t + 1.0;
   }
   return 0.0;
}

/* Pulse wave = difference of two sawtooths offset by duty cycle. */
static double pulse_sample(double phase, double dt, double duty) {
   double p2 = phase - duty;
   if (p2 < 0.0) {
      p2 += 1.0;
   }
   double s1 = 2.0 * phase - 1.0 - polyblep(phase, dt);
   double s2 = 2.0 * p2 - 1.0 - polyblep(p2, dt);
   return s1 - s2;
}

/* Render one note into an int16 PCM buffer. */
static int16_t *render_note(const ringtone_note_t *note, size_t *out_samples) {
   size_t total = (size_t)(RINGTONE_SAMPLE_RATE * note->duration_ms / 1000);
   int16_t *buf = calloc(total, sizeof(int16_t));
   if (!buf) {
      return NULL;
   }
   *out_samples = total;

   if (note->midi == 0) {
      return buf; /* rest */
   }

   double ph1 = 0.0;
   double ph2 = 0.0;

   size_t attack = RINGTONE_SAMPLE_RATE * RINGTONE_ATTACK_MS / 1000;
   size_t release = RINGTONE_SAMPLE_RATE * RINGTONE_RELEASE_MS / 1000;
   if (attack < 1) {
      attack = 1;
   }
   if (release < 1) {
      release = 1;
   }
   if (attack + release > total) {
      attack = total / 8;
      release = total / 8;
      if (attack < 1) {
         attack = 1;
      }
      if (release < 1) {
         release = 1;
      }
   }

   double f_lo = midi_to_hz((double)note->midi);
   double f_hi = f_lo * 2.0;
   double dt1 = f_lo / RINGTONE_SAMPLE_RATE;
   double dt2 = f_hi / RINGTONE_SAMPLE_RATE;

   for (size_t i = 0; i < total; i++) {
      double env;
      if (i < attack) {
         env = (double)i / (double)attack;
      } else if (i > total - release) {
         env = (double)(total - i) / (double)release;
      } else {
         env = 1.0;
      }

      double s = PULSE1_GAIN * pulse_sample(ph1, dt1, PULSE1_DUTY) +
                 PULSE2_GAIN * pulse_sample(ph2, dt2, PULSE2_DUTY);
      s *= env * MASTER_GAIN;
      if (s > 1.0) {
         s = 1.0;
      }
      if (s < -1.0) {
         s = -1.0;
      }
      buf[i] = (int16_t)(s * 32000);

      ph1 += dt1;
      if (ph1 >= 1.0) {
         ph1 -= 1.0;
      }
      ph2 += dt2;
      if (ph2 >= 1.0) {
         ph2 -= 1.0;
      }
   }
   return buf;
}

/* =============================================================================
 * Public API
 * ============================================================================= */

/* Pre-rendered melody buffer (allocated once on first play, reused thereafter) */
static int16_t *s_melody_pcm = NULL;
static size_t s_melody_pcm_samples = 0;

/**
 * @brief Pre-render the entire melody into a single PCM buffer.
 * @return SUCCESS (0) or FAILURE (1).
 */
static int ensure_melody_rendered(void) {
   if (s_melody_pcm) {
      return 0; /* Already rendered */
   }

   /* Calculate total samples */
   size_t total = 0;
   for (size_t i = 0; i < s_melody_len; i++) {
      total += (size_t)(RINGTONE_SAMPLE_RATE * s_melody[i].duration_ms / 1000);
   }

   s_melody_pcm = malloc(total * sizeof(int16_t));
   if (!s_melody_pcm) {
      LOG_ERROR("ringtone: failed to allocate %zu bytes for melody", total * sizeof(int16_t));
      return 1;
   }

   size_t offset = 0;
   for (size_t i = 0; i < s_melody_len; i++) {
      size_t n = 0;
      int16_t *note_buf = render_note(&s_melody[i], &n);
      if (!note_buf) {
         free(s_melody_pcm);
         s_melody_pcm = NULL;
         return 1;
      }
      memcpy(s_melody_pcm + offset, note_buf, n * sizeof(int16_t));
      offset += n;
      free(note_buf);
   }

   s_melody_pcm_samples = total;
   LOG_INFO("ringtone: pre-rendered %zu samples (%.1fs)", total,
            (double)total / RINGTONE_SAMPLE_RATE);
   return 0;
}

/**
 * @brief Play the Iron Man ringtone melody once.
 *
 * Pre-renders the melody on first call (cached for subsequent plays).
 * Opens a playback handle via audio_backend, writes the PCM buffer,
 * drains, and closes. Blocking — runs ~3.5 seconds.
 *
 * @return SUCCESS (0) or FAILURE (1).
 */
int ironman_ringtone_play(void) {
   if (ensure_melody_rendered() != 0) {
      return 1;
   }

   audio_stream_params_t params;
   audio_stream_playback_default_params(&params);
   params.sample_rate = RINGTONE_SAMPLE_RATE;
   params.channels = RINGTONE_CHANNELS;
   params.format = AUDIO_FORMAT_S16_LE;

   audio_hw_params_t hw_params;
   audio_stream_playback_handle_t *pb = audio_stream_playback_open(g_config.audio.playback_device,
                                                                   &params, &hw_params);
   if (!pb) {
      LOG_ERROR("ringtone: failed to open playback device");
      return 1;
   }

   ssize_t written = audio_stream_playback_write(pb, s_melody_pcm, s_melody_pcm_samples);
   if (written < 0) {
      int recovered = audio_stream_playback_recover(pb, (int)(-written));
      if (recovered != 0) {
         LOG_ERROR("ringtone: playback write failed");
         audio_stream_playback_close(pb);
         return 1;
      }
   }

   audio_stream_playback_drain(pb);
   audio_stream_playback_close(pb);
   return 0;
}
