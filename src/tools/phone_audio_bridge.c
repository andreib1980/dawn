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
 * Phone Audio Bridge — full-duplex PCM between the SIM7600G-H modem USB audio
 * port (/dev/ttyUSB4, raw 16 kHz S16LE mono) and the local handset (mic +
 * speaker).
 *
 * The SIM7600 USB PCM is a SYNCHRONOUS full-duplex stream: the modem exchanges
 * one downlink frame and one uplink frame per period and expects the host to
 * keep read and write BALANCED.  A two-thread design (independent reader +
 * writer) drifts and the modem stops draining uplink (verified 2026-07-06:
 * uplink writes all POLLOUT-timed-out mid-call).  So a SINGLE thread does both
 * directions in lockstep each cycle, paced by the blocking mic capture (~20 ms):
 * capture mic -> write uplink -> read the matching amount of downlink -> play.
 *
 * PulseAudio does the rate/channel conversion and permits multiple streams per
 * device, so both audio streams are opened at 16 kHz mono (matching the modem),
 * no resampler is needed, and the main-loop wake-word capture keeps running on
 * the same mic in parallel.  The fd is O_NONBLOCK and the mic capture bounds the
 * cycle, so teardown is bounded; a fatal error latches s_running=false.
 */

#include "tools/phone_audio_bridge.h"

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>

#include "audio/audio_backend.h"
#include "dawn.h"
#include "dawn_error.h"
#include "logging.h"

/* Modem USB PCM format (see docs/PHONE_SMS_DESIGN.md Hardware Notes). */
#define PHONE_PCM_RATE 16000
#define PHONE_PCM_CHANNELS 1
#define PHONE_PCM_FRAME_SAMPLES 320 /* 20 ms @ 16 kHz */
#define PHONE_PCM_FRAME_BYTES (PHONE_PCM_FRAME_SAMPLES * (int)sizeof(int16_t) * PHONE_PCM_CHANNELS)
#define PHONE_PCM_BUFFER_FRAMES (PHONE_PCM_FRAME_SAMPLES * 4)
/* All call-audio level control is digital, here in the bridge: the SIM7600's
 * own gain controls (CRXVOL/CTXVOL/COUTGAIN/CLVL) act on the analog codec only
 * and do NOT affect the USB PCM tap (verified 2026-07-06 — CRXVOL was flat
 * across its whole 0x0000-0xFFFF range on the PCM stream).  The raw downlink
 * ranges from near-silence (quiet talker) to hard clipping (loud talker), so a
 * fixed gain can't win.  We use a tanh soft limiter: ~GAIN for quiet audio,
 * graceful saturation for loud peaks instead of hard clipping.  Small-signal
 * gains, tunable by ear; 1.0 = unity. */
#define PHONE_DOWNLINK_GAIN 8.0f /* far-end -> local speaker */
#define PHONE_UPLINK_GAIN 4.0f   /* local mic -> far-end ("rough unless close to mic") */

/* ~20 ms cycles: log the APM output level roughly once/second (objective tuning
 * trace, design §8), and throttle the uplink-stall drop warning the same way. */
#define PHONE_RMS_LOG_INTERVAL 50
#define PHONE_DROP_LOG_INTERVAL 50

/* out = FS * tanh(gain * in / FS): soft-limiting gain on one S16 sample. */
static inline int16_t soft_gain_s16(int16_t s, float gain) {
   float y = 32767.0f * tanhf(gain * (float)s / 32767.0f);
   if (y > 32767.0f) {
      y = 32767.0f;
   } else if (y < -32768.0f) {
      y = -32768.0f;
   }
   return (int16_t)lrintf(y);
}
#define PHONE_PCM_POLL_MS                                                 \
   100 /* poll timeout bounding both directions (teardown responsiveness) \
        */

