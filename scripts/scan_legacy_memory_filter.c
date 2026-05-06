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
 * One-time legacy-data scanner for the memory injection filter.
 *
 * Facts, entities, and summaries stored before the filter shipped (April
 * 2026) were not retroactively checked. This standalone tool runs each
 * row's text fields through memory_filter_check() — the production filter
 * — and reports matches per table with sample IDs and snippets.
 *
 * Status: ad-hoc admin tool — not part of the CMake build, not invoked at
 * daemon runtime. Build with the standalone gcc one-liner below.
 *
 * Privacy note: sample snippets (≤80 chars per match) of fact/entity/
 * summary text print to stdout. memory_facts text may contain PII —
 * health, relationships, locations — so do not redirect this tool's
 * stdout to a shared log or paste the report into a chat without
 * reviewing.  Match IDs alone are enough to locate rows in the DB.
 *
 * Defaults to dry-run. Pass --delete-matches --confirm-delete to actually
 * delete; deletes run inside a single transaction per table so a SQLite
 * error aborts cleanly without a partial purge.
 *
 * Build (from project root):
 *
 *   gcc -O2 -Wall -Wextra \
 *       -I include -I common/include \
 *       scripts/scan_legacy_memory_filter.c \
 *       src/memory/memory_filter.c \
 *       common/src/logging.c \
 *       -lsqlite3 -lpthread \
 *       -o scan_legacy_memory_filter
 *
 * Run (read-only by default):
 *
 *   ./scan_legacy_memory_filter --db /var/lib/dawn/auth.db --user-id 1
 *   ./scan_legacy_memory_filter --db /var/lib/dawn/auth.db --all-users
 *
 * The filter is per-text not per-user, but reports group by user_id so the
 * developer can target a single user for cleanup.
 */

#include <getopt.h>
#include <inttypes.h>
#include <limits.h>
#include <sqlite3.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "memory/memory_filter.h"

#define DEFAULT_DB_PATH "/var/lib/dawn/auth.db"
#define MAX_SAMPLE_PRINT 5
#define SNIPPET_MAX 80

/* Dynamic array of matched IDs — sized for batch DELETE in a transaction. */
typedef struct {
   long long *ids;
   int *user_ids;
   int count;
   int cap;
} id_list_t;

typedef struct {
   const char *table_name;
   int matches;
   int scanned;
   long long sample_ids[MAX_SAMPLE_PRINT];
   char sample_snip[MAX_SAMPLE_PRINT][SNIPPET_MAX + 8];
   int sample_user[MAX_SAMPLE_PRINT];
   int sample_count;
   id_list_t hits;
} table_stats_t;

static int id_list_push(id_list_t *l, long long id, int user_id) {
   if (l->count == l->cap) {
      /* Cap doubling at INT_MAX/2 prevents `cap * 2` from overflowing.  At our
       * scale (memory_facts ~thousands) the loop never approaches this, but
       * the wrap was still a real bug in the prior pattern. */
      if (l->cap >= INT_MAX / 2) {
         return 1;
      }
      int new_cap = l->cap ? l->cap * 2 : 64;

      /* On realloc failure, the previous pointer is unchanged per ISO C.  We
       * assign back directly: a successful first realloc + a failed second
       * leaves the first pointer dangling if we tried to free it (the old
       * memory is already freed by the successful realloc).  CLI scanner
       * exits on OOM, so we don't try to roll back the partial growth — the
       * caller propagates the error and main() exits cleanly via id_list_free
       * on the still-valid (just-grown-but-not-fully) buffers. */
      long long *new_ids = (long long *)realloc(l->ids, sizeof(*new_ids) * (size_t)new_cap);
      if (!new_ids) {
         return 1;
      }
      l->ids = new_ids;

      int *new_users = (int *)realloc(l->user_ids, sizeof(*new_users) * (size_t)new_cap);
      if (!new_users) {
         return 1;
      }
      l->user_ids = new_users;
      l->cap = new_cap;
   }
   l->ids[l->count] = id;
   l->user_ids[l->count] = user_id;
   l->count++;
   return 0;
}

static void id_list_free(id_list_t *l) {
   free(l->ids);
   free(l->user_ids);
   l->ids = NULL;
   l->user_ids = NULL;
   l->count = 0;
   l->cap = 0;
}

