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
 * Alarm/Timer Overlay for DAWN Satellite SDL UI
 *
 * Simple solid card overlay for alarm/timer/reminder notifications.
 * Renders above screensaver. Touch targets are 56px tall for reliability.
 */

#include "ui_alarm.h"

#include <SDL2/SDL2_gfxPrimitives.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "audio_playback.h"
#include "ui_colors.h"
#include "ui_util.h"

/* Log macros (satellite style) */
#ifndef OLOG_INFO
#define OLOG_INFO(fmt, ...) fprintf(stderr, "[INFO] alarm: " fmt "\n", ##__VA_ARGS__)
#endif
#ifndef OLOG_WARNING
#define OLOG_WARNING(fmt, ...) fprintf(stderr, "[WARN] alarm: " fmt "\n", ##__VA_ARGS__)
#endif

/* Font paths */
#define FALLBACK_MONO_FONT "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf"
#define FALLBACK_BODY_FONT "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"

/* Card geometry */
#define CARD_WIDTH 420
#define CARD_PADDING 24
#define CARD_RADIUS 16

/* Button geometry */
#define BTN_HEIGHT 56 /* Touch target height (design spec) */
#define BTN_WIDTH 180
#define BTN_GAP 16
#define BTN_RADIUS 12

/* Card colors */
#define CARD_BG_R 0x1E
#define CARD_BG_G 0x1E
#define CARD_BG_B 0x2E

/* Chime playback */
#define ALARM_GAP_MS 200    /* Gap between alarm tone repeats (ms) */
#define ALARM_TIMEOUT_S 120 /* Max alarm sound duration (seconds) */

/* =============================================================================
 * Chime Sound Thread
 * ============================================================================= */

typedef struct {
   ui_alarm_t *alarm;
   bool is_alarm;
} chime_thread_arg_t;

static void *chime_sound_thread(void *arg) {
   chime_thread_arg_t *cta = (chime_thread_arg_t *)arg;
   ui_alarm_t *a = cta->alarm;
   bool is_alarm = cta->is_alarm;
   free(cta);

   audio_playback_t *pb = (audio_playback_t *)a->audio_pb;
   if (!pb) {
      a->sound_thread_active = false;
      return NULL;
   }

   /* Get volume and pre-scale into temp buffer */
   int vol_pct = audio_playback_get_volume(pb);
   float vol_scale = (float)vol_pct / 100.0f;

   dawn_chime_buf_t *src = is_alarm ? &a->alarm_tone : &a->chime;
   if (!src->pcm || src->samples == 0) {
      a->sound_thread_active = false;
      return NULL;
   }

   int16_t *scaled = malloc(src->samples * sizeof(int16_t));
   if (!scaled) {
      a->sound_thread_active = false;
      return NULL;
   }
   dawn_chime_apply_volume(scaled, src->pcm, src->samples, vol_scale);

   if (is_alarm) {
      /* Looping alarm tone until dismissed or timeout */
      time_t start = time(NULL);
      while (!atomic_load(&a->sound_stop) && (time(NULL) - start) < ALARM_TIMEOUT_S) {
         audio_playback_play(pb, scaled, src->samples, (unsigned int)src->sample_rate,
                             &a->sound_stop, true);
         if (atomic_load(&a->sound_stop))
            break;
         usleep(ALARM_GAP_MS * 1000);
      }

      /* If we exited due to timeout (not user dismiss), auto-dismiss overlay */
      if (!atomic_load(&a->sound_stop)) {
         OLOG_INFO("alarm sound timed out after %ds — dismissing overlay", ALARM_TIMEOUT_S);
         pthread_mutex_lock(&a->mutex);
         a->state = ALARM_STATE_IDLE;
         pthread_mutex_unlock(&a->mutex);
      }
   } else {
      /* Single chime for timers/reminders */
      audio_playback_play(pb, scaled, src->samples, (unsigned int)src->sample_rate, &a->sound_stop,
                          true);
   }

   free(scaled);
   a->sound_thread_active = false;
   return NULL;
}