/* Start/stop serialization + shared handles.  The bridge thread never takes
 * s_lock, so holding it across pthread_join() in stop cannot deadlock.
 *
 * Two flags with distinct roles: s_started (lifecycle, mutex-guarded, written
 * only by start/stop) says "the thread exists and must be joined + handles
 * closed"; s_running (loop control, atomic, thread-writable) says "keep
 * looping".  The thread may latch s_running=false on a fatal error, but stop()
 * still joins and cleans up because it gates on s_started — so a self-exited
 * thread can't leak. */
static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
static bool s_started = false;
static atomic_bool s_running = false;
static pthread_t s_bridge_tid;
static int s_pcm_fd = -1;
static audio_stream_capture_handle_t *s_capture = NULL;
static audio_stream_playback_handle_t *s_playback = NULL;
/* Near-end uplink processor (NULL -> raw soft-gain fallback) and the downlink
 * soft-limiter gain, both set from config in start, owned by the bridge thread. */
static phone_apm_t *s_uplink_apm = NULL;
static float s_downlink_gain = PHONE_DOWNLINK_GAIN;

/* Open the modem PCM serial node in raw, non-blocking binary mode. Returns fd or -1. */
static int open_pcm_port(const char *port) {
   /* This is only ever a modem serial node.  Require exactly
    * /dev/ttyUSB<digits> or /dev/ttyACM<digits> — an unanchored prefix would
    * also accept /dev/ttyUSBfoo or a path with trailing junk, letting a bad or
    * hostile config value aim open(O_RDWR) at an unintended device. */
   bool path_ok = false;
   if (strncmp(port, "/dev/ttyUSB", 11) == 0 || strncmp(port, "/dev/ttyACM", 11) == 0) {
      const char *p = port + 11;
      path_ok = (*p != '\0');
      for (; *p; p++) {
         if (*p < '0' || *p > '9') {
            path_ok = false;
            break;
         }
      }
   }
   if (!path_ok) {
      OLOG_ERROR("phone_bridge: refusing PCM port '%s' (must be /dev/ttyUSB<n> or /dev/ttyACM<n>)",
                 port);
      return -1;
   }

   /* O_NONBLOCK: never block in open() on carrier, and drive both directions via
    * poll() for a structurally-bounded teardown.  O_NOFOLLOW: don't traverse a
    * symlink swapped in at the final path component. */
   int fd = open(port, O_RDWR | O_NOCTTY | O_CLOEXEC | O_NONBLOCK | O_NOFOLLOW);
   if (fd < 0) {
      OLOG_ERROR("phone_bridge: open %s failed: %s", port, strerror(errno));
      return -1;
   }

   struct stat st;
   if (fstat(fd, &st) != 0 || !S_ISCHR(st.st_mode)) {
      OLOG_ERROR("phone_bridge: %s is not a character device", port);
      close(fd);
      return -1;
   }

   struct termios tio;
   if (tcgetattr(fd, &tio) != 0) {
      OLOG_ERROR("phone_bridge: tcgetattr %s failed: %s", port, strerror(errno));
      close(fd);
      return -1;
   }

   /* Raw, binary-clean: no canonical/echo, no CR/LF or XON/XOFF translation that
    * would corrupt PCM bytes. */
   cfmakeraw(&tio);
   tio.c_cflag |= (CLOCAL | CREAD);
   tio.c_cflag &= ~CRTSCTS;
   tio.c_iflag &= ~(IXON | IXOFF | IXANY);
   cfsetispeed(&tio, B115200); /* USB CDC ignores baud, but set a sane value */
   cfsetospeed(&tio, B115200);

   if (tcsetattr(fd, TCSANOW, &tio) != 0) {
      OLOG_ERROR("phone_bridge: tcsetattr %s failed: %s", port, strerror(errno));
      close(fd);
      return -1;
   }

   tcflush(fd, TCIFLUSH); /* drop any PCM buffered between CPCMREG=1 and our open */
   return fd;
}

/* Write a byte buffer to the modem with the bridge's EAGAIN/poll/EINTR
 * semantics.  Returns bytes actually written (< len only on a poll timeout or a
 * fatal error); sets *fatal on an unrecoverable write error. */