static void usage(const char *argv0) {
   fprintf(stderr,
           "Usage: %s [options]\n"
           "  --db PATH              Path to auth.db (default: %s)\n"
           "  --user-id N            Limit scan to user_id N\n"
           "  --all-users            Scan all users (mutually exclusive with --user-id)\n"
           "  --table NAME           One of: facts, entities, summaries, all (default: all)\n"
           "  --delete-matches       Actually DELETE matching rows (default: dry-run)\n"
           "  --confirm-delete       Required alongside --delete-matches\n"
           "  --help                 Show this message\n",
           argv0, DEFAULT_DB_PATH);
}

static void record_sample(table_stats_t *s, long long id, int user_id, const char *text) {
   if (s->sample_count >= MAX_SAMPLE_PRINT)
      return;
   s->sample_ids[s->sample_count] = id;
   s->sample_user[s->sample_count] = user_id;
   const char *src = text ? text : "(null)";
   size_t n = strlen(src);
   if (n > SNIPPET_MAX) {
      memcpy(s->sample_snip[s->sample_count], src, SNIPPET_MAX);
      s->sample_snip[s->sample_count][SNIPPET_MAX] = '\0';
      strncat(s->sample_snip[s->sample_count], "...", 4);
   } else {
      memcpy(s->sample_snip[s->sample_count], src, n);
      s->sample_snip[s->sample_count][n] = '\0';
   }
   /* The data we're printing is by definition adversary-influenced — the
    * scanner exists *because* legacy data bypassed the filter — so scrub
    * the full C0 control range + DEL.  Plain ASCII space replaces ESC,
    * BEL, BS, CSI lead bytes, and friends so a flagged fact can't drive
    * the operator's terminal.  UTF-8 high bytes pass through unchanged
    * (sufficient for unicode-as-content, not as escape transport). */
   for (unsigned char *p = (unsigned char *)s->sample_snip[s->sample_count]; *p; p++) {
      if (*p < 0x20 || *p == 0x7F)
         *p = ' ';
   }
   s->sample_count++;
}

static int scan_table(sqlite3 *db,
                      table_stats_t *s,
                      const char *select_sql,
                      bool match_combined,
                      int filter_user_id) {
   sqlite3_stmt *stmt = NULL;
   int rc = sqlite3_prepare_v2(db, select_sql, -1, &stmt, NULL);
   if (rc != SQLITE_OK) {
      fprintf(stderr, "prepare failed for %s: %s\n", s->table_name, sqlite3_errmsg(db));
      return 1;
   }
   if (filter_user_id > 0)
      sqlite3_bind_int(stmt, 1, filter_user_id);

   while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
      s->scanned++;
      long long id = sqlite3_column_int64(stmt, 0);
      int user_id = sqlite3_column_int(stmt, 1);
      const char *text2 = (const char *)sqlite3_column_text(stmt, 2);
      bool flagged = false;
      const char *flagged_text = text2;
      if (text2 && memory_filter_check(text2)) {
         flagged = true;
      }
      if (!flagged && match_combined) {
         const char *text3 = (const char *)sqlite3_column_text(stmt, 3);
         if (text3 && memory_filter_check(text3)) {
            flagged = true;
            flagged_text = text3;
         }
      }
      if (flagged) {
         s->matches++;
         record_sample(s, id, user_id, flagged_text);
         if (id_list_push(&s->hits, id, user_id) != 0) {
            fprintf(stderr, "OOM tracking matches for %s\n", s->table_name);
            sqlite3_finalize(stmt);
            return 1;
         }
      }
   }

   if (rc != SQLITE_DONE) {
      fprintf(stderr, "step failed for %s: %s\n", s->table_name, sqlite3_errmsg(db));
      sqlite3_finalize(stmt);
      return 1;
   }
   sqlite3_finalize(stmt);
   return 0;
}

static void print_table_report(const table_stats_t *s) {
   printf("\n[%s]\n", s->table_name);
   printf("  scanned: %d  matches: %d  (%.2f%%)\n", s->scanned, s->matches,
          s->scanned ? 100.0 * (double)s->matches / (double)s->scanned : 0.0);
   if (s->sample_count > 0) {
      printf("  samples (first %d):\n", s->sample_count);
      for (int i = 0; i < s->sample_count; i++) {
         printf("    user=%d id=%lld  %s\n", s->sample_user[i], s->sample_ids[i],
                s->sample_snip[i]);
      }
   }
}

/* Delete every matched id within a single transaction.
 *
 * Defense-in-depth: the table name is `snprintf`'d into a DELETE statement.
 * All callers in this file pass hard-coded literals ("memory_facts",
 * "memory_entities", "memory_summaries"), so the interpolation is not a
 * SQLi sink today.  The allowlist below makes that contract explicit so a
 * future caller can't accidentally feed user-influenced text into the
 * DELETE path. */
