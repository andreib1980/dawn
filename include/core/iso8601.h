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
 * ISO 8601 absolute timestamp parser — shared by tools that accept
 * date/time arguments from the LLM (scheduler, calendar, etc.).
 *
 * Distinct from time_query_parser, which extracts fuzzy temporal
 * expressions from natural-language search queries.  This module parses
 * concrete RFC 3339 / ISO 8601 strings into Unix timestamps.
 */

#ifndef ISO8601_H
#define ISO8601_H

#include <stdint.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Parse an ISO 8601 timestamp into a Unix timestamp.
 *
 * Accepted formats:
 *   - "2026-02-19"                  (date only — midnight local)
 *   - "2026-02-19T15:30:00"         (no timezone — interpreted as local)
 *   - "2026-02-19T15:30:00Z"        (UTC)
 *   - "2026-02-19T15:30:00-05:00"   (explicit offset)
 *   - "15:30" or "07:00"            (time only — today, or tomorrow if past)
 *
 * Thread-safe: relies on the process-wide TZ set once at startup from
 * g_config.localization.timezone.  Explicit timezone offsets are parsed
 * arithmetically.
 *
 * @param iso_str Input string.  May be NULL (returns -1).
 * @return Unix timestamp on success, or (time_t)-1 on parse error.
 *         The -1 sentinel mirrors mktime()'s contract.
 */
time_t iso8601_parse(const char *iso_str);

/**
 * @brief Parse a date-only ISO 8601 string as midnight UTC.
 *
 * Accepted formats:
 *   - "YYYY-MM-DD"  — midnight UTC of that date
 *   - "YYYY"        — January 1 midnight UTC of that year
 *
 * Distinct from iso8601_parse() in three ways:
 *   - UTC interpretation only (no local-TZ ambiguity)
 *   - Date-only input (no time-of-day, no offsets)
 *   - Returns 0 on parse failure / NULL input — callers treat 0 as
 *     "no bound" (e.g., open-ended valid_to in bitemporal relations)
 *
 * Used for date bounds stored in SQLite INTEGER columns where the 0
 * sentinel naturally represents "unset".
 *
 * @param iso_str Input string.  May be NULL (returns 0).
 * @return Unix timestamp at 00:00:00 UTC for the parsed date,
 *         or 0 on parse failure.
 */
int64_t iso8601_parse_date_utc(const char *iso_str);

#ifdef __cplusplus
}
#endif

#endif /* ISO8601_H */