/** Stop and join any running sound thread */
static void stop_sound_thread(ui_alarm_t *a) {
   if (!a->sound_thread_active)
      return;
   atomic_store(&a->sound_stop, 1);
   pthread_join(a->sound_thread, NULL);
   a->sound_thread_active = false;
}

/** Start chime playback in background thread */
static void start_sound_thread(ui_alarm_t *a, bool is_alarm) {
   if (!a->audio_pb || !a->chime.pcm)
      return;

   stop_sound_thread(a);
   atomic_store(&a->sound_stop, 0);

   chime_thread_arg_t *cta = malloc(sizeof(chime_thread_arg_t));
   if (!cta)
      return;
   cta->alarm = a;
   cta->is_alarm = is_alarm;

   a->sound_thread_active = true;
   if (pthread_create(&a->sound_thread, NULL, chime_sound_thread, cta) != 0) {
      a->sound_thread_active = false;
      free(cta);
   }
}

/* =============================================================================
 * Lifecycle
 * ============================================================================= */

int ui_alarm_init(ui_alarm_t *a, SDL_Renderer *r, int w, int h, const char *font_dir) {
   memset(a, 0, sizeof(*a));
   pthread_mutex_init(&a->mutex, NULL);

   a->renderer = r;
   a->screen_w = w;
   a->screen_h = h;
   a->state = ALARM_STATE_IDLE;

   /* Load fonts */
   a->title_font = ui_try_load_font(font_dir, "SourceSans3-Bold.ttf", FALLBACK_BODY_FONT, 36);
   a->label_font = ui_try_load_font(font_dir, "SourceSans3-Medium.ttf", FALLBACK_BODY_FONT, 22);
   a->btn_font = ui_try_load_font(font_dir, "SourceSans3-SemiBold.ttf", FALLBACK_BODY_FONT, 20);

   if (!a->title_font || !a->label_font || !a->btn_font) {
      OLOG_WARNING("Failed to load alarm overlay fonts");
      return 1;
   }

   /* Pre-cache button labels */
   a->dismiss_tex = ui_build_white_tex(r, a->btn_font, "DISMISS", &a->dismiss_w, &a->dismiss_h);
   a->snooze_tex = ui_build_white_tex(r, a->btn_font, "SNOOZE", &a->snooze_w, &a->snooze_h);
   a->static_cache_ready = (a->dismiss_tex != NULL && a->snooze_tex != NULL);

   /* Generate chime PCM buffers */
   dawn_chime_generate(&a->chime);
   dawn_alarm_tone_generate(&a->alarm_tone);
   atomic_init(&a->sound_stop, 0);

   OLOG_INFO("initialized (%dx%d)", w, h);
   return 0;
}

void ui_alarm_cleanup(ui_alarm_t *a) {
   stop_sound_thread(a);
   dawn_chime_free(&a->chime);
   dawn_chime_free(&a->alarm_tone);

   if (a->title_tex)
      SDL_DestroyTexture(a->title_tex);
   if (a->label_tex)
      SDL_DestroyTexture(a->label_tex);
   if (a->dismiss_tex)
      SDL_DestroyTexture(a->dismiss_tex);
   if (a->snooze_tex)
      SDL_DestroyTexture(a->snooze_tex);
   if (a->title_font)
      TTF_CloseFont(a->title_font);
   if (a->label_font)
      TTF_CloseFont(a->label_font);
   if (a->btn_font)
      TTF_CloseFont(a->btn_font);
   pthread_mutex_destroy(&a->mutex);
   memset(a, 0, sizeof(*a));
}

/* =============================================================================
 * Trigger / Dismiss (thread-safe)
 * ============================================================================= */