static size_t modem_write_some(const uint8_t *buf, size_t len, bool *fatal) {
   size_t off = 0;
   while (off < len && atomic_load(&s_running)) {
      ssize_t wn = write(s_pcm_fd, buf + off, len - off);
      if (wn < 0) {
         if (errno == EINTR) {
            continue;
         }
         if (errno == EAGAIN) { /* modem momentarily full — brief wait, else stop trying */
            struct pollfd pf = { .fd = s_pcm_fd, .events = POLLOUT };
            if (poll(&pf, 1, PHONE_PCM_POLL_MS) <= 0) {
               break;
            }
            continue;
         }
         OLOG_ERROR("phone_bridge: uplink write error: %s", strerror(errno));
         *fatal = true;
         break;
      }
      off += (size_t)wn;
   }
   return off;
}

/* Single lockstep thread: mic capture paces the cycle (~20 ms); each cycle
 * cleans + writes the uplink and reads the matching amount of downlink, keeping
 * the modem's synchronous full-duplex PCM balanced so it never stops draining
 * uplink.
 *
 * Uplink: with the near-end APM the 320-sample (20 ms) capture is staged into
 * APM-sized 160-sample (10 ms) blocks — 320 = 2x160, so staging is a
 * steady-state pass-through and only a transient short read carries a <160
 * remainder.  The previous cycle's post-gain downlink is fed to the APM as a
 * reverse (render) reference, split into two 10 ms halves so each uplink block
 * gets its time-aligned half — dark in Phase 1 (echo_cancel off), so flipping
 * AECM on in Phase 2 is a config change, not a re-plumb.  Without the APM the
 * uplink falls back to the raw tanh soft-limiter.
 *
 * Balance: the downlink read is keyed to the samples the uplink actually emitted
 * this cycle (E) — exactly the pre-APM behaviour in steady state (E = 320 =
 * frames), so the read/write balance is unchanged.  arch-L1: any unwritten
 * uplink remainder is carried in up_pending[] (pending-then-new) so a partial
 * write() never leaves the modem mid-sample and desyncs 16-bit framing. */
