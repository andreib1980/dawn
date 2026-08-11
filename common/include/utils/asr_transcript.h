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
 * ASR transcript filtering — shared by the daemon and the satellite.
 *
 * Whisper (and Vosk) return a placeholder marker such as "[BLANK_AUDIO]" when a
 * recording contains no intelligible speech (the user pressed the audio button
 * but nothing was heard).  Such a result must NOT be dispatched to the LLM.  This
 * is the single place the "nothing worth sending" test lives, so every capture
 * path (local mic, WebUI browser push-to-talk, Tier-2 satellite, always-on) and
 * any future silence marker are handled uniformly.
 */
#ifndef UTILS_ASR_TRANSCRIPT_H
#define UTILS_ASR_TRANSCRIPT_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief True if an ASR transcript is blank — nothing worth sending to the LLM.
 *
 * Returns true when @p text is NULL, empty, whitespace-only, or contains a known
 * ASR silence marker (e.g. "[BLANK_AUDIO]").  Callers use this to drop the turn
 * instead of passing silence to the LLM.
 *
 * @param text The ASR transcript (may be NULL).
 * @return true if blank/silence and should be discarded; false if it carries
 *         real content.
 */
bool asr_transcript_is_blank(const char *text);

#ifdef __cplusplus
}
#endif

#endif /* UTILS_ASR_TRANSCRIPT_H */
