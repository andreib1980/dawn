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
 * Summarize-missing backfill — operator-triggered worker that finds
 * conversations without a memory_summaries row and writes one using the
 * canonical extraction prompt.  Summary-only by design: facts, entities,
 * relations, preferences, and corrections from the LLM response are
 * intentionally dropped.  Use `dawn-admin memory reextract` when you need
 * a full re-extraction.
 *
 * Reason this is a separate workstream from the live extractor: the live
 * path advances last_extracted_msg_count before storing the summary, so
 * an LLM error that drops the summary field on the floor leaves the
 * conversation marked as up-to-date with no summary forever.  Recovery
 * doesn't catch it because the extraction counter already matches.  This
 * module bypasses the counter and asks the LLM only for the summary.
 */

#ifndef MEMORY_SUMMARIZE_MISSING_H
#define MEMORY_SUMMARIZE_MISSING_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Count conversations missing a summary row for the given user.
 *
 * Matches the same criteria as the worker: non-private, message_count >= 2,
 * no row in memory_summaries with source_conversation_id == c.id.  Use this
 * for dry-run output before triggering the worker.
 *
 * @param user_id    user ID
 * @param out_count  receives the count on SUCCESS (set to 0 on FAILURE)
 * @return SUCCESS / FAILURE
 */
int memory_summarize_missing_count(int user_id, int *out_count);

/**
 * @brief Start the summarize-missing worker thread for one user.
 *
 * Spawns a detached worker that iterates conversations missing summaries,
 * loads each from the DB, runs the extraction LLM with the canonical
 * prompt, and stores only the resulting summary (with topics/title for
 * context).  Logs progress every 10 conversations.  Idempotent: a second
 * call while already running returns FAILURE.
 *
 * @param user_id    user ID (must be > 0)
 * @param max_count  cap on conversations to process this run.  0 is the
 *                   "unlimited" sentinel and is clamped server-side to a
 *                   hard cap (SUMMARIZE_HARD_CAP in the .c, currently
 *                   1000); operators with larger backlogs can chain runs.
 *                   Useful for testing or staged backfills.
 * @return SUCCESS if the worker started, FAILURE if already running / OOM /
 *         user_id invalid
 */
int memory_summarize_missing_start(int user_id, uint32_t max_count);

/**
 * @brief Check whether a summarize-missing worker is currently running.
 */
bool memory_summarize_missing_is_running(void);

/**
 * @brief Cooperatively stop a running summarize-missing worker.
 *
 * Sets a shutdown flag and joins the thread (with 5 s timeout, then cancel).
 * Safe to call when no worker is running.
 */
void memory_summarize_missing_stop(void);

#ifdef __cplusplus
}
#endif

#endif /* MEMORY_SUMMARIZE_MISSING_H */