static void *bridge_thread(void *arg) {
   (void)arg;
   int16_t up[PHONE_PCM_FRAME_SAMPLES];
   uint8_t dn[PHONE_PCM_FRAME_BYTES + 1]; /* +1 for a split-sample carry byte */
   size_t dn_carry = 0;

   phone_apm_t *apm = s_uplink_apm; /* may be NULL -> raw-gain fallback */
   const float dn_gain = s_downlink_gain;

   /* Uplink APM staging (bridge-thread-local -> resets per call): up to a
    * 159-sample carry plus one fresh capture. */
   int16_t up_stage[PHONE_APM_FRAME + PHONE_PCM_FRAME_SAMPLES];
   int up_stage_n = 0;
   /* Previous cycle's post-gain downlink (two 10 ms halves), fed as the reverse
    * reference so each uplink block gets its time-aligned half. */
   int16_t reverse_ref[PHONE_PCM_FRAME_SAMPLES];
   memset(reverse_ref, 0, sizeof(reverse_ref));

   /* arch-L1 uplink byte carry.  New bytes are only appended when the buffer is
    * empty, so it never exceeds one frame — sized accordingly. */
   uint8_t up_pending[PHONE_PCM_FRAME_BYTES];
   size_t up_pending_n = 0;

   unsigned cycle = 0;         /* for the ~1 s periodic APM level log */
   uint64_t dropped_total = 0; /* uplink samples dropped during a write stall */
   unsigned drop_events = 0;

   while (atomic_load(&s_running)) {
      /* ---- capture mic (blocking ~20 ms — the real-time pace) ---- */
      ssize_t frames = audio_stream_capture_read(s_capture, up, PHONE_PCM_FRAME_SAMPLES);
      if (frames < 0) {
         int err = (int)(-frames);
         if (err == AUDIO_ERR_OVERRUN) {
            audio_stream_capture_recover(s_capture, err);
            continue;
         }
         OLOG_ERROR("phone_bridge: capture error: %s", audio_error_string((audio_error_t)err));
         atomic_store(&s_running, false);
         break;
      }
      if (frames == 0) {
         continue; /* no data — the blocking mic read is the loop's only pacer */
      }

      /* ---- uplink: produce `emit` cleaned samples for this cycle ---- */
      int16_t *emit = up;
      size_t emit_n = 0;
      if (apm != NULL) {
         memcpy(up_stage + up_stage_n, up, (size_t)frames * sizeof(int16_t));
         up_stage_n += (int)frames;
         int blocks = up_stage_n / PHONE_APM_FRAME;
         for (int b = 0; b < blocks; b++) {
            int16_t *blk = up_stage + (size_t)b * PHONE_APM_FRAME;
            /* time-aligned reverse half; (b & 1) keeps the index in-bounds
             * regardless of block count (blocks is at most 2 here). */
            phone_apm_reverse_10ms(apm, reverse_ref + (size_t)(b & 1) * PHONE_APM_FRAME);
            phone_apm_process_10ms(apm, blk); /* HPF + NS + AGC2, in place */
         }
         emit = up_stage;
         emit_n = (size_t)blocks * PHONE_APM_FRAME;
      } else {
         for (ssize_t i = 0; i < frames; i++) {
            up[i] = soft_gain_s16(up[i], PHONE_UPLINK_GAIN);
         }
         emit_n = (size_t)frames;
      }

      /* ---- uplink write: pending-then-new, carry the remainder (arch-L1) ---- */
      bool fatal = false;
      bool new_written = false;
      if (up_pending_n > 0) {
         size_t w = modem_write_some(up_pending, up_pending_n, &fatal);
         if (w < up_pending_n) {
            memmove(up_pending, up_pending + w, up_pending_n - w);
         }
         up_pending_n -= w;
      }
      if (!fatal && up_pending_n == 0 && emit_n > 0) {
         const uint8_t *nb = (const uint8_t *)emit;
         size_t nlen = emit_n * sizeof(int16_t);
         size_t w = modem_write_some(nb, nlen, &fatal);
         if (w < nlen) { /* carry unwritten remainder; bounded by one frame */
            up_pending_n = nlen - w;
            memcpy(up_pending, nb + w, up_pending_n);
         }
         new_written = true;
      }
      if (fatal) {
         atomic_store(&s_running, false);
         break;
      }
      /* Pending didn't drain -> this cycle's cleaned uplink was neither written
       * nor carried: dropped (bounded by design, arch-L1 keeps framing intact).
       * Only during a sustained modem write stall — log it, throttled. */
      if (emit_n > 0 && !new_written) {
         dropped_total += emit_n;
         if ((drop_events++ % PHONE_DROP_LOG_INTERVAL) == 0) {
            OLOG_WARNING("phone_bridge: uplink write stalled — dropped ~%llu samples so far",
                         (unsigned long long)dropped_total);
         }
      }

      /* ---- shift the APM stage remainder AFTER emit has been consumed ---- */
      if (apm != NULL) {
         int rem = up_stage_n - (int)emit_n;
         if (rem > 0) {
            memmove(up_stage, up_stage + emit_n, (size_t)rem * sizeof(int16_t));
         }
         up_stage_n = rem;
      }

      /* ---- downlink: read balanced to the emitted uplink, soft-gain, play ---- */
      /* want tracks emitted samples so read/write stay balanced.  emit_n == 0
       * only on a transient sub-160 capture that staged without emitting; read a
       * full frame then (deliberate — a non-blocking cap that drains rather than
       * starves downlink, matching the pre-APM frames==0 behaviour). */
      size_t want = emit_n * sizeof(int16_t);
      if (want == 0 || want > (size_t)PHONE_PCM_FRAME_BYTES) {
         want = PHONE_PCM_FRAME_BYTES;
      }
      ssize_t n = read(s_pcm_fd, dn + dn_carry, want);
      if (n < 0) {
         if (errno != EAGAIN && errno != EINTR) {
            OLOG_ERROR("phone_bridge: downlink read error: %s", strerror(errno));
            atomic_store(&s_running, false);
            break;
         }
         n = 0;
      }
      if (n > 0) {
         size_t total = dn_carry + (size_t)n;
         size_t dframes = total / 2; /* mono S16 */
         int16_t *sm = (int16_t *)dn;
         for (size_t i = 0; i < dframes; i++) {
            sm[i] = soft_gain_s16(sm[i], dn_gain);
         }
         /* Stash up to the last 320 post-gain downlink samples (what actually
          * plays out the speaker) as next cycle's reverse reference, right-
          * aligned so the two 10 ms halves stay time-ordered for the per-block
          * feed above. */
         if (apm != NULL) {
            size_t keep = dframes < (size_t)PHONE_PCM_FRAME_SAMPLES
                              ? dframes
                              : (size_t)PHONE_PCM_FRAME_SAMPLES;
            memset(reverse_ref, 0, sizeof(reverse_ref));
            memcpy(reverse_ref + (PHONE_PCM_FRAME_SAMPLES - keep), sm + (dframes - keep),
                   keep * sizeof(int16_t));
         }
         size_t pw = 0;
         while (pw < dframes && atomic_load(&s_running)) {
            ssize_t w = audio_stream_playback_write(s_playback, dn + pw * 2, dframes - pw);
            if (w < 0) {
               int err = (int)(-w);
               if (err == AUDIO_ERR_UNDERRUN) {
                  audio_stream_playback_recover(s_playback, err);
                  continue;
               }
               OLOG_ERROR("phone_bridge: playback error: %s",
                          audio_error_string((audio_error_t)err));
               atomic_store(&s_running, false);
               break;
            }
            if (w == 0) {
               break; /* guard: a 0-return would spin (pulse won't, but be safe) */
            }
            pw += (size_t)w;
         }
         size_t used = dframes * 2;
         dn_carry = total - used;
         if (dn_carry) {
            dn[0] = dn[used];
         }
      }

      /* ---- objective tuning trace: APM output level ~once/second ---- */
      if (apm != NULL && (++cycle % PHONE_RMS_LOG_INTERVAL) == 0) {
         int rms_dbfs = 0;
         if (phone_apm_output_rms_dbfs(apm, &rms_dbfs)) {
            OLOG_INFO("phone_bridge: uplink APM output ~%d dBFS", rms_dbfs);
         }
      }
   }
   return NULL;
}

