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
 * Stubs for test_memory_predicate_dedup.  The tested functions
 * (memory_predicate_is_standard / _jaccard) are pure — no DB, no log.
 * But memory_predicate_dedup.c also defines memory_predicate_canonicalize
 * which calls s_db and log_message; both symbols must resolve for the
 * test binary to link, even though the tests don't exercise the
 * canonicalize path.  s_db.initialized = false ensures
 * AUTH_DB_LOCK_OR_FAIL short-circuits if canonicalize were called.
 */

#define AUTH_DB_INTERNAL_ALLOWED

#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>

#include "auth/auth_db_internal.h"

auth_db_state_t s_db = {
   .db = NULL,
   .mutex = PTHREAD_MUTEX_INITIALIZER,
   .initialized = false,
};

void log_message(int level, const char *file, int line, const char *fmt, ...) {
   (void)level;
   (void)file;
   (void)line;
   (void)fmt;
}