void ui_alarm_trigger(ui_alarm_t *a, int64_t event_id, const char *label, const char *type) {
   pthread_mutex_lock(&a->mutex);

   a->event_id = event_id;
   if (label)
      snprintf(a->label, ALARM_LABEL_MAX, "%s", label);
   if (type)
      snprintf(a->type, sizeof(a->type), "%s", type);

   bool was_idle = (a->state == ALARM_STATE_IDLE);
   a->state = ALARM_STATE_ACTIVE;

   pthread_mutex_unlock(&a->mutex);
   OLOG_INFO("triggered: [%s] %s (id=%lld)", type ? type : "?", label ? label : "?",
             (long long)event_id);

   /* Start chime sound if overlay just activated */
   if (was_idle && a->audio_pb) {
      bool is_alarm = (type && strcmp(type, "alarm") == 0);
      start_sound_thread(a, is_alarm);
   }
}

void ui_alarm_dismiss(ui_alarm_t *a) {
   /* Stop sound (non-blocking signal; thread self-exits) */
   atomic_store(&a->sound_stop, 1);

   pthread_mutex_lock(&a->mutex);
   a->state = ALARM_STATE_IDLE;
   pthread_mutex_unlock(&a->mutex);
   OLOG_INFO("dismissed");
}

bool ui_alarm_is_active(const ui_alarm_t *a) {
   return a->state != ALARM_STATE_IDLE;
}

/* =============================================================================
 * Render — simple solid card, no animation
 * ============================================================================= */

/**
 * @brief Shorten a label in place so it fits within @p max_w pixels on one line.
 *
 * Scheduler messages (timers, reminders, tasks) can be a full sentence, but the
 * overlay card only has one line of room. Rather than let the texture clip
 * mid-glyph, trim the text to fit and append an ellipsis. Trimming steps back
 * one UTF-8 character at a time so multibyte sequences are never split.
 */
static void truncate_label_to_width(TTF_Font *font, char *text, size_t text_sz, int max_w) {
   int w = 0, h = 0;

   if (!font || !text || max_w <= 0)
      return;
   if (TTF_SizeUTF8(font, text, &w, &h) == 0 && w <= max_w)
      return; /* already fits */

   /* Walk backwards, replacing the tail with an ellipsis in place until the
    * "<prefix>..." form fits. Shrinking monotonically means each shorter
    * candidate overwrites only bytes past its own prefix, so earlier content is
    * never corrupted and no scratch buffer is needed. */
   size_t len = strlen(text);
   while (len > 0) {
      len--;
      /* back up to a UTF-8 character boundary (skip continuation bytes) */
      while (len > 0 && ((unsigned char)text[len] & 0xC0) == 0x80)
         len--;

      /* drop trailing spaces so the ellipsis hugs the last word */
      size_t trimmed = len;
      while (trimmed > 0 && text[trimmed - 1] == ' ')
         trimmed--;

      /* need room for "...\0" at this position */
      if (trimmed + 4 > text_sz)
         continue;

      text[trimmed] = '.';
      text[trimmed + 1] = '.';
      text[trimmed + 2] = '.';
      text[trimmed + 3] = '\0';

      if (TTF_SizeUTF8(font, text, &w, &h) == 0 && w <= max_w)
         return;
   }

   /* Nothing fit (max_w smaller than the ellipsis itself) — show just the dots. */
   snprintf(text, text_sz, "...");
}