static int delete_table_hits(sqlite3 *db, const char *table, const id_list_t *hits) {
   if (hits->count == 0)
      return 0;
   if (!table || (strcmp(table, "memory_facts") != 0 && strcmp(table, "memory_entities") != 0 &&
                  strcmp(table, "memory_summaries") != 0)) {
      fprintf(stderr, "delete_table_hits: refusing to operate on unknown table '%s'\n",
              table ? table : "(null)");
      return 1;
   }

   char *err_msg = NULL;
   int rc = sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, &err_msg);
   if (rc != SQLITE_OK) {
      fprintf(stderr, "BEGIN failed for %s: %s\n", table, err_msg ? err_msg : "?");
      sqlite3_free(err_msg);
      return 1;
   }

   char delete_sql[128];
   snprintf(delete_sql, sizeof(delete_sql), "DELETE FROM %s WHERE id = ?1;", table);
   sqlite3_stmt *del = NULL;
   rc = sqlite3_prepare_v2(db, delete_sql, -1, &del, NULL);
   if (rc != SQLITE_OK) {
      fprintf(stderr, "prepare DELETE failed for %s: %s\n", table, sqlite3_errmsg(db));
      sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
      return 1;
   }

   int deleted = 0;
   for (int i = 0; i < hits->count; i++) {
      sqlite3_bind_int64(del, 1, hits->ids[i]);
      rc = sqlite3_step(del);
      if (rc != SQLITE_DONE) {
         fprintf(stderr, "DELETE %s id=%lld failed: %s — rolling back\n", table, hits->ids[i],
                 sqlite3_errmsg(db));
         sqlite3_finalize(del);
         sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
         return 1;
      }
      deleted += sqlite3_changes(db);
      sqlite3_reset(del);
   }
   sqlite3_finalize(del);

   rc = sqlite3_exec(db, "COMMIT", NULL, NULL, &err_msg);
   if (rc != SQLITE_OK) {
      fprintf(stderr, "COMMIT failed for %s: %s\n", table, err_msg ? err_msg : "?");
      sqlite3_free(err_msg);
      sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
      return 1;
   }
   printf("  deleted: %d row%s from %s\n", deleted, deleted == 1 ? "" : "s", table);
   return 0;
}

