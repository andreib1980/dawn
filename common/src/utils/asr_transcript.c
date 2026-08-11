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
 * ASR transcript filtering — see utils/asr_transcript.h.
 */
#include "utils/asr_transcript.h"

#include <ctype.h>
#include <stddef.h>
#include <string.h>

/* ASR silence markers.  Whisper emits "[BLANK_AUDIO]" for a recording with no
 * intelligible speech; a blank segment can be surrounded by whitespace/newlines,
 * so a substring test (not equality) is used, matching the legacy inline checks
 * this helper replaces.  Add new markers here — this is the one authoritative
 * list. */
static const char *const ASR_BLANK_MARKERS[] = {
   "[BLANK_AUDIO]",
};

bool asr_transcript_is_blank(const char *text) {
   if (text == NULL) {
      return true;
   }

   /* Empty or whitespace-only: nothing to send. */
   size_t i = 0;
   while (text[i] != '\0' && isspace((unsigned char)text[i])) {
      i++;
   }
   if (text[i] == '\0') {
      return true;
   }

   for (size_t m = 0; m < sizeof(ASR_BLANK_MARKERS) / sizeof(ASR_BLANK_MARKERS[0]); m++) {
      if (strstr(text, ASR_BLANK_MARKERS[m]) != NULL) {
         return true;
      }
   }

   return false;
}
