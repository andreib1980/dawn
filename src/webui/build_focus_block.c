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
 * Per-turn focus-block builder — Phase 1e of Dynamic Context Injection.
 * See header for the full contract.
 */

#include "webui/build_focus_block.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "config/dawn_config.h"
#include "core/strbuf.h"
#include "dawn_error.h"
#include "logging.h"
#include "memory/focus_source.h"
#include "memory/memory_embeddings.h"

/* Per-call rendering buffer initial size — sized to comfortably hold
 * the typical ~8-candidate result.  Grows via strbuf if exceeded.
 *
 * Max cap: top_k_max (64) × (FOCUS_TEXT_MAX_BYTES (4096) + framing
 * overhead per line ~96) + slack ≈ 270 KB worst case.  Practical
 * top_k default (8) × 4 KB = ~33 KB lands right at the previous 32 KB
 * cap and silently truncated under document-heavy turns; sized to
 * 64 KB to comfortably hold the typical 8-candidate document-heavy
 * case while still bounded against runaway adapter output. */
#define FOCUS_BLOCK_INIT_BYTES 4096
#define FOCUS_BLOCK_MAX_BYTES (64 * 1024)

/* Truncate user_turn_text to this many chars when building the
 * privacy-safe LOG_INFO summary.  Logs go to syslog; the daemon does
 * not log raw fact / message text by convention. */
#define FOCUS_LOG_TURN_EXCERPT_CHARS 32

static double monotonic_ms_now(void) {
   struct timespec ts;
   if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
      return 0.0;
   return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1.0e6;
}

/* Privacy-safe per-call log line — excerpts at most
 * FOCUS_LOG_TURN_EXCERPT_CHARS of the user query and NEVER the
 * candidate text content.  Extracted from inline duplicate code at
 * the empty-result and success paths so the format string is one
 * source of truth. */
static void log_focus_summary(const char *turn_text,
                              int candidates,
                              int rejections,
                              double elapsed_ms) {
   char excerpt[FOCUS_LOG_TURN_EXCERPT_CHARS + 1];
   const size_t turn_len = (turn_text != NULL) ? strlen(turn_text) : 0;
   const size_t copy = (turn_len > FOCUS_LOG_TURN_EXCERPT_CHARS) ? FOCUS_LOG_TURN_EXCERPT_CHARS
                                                                 : turn_len;
   if (copy > 0)
      memcpy(excerpt, turn_text, copy);
   excerpt[copy] = '\0';
   OLOG_INFO("focus: turn=\"%s%s\" candidates=%d rejections=%d elapsed_ms=%.1f", excerpt,
             turn_len > FOCUS_LOG_TURN_EXCERPT_CHARS ? "..." : "", candidates, rejections,
             elapsed_ms);
}