void ui_alarm_render(ui_alarm_t *a, SDL_Renderer *r, double time_sec) {
   (void)time_sec;

   if (a->state == ALARM_STATE_IDLE)
      return;

   pthread_mutex_lock(&a->mutex);

   /* Copy alarm data under mutex */
   char label[ALARM_LABEL_MAX];
   char type[16];
   snprintf(label, sizeof(label), "%s", a->label);
   snprintf(type, sizeof(type), "%s", a->type);
   pthread_mutex_unlock(&a->mutex);

   /* Determine title text and accent color by type */
   uint8_t accent_r, accent_g, accent_b;
   const char *title_text;
   if (strcmp(type, "alarm") == 0) {
      accent_r = COLOR_THINKING_R;
      accent_g = COLOR_THINKING_G;
      accent_b = COLOR_THINKING_B;
      title_text = "ALARM";
   } else if (strcmp(type, "reminder") == 0) {
      accent_r = 0x64;
      accent_g = 0xB5;
      accent_b = 0xF6;
      title_text = "REMINDER";
   } else if (strcmp(type, "task") == 0) {
      accent_r = 0x22;
      accent_g = 0xC5;
      accent_b = 0x5E;
      title_text = "TASK COMPLETE";
   } else {
      accent_r = 0x4C;
      accent_g = 0xAF;
      accent_b = 0x50;
      title_text = "TIMER";
   }

   /* Calculate card width first — the label is trimmed to fit inside it. */
   bool can_snooze = (strcmp(type, "alarm") == 0);
   int btn_row_w = can_snooze ? (BTN_WIDTH * 2 + BTN_GAP) : BTN_WIDTH;

   int card_w = CARD_WIDTH;
   if (card_w < btn_row_w + CARD_PADDING * 2)
      card_w = btn_row_w + CARD_PADDING * 2;

   int max_w = card_w - CARD_PADDING * 2;

   /* Shorten long messages so they fit on the card's single line. */
   truncate_label_to_width(a->label_font, label, sizeof(label), max_w);

   /* Rebuild cached textures if type/label changed */
   if (strcmp(a->cached_type, type) != 0) {
      if (a->title_tex)
         SDL_DestroyTexture(a->title_tex);
      a->title_tex = ui_build_white_tex(r, a->title_font, title_text, &a->title_w, &a->title_h);
      snprintf(a->cached_type, sizeof(a->cached_type), "%s", type);
   }
   if (strcmp(a->cached_label, label) != 0) {
      if (a->label_tex)
         SDL_DestroyTexture(a->label_tex);
      a->label_tex = ui_build_white_tex(r, a->label_font, label, &a->label_w, &a->label_h);
      snprintf(a->cached_label, sizeof(a->cached_label), "%s", label);
   }

   /* Card height: padding + title + gap + label + gap + button + padding */
   int card_h = CARD_PADDING + a->title_h + 16 + a->label_h + 24 + BTN_HEIGHT + CARD_PADDING;
   int card_x = (a->screen_w - card_w) / 2;
   int card_y = (a->screen_h - card_h) / 2;

   /* Draw solid dark card background */
   roundedBoxRGBA(r, card_x, card_y, card_x + card_w - 1, card_y + card_h - 1, CARD_RADIUS,
                  CARD_BG_R, CARD_BG_G, CARD_BG_B, 255);

   /* Draw accent-colored top border (3px) */
   roundedBoxRGBA(r, card_x, card_y, card_x + card_w - 1, card_y + 3, CARD_RADIUS, accent_r,
                  accent_g, accent_b, 255);

   /* Draw title (centered, accent colored) */
   int content_y = card_y + CARD_PADDING;
   if (a->title_tex) {
      SDL_SetTextureColorMod(a->title_tex, accent_r, accent_g, accent_b);
      SDL_SetTextureAlphaMod(a->title_tex, 255);
      SDL_Rect dst = { card_x + (card_w - a->title_w) / 2, content_y, a->title_w, a->title_h };
      SDL_RenderCopy(r, a->title_tex, NULL, &dst);
   }
   content_y += a->title_h + 16;

   /* Draw label (centered, white) */
   if (a->label_tex) {
      SDL_SetTextureColorMod(a->label_tex, 0xEE, 0xEE, 0xEE);
      SDL_SetTextureAlphaMod(a->label_tex, 255);
      int draw_w = a->label_w > max_w ? max_w : a->label_w; /* safety clamp */
      SDL_Rect dst = { card_x + (card_w - draw_w) / 2, content_y, draw_w, a->label_h };
      SDL_RenderCopy(r, a->label_tex, NULL, &dst);
   }
   content_y += a->label_h + 24;

   /* Draw buttons */
   int btn_area_x = card_x + (card_w - btn_row_w) / 2;

   /* Dismiss button (red) */
   int dismiss_x = can_snooze ? btn_area_x : (card_x + (card_w - BTN_WIDTH) / 2);
   a->dismiss_btn = (SDL_Rect){ dismiss_x, content_y, BTN_WIDTH, BTN_HEIGHT };
   roundedBoxRGBA(r, dismiss_x, content_y, dismiss_x + BTN_WIDTH - 1, content_y + BTN_HEIGHT - 1,
                  BTN_RADIUS, COLOR_ERROR_R, COLOR_ERROR_G, COLOR_ERROR_B, 255);
   if (a->dismiss_tex) {
      SDL_SetTextureColorMod(a->dismiss_tex, 0xFF, 0xFF, 0xFF);
      SDL_SetTextureAlphaMod(a->dismiss_tex, 255);
      SDL_Rect tdst = { dismiss_x + (BTN_WIDTH - a->dismiss_w) / 2,
                        content_y + (BTN_HEIGHT - a->dismiss_h) / 2, a->dismiss_w, a->dismiss_h };
      SDL_RenderCopy(r, a->dismiss_tex, NULL, &tdst);
   }

   if (can_snooze) {
      /* Snooze button (dark grey) */
      int snooze_x = btn_area_x + BTN_WIDTH + BTN_GAP;
      a->snooze_btn = (SDL_Rect){ snooze_x, content_y, BTN_WIDTH, BTN_HEIGHT };
      roundedBoxRGBA(r, snooze_x, content_y, snooze_x + BTN_WIDTH - 1, content_y + BTN_HEIGHT - 1,
                     BTN_RADIUS, 0x40, 0x40, 0x50, 255);
      if (a->snooze_tex) {
         SDL_SetTextureColorMod(a->snooze_tex, 0xCC, 0xCC, 0xCC);
         SDL_SetTextureAlphaMod(a->snooze_tex, 255);
         SDL_Rect tdst = { snooze_x + (BTN_WIDTH - a->snooze_w) / 2,
                           content_y + (BTN_HEIGHT - a->snooze_h) / 2, a->snooze_w, a->snooze_h };
         SDL_RenderCopy(r, a->snooze_tex, NULL, &tdst);
      }
   } else {
      a->snooze_btn = (SDL_Rect){ 0, 0, 0, 0 };
   }
}

/* =============================================================================
 * Touch Handling
 * ============================================================================= */

bool ui_alarm_handle_tap(ui_alarm_t *a, int x, int y) {
   if (a->state != ALARM_STATE_ACTIVE)
      return false;

   SDL_Point p = { x, y };

   if (SDL_PointInRect(&p, &a->dismiss_btn)) {
      OLOG_INFO("dismiss tapped (event_id=%lld)", (long long)a->event_id);
      if (a->on_dismiss) {
         a->on_dismiss(a->event_id, a->cb_userdata);
      }
      ui_alarm_dismiss(a);
      return true;
   }

   if (SDL_PointInRect(&p, &a->snooze_btn)) {
      OLOG_INFO("snooze tapped (event_id=%lld)", (long long)a->event_id);
      if (a->on_snooze) {
         a->on_snooze(a->event_id, 0, a->cb_userdata); /* 0 = use default */
      }
      ui_alarm_dismiss(a);
      return true;
   }

   return true; /* Consume tap even outside buttons (modal) */
}

/* =============================================================================
 * Audio Playback Wiring
 * ============================================================================= */

void ui_alarm_set_audio_playback(ui_alarm_t *a, struct audio_playback *pb) {
   if (a)
      a->audio_pb = pb;
}