/* Close handles + fd + APM. Caller holds s_lock and the bridge thread is already
 * stopped (so the APM has no concurrent user). */
static void cleanup_locked(void) {
   if (s_uplink_apm) {
      phone_apm_destroy(s_uplink_apm);
      s_uplink_apm = NULL;
   }
   if (s_playback) {
      audio_stream_playback_close(s_playback);
      s_playback = NULL;
   }
   if (s_capture) {
      audio_stream_capture_close(s_capture);
      s_capture = NULL;
   }
   if (s_pcm_fd >= 0) {
      close(s_pcm_fd);
      s_pcm_fd = -1;
   }
}

int phone_bridge_start(const phone_bridge_config_t *cfg) {
   if (!cfg || !cfg->pcm_port || !cfg->pcm_port[0]) {
      OLOG_ERROR("phone_bridge: no PCM port configured");
      return FAILURE;
   }
   const char *pcm_port = cfg->pcm_port;

   pthread_mutex_lock(&s_lock);
   if (s_started) {
      pthread_mutex_unlock(&s_lock);
      return SUCCESS; /* idempotent */
   }

   if (audio_backend_get_type() == AUDIO_BACKEND_NONE) {
      OLOG_ERROR("phone_bridge: audio backend not initialized");
      pthread_mutex_unlock(&s_lock);
      return FAILURE;
   }
   if (audio_backend_get_type() != AUDIO_BACKEND_PULSE) {
      /* Non-pulse backends won't rate/channel-convert a raw hw device to 16 kHz
       * mono; the local handset path assumes pulse (see file header). */
      OLOG_WARNING("phone_bridge: backend is %s, not pulse — 16 kHz mono conversion "
                   "may fail on a raw ALSA device",
                   audio_backend_type_name(audio_backend_get_type()));
   }

   const char *cap_dev = getPcmCaptureDevice();
   const char *play_dev = getPcmPlaybackDevice();
   if (!cap_dev || !cap_dev[0] || !play_dev || !play_dev[0]) {
      OLOG_ERROR("phone_bridge: local capture/playback device not configured");
      pthread_mutex_unlock(&s_lock);
      return FAILURE;
   }

   s_pcm_fd = open_pcm_port(pcm_port);
   if (s_pcm_fd < 0) {
      pthread_mutex_unlock(&s_lock);
      return FAILURE;
   }

   audio_stream_params_t params = { .sample_rate = PHONE_PCM_RATE,
                                    .channels = PHONE_PCM_CHANNELS,
                                    .format = AUDIO_FORMAT_S16_LE,
                                    .period_frames = PHONE_PCM_FRAME_SAMPLES,
                                    .buffer_frames = PHONE_PCM_BUFFER_FRAMES };
   audio_hw_params_t hw;

   s_capture = audio_stream_capture_open(cap_dev, &params, &hw);
   if (!s_capture) {
      OLOG_ERROR("phone_bridge: failed to open capture device %s", cap_dev);
      cleanup_locked();
      pthread_mutex_unlock(&s_lock);
      return FAILURE;
   }
   s_playback = audio_stream_playback_open(play_dev, &params, &hw);
   if (!s_playback) {
      OLOG_ERROR("phone_bridge: failed to open playback device %s", play_dev);
      cleanup_locked();
      pthread_mutex_unlock(&s_lock);
      return FAILURE;
   }

   /* Downlink gain + near-end uplink processor from config.  A NULL APM (WebRTC
    * not built, or init failed) is not an error — the thread falls back to the
    * raw soft-limiter. */
   s_downlink_gain = (cfg->downlink_gain > 0.0f) ? cfg->downlink_gain : PHONE_DOWNLINK_GAIN;
   s_uplink_apm = phone_apm_create(&cfg->uplink);
   OLOG_INFO("phone_bridge: uplink %s",
             s_uplink_apm ? "APM (AGC2+NS+HPF)" : "raw soft-gain (APM unavailable)");

   atomic_store(&s_running, true);
   if (pthread_create(&s_bridge_tid, NULL, bridge_thread, NULL) != 0) {
      OLOG_ERROR("phone_bridge: failed to start bridge thread");
      atomic_store(&s_running, false);
      cleanup_locked();
      pthread_mutex_unlock(&s_lock);
      return FAILURE;
   }

   s_started = true;
   OLOG_INFO("phone_bridge: started (port=%s, cap=%s, play=%s, 16 kHz mono)", pcm_port, cap_dev,
             play_dev);
   pthread_mutex_unlock(&s_lock);
   return SUCCESS;
}

void phone_bridge_stop(void) {
   pthread_mutex_lock(&s_lock);
   if (!s_started) {
      pthread_mutex_unlock(&s_lock);
      return;
   }
   /* Join even if the thread already self-latched s_running=false on error — it
    * exists and must be reaped, and the fd/handles must be closed. */
   atomic_store(&s_running, false);
   pthread_join(s_bridge_tid, NULL);
   cleanup_locked();
   s_started = false;
   OLOG_INFO("phone_bridge: stopped");
   pthread_mutex_unlock(&s_lock);
}

bool phone_bridge_is_active(void) {
   return atomic_load(&s_running);
}
