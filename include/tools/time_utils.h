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
 * Time Utilities - Common time functions shared across modules
 */

#ifndef TIME_UTILS_H
#define TIME_UTILS_H

#include <stdint.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Get current time in milliseconds (monotonic clock)
 *
 * Uses CLOCK_MONOTONIC for consistent timing that isn't affected by
 * system time changes. Ideal for measuring elapsed time and timeouts.
 *
 * Thread Safety: This function is thread-safe.
 *
 * @return Current monotonic time in milliseconds
 */
static inline uint64_t get_time_ms(void) {
   struct timespec ts;
   clock_gettime(CLOCK_MONOTONIC, &ts);
   return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

/**
 * @brief Parse a human-readable time period string into seconds
 *
 * Supports formats like "1h", "24h", "7d", "2w", "30m".
 * Units: h=hours, d=days, w=weeks, m=minutes (case insensitive)
 * If no unit is specified, defaults to hours.
 *
 * Security: Uses bounded integer parsing, rejects negative values.
 *
 * Thread Safety: This function is thread-safe (pure function).
 *
 * @param period Time period string (e.g., "24h", "7d", "1w", "1y")
 * @return Number of seconds, or 0 if invalid/empty input
 *
 * Examples:
 *   "1h"  -> 3600
 *   "24h" -> 86400
 *   "7d"  -> 604800
 *   "2w"  -> 1209600
 *   "30m" -> 1800
 *   "1y"  -> 31536000
 *   "10y" -> 315360000
 *   "12"  -> 43200 (defaults to hours)
 *   ""    -> 0
 *   "-5d" -> 0 (negative rejected)
 */
static inline time_t parse_time_period(const char *period) {
   if (!period || !period[0]) {
      return 0;
   }

   /* Skip leading whitespace */
   while (*period == ' ' || *period == '\t') {
      period++;
   }

   /* Reject negative values */
   if (*period == '-') {
      return 0;
   }

   /* Parse the numeric value (bounded to prevent overflow) */
   long value = 0;
   const char *p = period;
   while (*p >= '0' && *p <= '9') {
      value = value * 10 + (*p - '0');
      if (value > 365 * 10) { /* Cap at ~10 years to prevent overflow */
         return 0;
      }
      p++;
   }

   if (value <= 0) {
      return 0;
   }

   /* Skip whitespace between number and unit */
   while (*p == ' ' || *p == '\t') {
      p++;
   }

   /* Determine multiplier based on unit */
   time_t multiplier;
   switch (*p) {
      case 'm':
      case 'M':
         multiplier = 60; /* minutes */
         break;
      case 'h':
      case 'H':
      case '\0':            /* default to hours if no unit */
         multiplier = 3600; /* hours */
         break;
      case 'd':
      case 'D':
         multiplier = 86400; /* days */
         break;
      case 'w':
      case 'W':
         multiplier = 604800; /* weeks */
         break;
      case 'y':
      case 'Y':
         multiplier = 31536000; /* years (365 days; not leap-year-aware — fine for
                                 * approximate windowing) */
         break;
      default:
         return 0; /* unknown unit */
   }

   return (time_t)value * multiplier;
}

#ifdef __cplusplus
}
#endif

#endif /* TIME_UTILS_H */