int build_focus_block(int user_id, const char *user_turn_text, char **out_block) {
   if (out_block == NULL)
      return FAILURE;
   *out_block = NULL;

   /* Snapshot the focus_injection sub-config under one read — guards
    * against TOCTOU when the WebUI settings handler mutates `enabled`
    * + `top_k` from a different thread mid-call (security audit M1).
    * Inexpensive struct copy — current shape is two ints + a few
    * floats. */
   const focus_injection_config_t fi = g_config.memory.focus_injection;

   /* Gate 1: feature flag.  ZERO embedding compute, ZERO focus_compose
    * call when disabled.  This is the absolute top-of-function gate
    * per Phase 1e invariant 6. */
   if (!fi.enabled)
      return SUCCESS;

   /* Gate 2: caller contract.  Unauthenticated callers and empty
    * turn text short-circuit silently. */
   if (user_id <= 0 || user_turn_text == NULL || user_turn_text[0] == '\0')
      return SUCCESS;

   const double t_start = monotonic_ms_now();

   /* Embed the user turn.  On failure, fall through to focus_compose
    * with NULL embedding — adapters with requires_embedding=true skip
    * themselves; calendar (requires_embedding=false) still consults.
    *
    * The 8 KB query_embed buffer must outlive the embed call all the
    * way through focus_compose (adapters read it synchronously), so
    * it stays at function scope.  The early-out below skips embedding
    * AND focus_compose vector adapters when the engine is unavailable
    * — the storage cost is paid (8 KB stack on the worker thread),
    * but the embed/compose CPU cost is not.  Default Linux pthread
    * stacks are 8 MB so the headroom is fine.  TODO(perf): if a
    * future constrained-stack worker needs this, switch to a thread-
    * local heap buffer. */
   float query_embed[MAX_EMBEDDING_DIMS];
   int embed_dims = 0;
   const float *query_ptr = NULL;
   if (memory_embeddings_available()) {
      if (memory_embeddings_embed(user_turn_text, query_embed, &embed_dims) == SUCCESS &&
          embed_dims > 0) {
         query_ptr = query_embed;
      } else {
         OLOG_WARNING("focus: embedding compute failed for user_id=%d — "
                      "passing NULL embedding to focus_compose (vector adapters skip)",
                      user_id);
         embed_dims = 0;
      }
   }
   /* Defensive clamp — security audit L2: any backend that returned a
    * negative dim would wrap to a huge size_t after the cast and walk
    * focus_compose into undefined memory. */
   if (embed_dims < 0)
      embed_dims = 0;

   const time_t now = time(NULL);
   const int per_source_max = fi.top_k > 0 ? fi.top_k : 8;

   focus_compose_result_t result = { 0 };
   const int rc = focus_compose(user_id, /*include_private*/ false, user_turn_text, query_ptr,
                                (size_t)embed_dims, now, per_source_max, &result);
   if (rc != SUCCESS) {
      OLOG_WARNING("focus: focus_compose failed (user_id=%d)", user_id);
      focus_result_free(&result);
      return FAILURE;
   }

   /* Empty result is a successful no-op — composer omits the section. */
   if (result.candidate_count <= 0) {
      log_focus_summary(user_turn_text, 0, result.rejection_count, monotonic_ms_now() - t_start);
      focus_result_free(&result);
      return SUCCESS;
   }

   /* Render survivors.  Format: one line per candidate, opening with
    * the source_id in brackets so the LLM can attribute relevance.
    * Text content is reproduced verbatim from the candidate (already
    * truncated to FOCUS_TEXT_MAX_BYTES inside the framework). */
   strbuf_t sb;
   strbuf_init_with_max(&sb, FOCUS_BLOCK_INIT_BYTES, FOCUS_BLOCK_MAX_BYTES);
   for (int i = 0; i < result.candidate_count; i++) {
      const focus_candidate_t *c = &result.candidates[i];
      if (c->text == NULL || c->text[0] == '\0')
         continue;
      if (strbuf_appendf(&sb, "[%s] %s\n", c->source_id, c->text) < 0) {
         /* strbuf max-cap hit — stop appending; surface the partial
          * block so the LLM still sees the highest-ranked items. */
         OLOG_WARNING("focus: strbuf max cap reached at candidate %d/%d — truncating", i,
                      result.candidate_count);
         break;
      }
   }

   /* Lift ownership of the strbuf-internal buffer into out_block.
    * Use _or_null variant so an empty buffer (every candidate had
    * empty text — pathological but possible) collapses to NULL and
    * the composer omits the section.  strbuf_steal resets the buf
    * to its zero state, so strbuf_free is safe + a no-op afterward. */
   *out_block = strbuf_steal_or_null(&sb);
   strbuf_free(&sb);

   /* Per-source rejection logging — fires only when the framework
    * filter caught poison in adapter output. */
   for (int i = 0; i < result.rejection_count; i++) {
      const focus_filter_rejection_t *r = &result.rejections[i];
      if (r->count > 0)
         OLOG_WARNING("focus: filter rejected %d candidate(s) from source='%s' (user_id=%d)",
                      r->count, r->source_id, user_id);
   }

   log_focus_summary(user_turn_text, result.candidate_count, result.rejection_count,
                     monotonic_ms_now() - t_start);

   focus_result_free(&result);
   return SUCCESS;
}