int main(int argc, char *argv[]) {
   const char *db_path = DEFAULT_DB_PATH;
   int user_id = 0;
   bool all_users = false;
   const char *table_choice = "all";
   bool delete_matches_flag = false;
   bool confirm_delete = false;

   static struct option long_opts[] = {
      { "db", required_argument, 0, 'd' },       { "user-id", required_argument, 0, 'u' },
      { "all-users", no_argument, 0, 'a' },      { "table", required_argument, 0, 't' },
      { "delete-matches", no_argument, 0, 'D' }, { "confirm-delete", no_argument, 0, 'C' },
      { "help", no_argument, 0, 'h' },           { 0, 0, 0, 0 },
   };
   int opt;
   while ((opt = getopt_long(argc, argv, "", long_opts, NULL)) != -1) {
      switch (opt) {
         case 'd':
            db_path = optarg;
            break;
         case 'u': {
            /* atoi silently maps "1foo" → 1 and "abc" → 0; the latter then
             * passes the `user_id <= 0 && !all_users` guard if --all-users
             * happens to be set, so a typo could broaden the destructive
             * scope.  strtol with end-pointer validation rejects both. */
            char *endp = NULL;
            long v = strtol(optarg, &endp, 10);
            if (!optarg[0] || !endp || *endp != '\0' || v <= 0 || v > INT_MAX) {
               fprintf(stderr, "error: --user-id requires a positive integer\n");
               return 2;
            }
            user_id = (int)v;
            break;
         }
         case 'a':
            all_users = true;
            break;
         case 't':
            table_choice = optarg;
            break;
         case 'D':
            delete_matches_flag = true;
            break;
         case 'C':
            confirm_delete = true;
            break;
         case 'h':
            usage(argv[0]);
            return 0;
         default:
            usage(argv[0]);
            return 2;
      }
   }
   if (user_id <= 0 && !all_users) {
      fprintf(stderr, "error: must specify --user-id N or --all-users\n");
      usage(argv[0]);
      return 2;
   }
   if (user_id > 0 && all_users) {
      fprintf(stderr, "error: --user-id and --all-users are mutually exclusive\n");
      return 2;
   }
   if (delete_matches_flag && !confirm_delete) {
      fprintf(stderr, "error: --delete-matches requires --confirm-delete\n");
      return 2;
   }

   sqlite3 *db = NULL;
   int open_flags = delete_matches_flag ? SQLITE_OPEN_READWRITE : SQLITE_OPEN_READONLY;
   int rc = sqlite3_open_v2(db_path, &db, open_flags, NULL);
   if (rc != SQLITE_OK) {
      fprintf(stderr, "open %s failed: %s\n", db_path, db ? sqlite3_errmsg(db) : "unknown");
      if (db)
         sqlite3_close(db);
      return 1;
   }

   bool do_facts = strcmp(table_choice, "all") == 0 || strcmp(table_choice, "facts") == 0;
   bool do_entities = strcmp(table_choice, "all") == 0 || strcmp(table_choice, "entities") == 0;
   bool do_summaries = strcmp(table_choice, "all") == 0 || strcmp(table_choice, "summaries") == 0;

   table_stats_t facts_s = { .table_name = "memory_facts" };
   table_stats_t entities_s = { .table_name = "memory_entities" };
   table_stats_t summaries_s = { .table_name = "memory_summaries" };

   const char *facts_all = "SELECT id, user_id, fact_text FROM memory_facts ORDER BY id";
   const char *facts_user = "SELECT id, user_id, fact_text FROM memory_facts "
                            "WHERE user_id = ?1 ORDER BY id";
   /* memory_entities: name + canonical_name. entity_type is structured (LLM
    * enum) and not user-controllable, so skip it. */
   const char *ents_all = "SELECT id, user_id, name, canonical_name "
                          "FROM memory_entities ORDER BY id";
   const char *ents_user = "SELECT id, user_id, name, canonical_name "
                           "FROM memory_entities WHERE user_id = ?1 ORDER BY id";
   /* memory_summaries: summary + topics. topics is TEXT (comma-joined or
    * JSON-list — pass through as a whole string). */
   const char *sums_all = "SELECT id, user_id, summary, topics "
                          "FROM memory_summaries ORDER BY id";
   const char *sums_user = "SELECT id, user_id, summary, topics "
                           "FROM memory_summaries WHERE user_id = ?1 ORDER BY id";

   printf("=== legacy memory filter scan ===\n");
   printf("  db:        %s\n", db_path);
   printf("  scope:     %s\n", all_users ? "all users" : "single user");
   if (!all_users)
      printf("  user_id:   %d\n", user_id);
   printf("  tables:    %s\n", table_choice);
   printf("  delete:    %s\n", delete_matches_flag ? "ENABLED" : "dry-run");

   int err = 0;
   if (do_facts)
      err |= scan_table(db, &facts_s, all_users ? facts_all : facts_user, false,
                        all_users ? 0 : user_id);
   if (do_entities)
      err |= scan_table(db, &entities_s, all_users ? ents_all : ents_user, true,
                        all_users ? 0 : user_id);
   if (do_summaries)
      err |= scan_table(db, &summaries_s, all_users ? sums_all : sums_user, true,
                        all_users ? 0 : user_id);

   if (err) {
      sqlite3_close(db);
      id_list_free(&facts_s.hits);
      id_list_free(&entities_s.hits);
      id_list_free(&summaries_s.hits);
      return 1;
   }

   if (do_facts)
      print_table_report(&facts_s);
   if (do_entities)
      print_table_report(&entities_s);
   if (do_summaries)
      print_table_report(&summaries_s);

   int total_matches = facts_s.matches + entities_s.matches + summaries_s.matches;
   int total_scanned = facts_s.scanned + entities_s.scanned + summaries_s.scanned;
   printf("\nTOTAL: %d matches across %d scanned rows.\n", total_matches, total_scanned);

   if (delete_matches_flag) {
      printf("\n--delete-matches enabled (per-table transactions):\n");
      if (do_facts)
         err |= delete_table_hits(db, "memory_facts", &facts_s.hits);
      if (do_entities)
         err |= delete_table_hits(db, "memory_entities", &entities_s.hits);
      if (do_summaries)
         err |= delete_table_hits(db, "memory_summaries", &summaries_s.hits);
   } else if (total_matches > 0) {
      printf("\n(dry-run; pass --delete-matches --confirm-delete to remove "
             "the %d row%s above)\n",
             total_matches, total_matches == 1 ? "" : "s");
   }

   id_list_free(&facts_s.hits);
   id_list_free(&entities_s.hits);
   id_list_free(&summaries_s.hits);
   sqlite3_close(db);
   return err ? 1 : 0;
}
