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
 * Note-extraction guard (Phase 9 of the notes/reference-text store).
 *
 * Keeps reference text the user filed via the `document_manage`
 * save_note/save_text tool out of session-end memory extraction, so the
 * canonical text lives only in the document store and is never double-stored as
 * a semantic fact that then drifts stale when the note is edited.  At extraction
 * time it redacts (on copies — never the live conversation history) the filed
 * bodies wherever they appear (the tool-call arguments and the originating user
 * message) plus the echoed bodies in `document_read` / `document_search` tool
 * RESULTS, which would otherwise re-inject the text in a later extraction
 * window.  Exact normalized-substring matching only, so zero false positives.
 *
 * Handles both provider history shapes: OpenAI (`tool_calls[].function.{name,
 * arguments}`, `role:"tool"` results) and Claude (`content[]` `tool_use`/
 * `tool_result` blocks).
 */

#ifndef MEMORY_NOTE_GUARD_H
#define MEMORY_NOTE_GUARD_H

#include <json-c/json.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque collected state for one extraction pass. */
typedef struct memory_note_guard memory_note_guard_t;

/**
 * @brief Scan the FULL conversation history and build a guard.
 *
 * Collects the bodies filed via `document_manage` save_note/save_text (both
 * provider shapes) and the call-ids of `document_read`/`document_search` calls
 * whose results echo stored text.  Scanning the full history (not just the new
 * extraction window) covers a body filed in a prior window that is re-quoted in
 * a new message.
 *
 * Honors the `[memory] note_extraction_guard` config flag: when disabled,
 * returns NULL (a NULL guard makes memory_note_guard_redact() a pass-through).
 *
 * @param conversation_history JSON array of message objects (not mutated).
 * @return Owned guard, or NULL when the guard is disabled or on allocation
 *         failure.  In every NULL case the caller simply skips redaction.
 */
memory_note_guard_t *memory_note_guard_create(struct json_object *conversation_history);

/**
 * @brief True if the guard collected anything to redact.
 *
 * NULL-safe (returns false).  A cheap gate for diagnostics/logging.
 */
bool memory_note_guard_active(const memory_note_guard_t *guard);

/**
 * @brief Return a redacted view of @p msg for extraction.
 *
 * Never mutates @p msg (it is a live reference into the session's conversation
 * history): returns an owned (+1) reference to a freshly-built copy when
 * anything is redacted, or a shared (+1) reference to @p msg when nothing
 * matches.  The caller adds the result to its array unconditionally and owns
 * one reference.  NULL guard is a pass-through (shares @p msg).
 */
struct json_object *memory_note_guard_redact(const memory_note_guard_t *guard,
                                             struct json_object *msg);

/**
 * @brief Free a guard.  NULL-safe.
 */
void memory_note_guard_free(memory_note_guard_t *guard);

#ifdef __cplusplus
}
#endif

#endif /* MEMORY_NOTE_GUARD_H */
