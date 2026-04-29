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
 * Unit tests for music_db.c — search, dedup, browse, source abstraction.
 * Uses a temp-file SQLite database with direct SQL inserts for test data.
 */

#include <sqlite3.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "audio/audio_decoder.h"
#include "audio/music_db.h"
#include "audio/music_source.h"

/* =============================================================================
 * Stubs for symbols referenced by music_db.c but not exercised in tests
 * ============================================================================= */

/* audio_decoder_get_extensions() — called by is_supported_extension() in scan path only */
static const char *stub_extensions[] = { ".flac", ".mp3", ".ogg", NULL };
const char **audio_decoder_get_extensions(void) {
   return (const char **)stub_extensions;
}

/* audio_decoder_get_metadata() — called during scan, which we never invoke */
int audio_decoder_get_metadata(const char *path, audio_metadata_t *metadata) {
   (void)path;
   (void)metadata;
   return 0;
}

/* =============================================================================
 * Unity Test Framework
 * ============================================================================= */

#include "unity.h"

/* =============================================================================
 * Setup / Teardown
 * ============================================================================= */

static char g_db_path[256];
static sqlite3 *g_test_db = NULL; /* Second handle for inserting test data */

static void setup_db(void) {
   snprintf(g_db_path, sizeof(g_db_path), "/tmp/test_music_db_XXXXXX");
   int fd = mkstemp(g_db_path);
   if (fd < 0) {
      fprintf(stderr, "mkstemp failed\n");
      exit(1);
   }
   close(fd);

   int rc = music_db_init(g_db_path);
   if (rc != 0) {
      fprintf(stderr, "music_db_init failed: %d\n", rc);
      exit(1);
   }

   rc = sqlite3_open(g_db_path, &g_test_db);
   if (rc != SQLITE_OK) {
      fprintf(stderr, "sqlite3_open test handle failed\n");
      exit(1);
   }
   sqlite3_busy_timeout(g_test_db, 5000);
}

static void teardown_db(void) {
   if (g_test_db) {
      sqlite3_close(g_test_db);
      g_test_db = NULL;
   }
   music_db_cleanup();
   unlink(g_db_path);
}

/** Insert a track row directly via the test handle */
static void insert_track(const char *path,
                         const char *title,
                         const char *artist,
                         const char *album,
                         const char *genre,
                         int source,
                         int duration) {
   const char *sql = "INSERT OR REPLACE INTO music_metadata "
                     "(path, mtime, title, artist, album, genre, source, duration_sec) "
                     "VALUES (?, 1000, ?, ?, ?, ?, ?, ?)";
   sqlite3_stmt *stmt = NULL;
   sqlite3_prepare_v2(g_test_db, sql, -1, &stmt, NULL);
   sqlite3_bind_text(stmt, 1, path, -1, SQLITE_STATIC);
   sqlite3_bind_text(stmt, 2, title, -1, SQLITE_STATIC);
   sqlite3_bind_text(stmt, 3, artist, -1, SQLITE_STATIC);
   sqlite3_bind_text(stmt, 4, album, -1, SQLITE_STATIC);
   sqlite3_bind_text(stmt, 5, genre, -1, SQLITE_STATIC);
   sqlite3_bind_int(stmt, 6, source);
   sqlite3_bind_int(stmt, 7, duration);
   sqlite3_step(stmt);
   sqlite3_finalize(stmt);
}

/** Clear all rows between test groups */
static void clear_tracks(void) {
   sqlite3_exec(g_test_db, "DELETE FROM music_metadata", NULL, NULL, NULL);
}

/* =============================================================================
 * Group 1: music_source abstraction (no DB needed)
 * ============================================================================= */

static void test_source_names(void) {
   printf("\n--- test_source_names ---\n");
   TEST_ASSERT_TRUE_MESSAGE(strcmp(music_source_name(MUSIC_SOURCE_LOCAL), "local") == 0,
                            "LOCAL source name is 'local'");
   TEST_ASSERT_TRUE_MESSAGE(strcmp(music_source_name(MUSIC_SOURCE_PLEX), "plex") == 0,
                            "PLEX source name is 'plex'");
   TEST_ASSERT_TRUE_MESSAGE(strcmp(music_source_name(99), "unknown") == 0,
                            "Invalid source name is 'unknown'");
}

static void test_source_prefixes(void) {
   printf("\n--- test_source_prefixes ---\n");
   TEST_ASSERT_TRUE_MESSAGE(strcmp(music_source_path_prefix(MUSIC_SOURCE_LOCAL), "") == 0,
                            "LOCAL prefix is empty");
   TEST_ASSERT_TRUE_MESSAGE(strcmp(music_source_path_prefix(MUSIC_SOURCE_PLEX), "plex:") == 0,
                            "PLEX prefix is 'plex:'");
   TEST_ASSERT_TRUE_MESSAGE(strcmp(music_source_path_prefix(99), "") == 0,
                            "Invalid source prefix is empty");
}

static void test_source_from_path(void) {
   printf("\n--- test_source_from_path ---\n");
   TEST_ASSERT_TRUE_MESSAGE(music_source_from_path("/home/user/Music/song.flac") ==
                                MUSIC_SOURCE_LOCAL,
                            "Local path identified as LOCAL");
   TEST_ASSERT_TRUE_MESSAGE(music_source_from_path("plex:/library/parts/123/file.mp3") ==
                                MUSIC_SOURCE_PLEX,
                            "plex: path identified as PLEX");
   TEST_ASSERT_TRUE_MESSAGE(music_source_from_path(NULL) == MUSIC_SOURCE_LOCAL,
                            "NULL path returns LOCAL");
}

/* =============================================================================
 * Group 2: Init and schema
 * ============================================================================= */

static void test_init_cleanup(void) {
   printf("\n--- test_init_cleanup ---\n");

   /* Already initialized by setup_db() */
   TEST_ASSERT_TRUE_MESSAGE(music_db_is_initialized() == true, "DB is initialized after init");

   /* Cleanup */
   music_db_cleanup();
   TEST_ASSERT_TRUE_MESSAGE(music_db_is_initialized() == false, "DB not initialized after cleanup");

   /* Double cleanup is safe */
   music_db_cleanup();
   TEST_ASSERT_TRUE_MESSAGE(music_db_is_initialized() == false, "Double cleanup is safe");

   /* Re-init for remaining tests */
   int rc = music_db_init(g_db_path);
   TEST_ASSERT_TRUE_MESSAGE(rc == 0, "Re-init succeeds");
   TEST_ASSERT_TRUE_MESSAGE(music_db_is_initialized() == true, "DB initialized after re-init");
}

static void test_schema_migration_idempotent(void) {
   printf("\n--- test_schema_migration_idempotent ---\n");

   /* Calling init again on same DB should succeed (idempotent) */
   music_db_cleanup();
   int rc = music_db_init(g_db_path);
   TEST_ASSERT_TRUE_MESSAGE(rc == 0, "Second init on same DB succeeds");
   TEST_ASSERT_TRUE_MESSAGE(music_db_is_initialized() == true, "DB initialized after second init");
}

/* =============================================================================
 * Group 3: Search with dedup
 * ============================================================================= */

static void test_search_basic(void) {
   printf("\n--- test_search_basic ---\n");
   clear_tracks();

   insert_track("/music/a.flac", "Song A", "Artist One", "Album X", "Rock", 0, 200);
   insert_track("/music/b.flac", "Song B", "Artist One", "Album X", "Rock", 0, 180);
   insert_track("/music/c.flac", "Song C", "Artist Two", "Album Y", "Jazz", 0, 240);

   music_search_result_t results[10];
   int count = 0;
   music_db_search("Artist One", results, 10, &count);

   TEST_ASSERT_TRUE_MESSAGE(count == 2, "Search by artist returns 2 matches");
   if (count >= 1) {
      TEST_ASSERT_TRUE_MESSAGE(strcmp(results[0].artist, "Artist One") == 0,
                               "First result is Artist One");
   }
}

static void test_search_by_title(void) {
   printf("\n--- test_search_by_title ---\n");
   clear_tracks();

   insert_track("/music/a.flac", "Bohemian Rhapsody", "Queen", "Night Opera", "Rock", 0, 355);
   insert_track("/music/b.flac", "Another Song", "Queen", "News", "Rock", 0, 200);

   music_search_result_t results[10];
   int count = 0;
   music_db_search("Bohemian", results, 10, &count);

   TEST_ASSERT_TRUE_MESSAGE(count == 1, "Search by title finds 1 match");
   if (count >= 1) {
      TEST_ASSERT_TRUE_MESSAGE(strcmp(results[0].title, "Bohemian Rhapsody") == 0, "Title matches");
   }
}

static void test_search_by_album(void) {
   printf("\n--- test_search_by_album ---\n");
   clear_tracks();

   insert_track("/music/a.flac", "Track 1", "Artist", "Dark Side", "Rock", 0, 300);
   insert_track("/music/b.flac", "Track 2", "Artist", "The Wall", "Rock", 0, 250);

   music_search_result_t results[10];
   int count = 0;
   music_db_search("Dark Side", results, 10, &count);

   TEST_ASSERT_TRUE_MESSAGE(count == 1, "Search by album finds 1 match");
   if (count >= 1) {
      TEST_ASSERT_TRUE_MESSAGE(strcmp(results[0].album, "Dark Side") == 0, "Album matches");
   }
}

static void test_search_by_genre(void) {
   printf("\n--- test_search_by_genre ---\n");
   clear_tracks();

   insert_track("/music/a.flac", "Jazz Song", "Miles", "Kind Of Blue", "Jazz", 0, 300);
   insert_track("/music/b.flac", "Rock Song", "Led Zep", "Led Zep IV", "Rock", 0, 250);

   music_search_result_t results[10];
   int count = 0;
   music_db_search("Jazz", results, 10, &count);

   /* Should match both the genre "Jazz" AND the title "Jazz Song" — but both belong to same track
    */
   TEST_ASSERT_TRUE_MESSAGE(count == 1, "Search by genre finds 1 match");
   if (count >= 1) {
      TEST_ASSERT_TRUE_MESSAGE(strcmp(results[0].genre, "Jazz") == 0, "Genre matches");
   }
}

static void test_search_dedup_local_wins(void) {
   printf("\n--- test_search_dedup_local_wins ---\n");
   clear_tracks();

   /* Same track on both sources with different case — local (0) should win over plex (1) */
   insert_track("/music/song.flac", "Dedup Song", "Dedup Artist", "Dedup Album", "Rock", 0, 200);
   insert_track("plex:/library/song.mp3", "dedup song", "DEDUP ARTIST", "dedup album", "Rock", 1,
                200);

   music_search_result_t results[10];
   int count = 0;
   music_db_search("Dedup Song", results, 10, &count);

   TEST_ASSERT_TRUE_MESSAGE(count == 1, "Dedup returns 1 result (not 2)");
   if (count >= 1) {
      TEST_ASSERT_TRUE_MESSAGE(results[0].source == MUSIC_SOURCE_LOCAL, "Local source wins dedup");
      TEST_ASSERT_TRUE_MESSAGE(strncmp(results[0].path, "/music/", 7) == 0, "Local path returned");
   }
}

static void test_search_dedup_plex_only(void) {
   printf("\n--- test_search_dedup_plex_only ---\n");
   clear_tracks();

   /* Track only on Plex — should be returned */
   insert_track("plex:/library/exclusive.mp3", "Plex Only", "Plex Artist", "Plex Album", "Pop", 1,
                180);

   music_search_result_t results[10];
   int count = 0;
   music_db_search("Plex Only", results, 10, &count);

   TEST_ASSERT_TRUE_MESSAGE(count == 1, "Plex-only track returned");
   if (count >= 1) {
      TEST_ASSERT_TRUE_MESSAGE(results[0].source == MUSIC_SOURCE_PLEX, "Source is PLEX");
   }
}

static void test_search_dedup_case_insensitive(void) {
   printf("\n--- test_search_dedup_case_insensitive ---\n");
   clear_tracks();

   /* Dedup must be case-insensitive: metadata sources often differ in casing */

   /* Artist case difference */
   insert_track("/music/a1.flac", "Song", "Pink Floyd", "Album", "Rock", 0, 200);
   insert_track("plex:/lib/a1.mp3", "Song", "PINK FLOYD", "Album", "Rock", 1, 200);

   music_search_result_t results[10];
   int count = 0;
   music_db_search("Song", results, 10, &count);
   TEST_ASSERT_TRUE_MESSAGE(count == 1, "Case-diff artist deduplicates to 1");
   if (count >= 1) {
      TEST_ASSERT_TRUE_MESSAGE(results[0].source == MUSIC_SOURCE_LOCAL,
                               "Local wins artist case dedup");
   }

   /* Title case difference */
   clear_tracks();
   insert_track("/music/b1.flac", "Comfortably Numb", "Artist", "Album", "Rock", 0, 300);
   insert_track("plex:/lib/b1.mp3", "comfortably numb", "Artist", "Album", "Rock", 1, 300);

   music_db_search("Artist", results, 10, &count);
   TEST_ASSERT_TRUE_MESSAGE(count == 1, "Case-diff title deduplicates to 1");

   /* Album case difference */
   clear_tracks();
   insert_track("/music/c1.flac", "Track", "Art", "The Wall", "Rock", 0, 250);
   insert_track("plex:/lib/c1.mp3", "Track", "Art", "THE WALL", "Rock", 1, 250);

   music_db_search("Track", results, 10, &count);
   TEST_ASSERT_TRUE_MESSAGE(count == 1, "Case-diff album deduplicates to 1");

   /* All three differ in case simultaneously */
   clear_tracks();
   insert_track("/music/d1.flac", "Time", "Pink Floyd", "Dark Side", "Rock", 0, 400);
   insert_track("plex:/lib/d1.mp3", "TIME", "pink floyd", "DARK SIDE", "Rock", 1, 400);

   music_db_search("Pink Floyd", results, 10, &count);
   TEST_ASSERT_TRUE_MESSAGE(count == 1, "Triple case-diff deduplicates to 1");
   if (count >= 1) {
      TEST_ASSERT_TRUE_MESSAGE(results[0].source == MUSIC_SOURCE_LOCAL,
                               "Local wins triple case dedup");
   }

   /* Stats also reflect case-insensitive dedup */
   music_db_stats_t stats;
   music_db_get_stats(&stats);
   TEST_ASSERT_TRUE_MESSAGE(stats.track_count == 1,
                            "Stats track count reflects case-insensitive dedup");
}

static void test_search_dedup_different_titles(void) {
   printf("\n--- test_search_dedup_different_titles ---\n");
   clear_tracks();

   /* Same artist, different titles — both should survive dedup */
   insert_track("/music/a.flac", "Title Alpha", "Shared Artist", "Album", "Rock", 0, 200);
   insert_track("plex:/lib/b.mp3", "Title Beta", "Shared Artist", "Album", "Rock", 1, 210);

   music_search_result_t results[10];
   int count = 0;
   music_db_search("Shared Artist", results, 10, &count);

   TEST_ASSERT_TRUE_MESSAGE(count == 2, "Different titles both returned (no false dedup)");
}

static void test_search_like_escaping(void) {
   printf("\n--- test_search_like_escaping ---\n");
   clear_tracks();

   /* "100%" should not act as wildcard — the % must be escaped */
   insert_track("/music/percent.flac", "100% Pure", "Test", "Album", "Rock", 0, 200);
   insert_track("/music/other.flac", "100 reasons", "Test", "Album", "Rock", 0, 180);

   music_search_result_t results[10];
   int count = 0;
   music_db_search("100% Pure", results, 10, &count);

   /* Should match "100% Pure" but NOT "100 reasons" (% is escaped, not wildcard) */
   TEST_ASSERT_TRUE_MESSAGE(count == 1, "Escaped %% prevents wildcard match");
   if (count >= 1) {
      TEST_ASSERT_TRUE_MESSAGE(strcmp(results[0].title, "100% Pure") == 0,
                               "Only exact percent match");
   }

   /* "_" should not match single char wildcard */
   clear_tracks();
   insert_track("/music/under.flac", "test_x", "Test", "Album", "Rock", 0, 200);
   insert_track("/music/nope.flac", "testYx", "Test", "Album", "Rock", 0, 180);

   music_db_search("test_x", results, 10, &count);
   TEST_ASSERT_TRUE_MESSAGE(count == 1, "Escaped _ prevents single-char wildcard");
   if (count >= 1) {
      TEST_ASSERT_TRUE_MESSAGE(strcmp(results[0].title, "test_x") == 0,
                               "Only literal underscore matches");
   }
}

/* =============================================================================
 * Group 4: Browse with dedup
 * ============================================================================= */

static void test_list_artists_dedup(void) {
   printf("\n--- test_list_artists_dedup ---\n");
   clear_tracks();

   /* Same artist on both sources */
   insert_track("/music/a.flac", "Song A", "Shared Artist", "Album 1", "Rock", 0, 200);
   insert_track("plex:/lib/b.mp3", "Song B", "Shared Artist", "Album 2", "Rock", 1, 210);

   char artists[10][AUDIO_METADATA_STRING_MAX];
   int count = 0;
   music_db_list_artists(artists, 10, 0, &count);

   TEST_ASSERT_TRUE_MESSAGE(count == 1, "Artist appears once despite two sources");
   if (count >= 1) {
      TEST_ASSERT_TRUE_MESSAGE(strcmp(artists[0], "Shared Artist") == 0, "Artist name correct");
   }
}

static void test_list_albums_dedup(void) {
   printf("\n--- test_list_albums_dedup ---\n");
   clear_tracks();

   /* Same album+artist+title on both sources */
   insert_track("/music/a.flac", "Track 1", "Artist", "Shared Album", "Rock", 0, 200);
   insert_track("plex:/lib/b.mp3", "Track 1", "Artist", "Shared Album", "Rock", 1, 200);

   char albums[10][AUDIO_METADATA_STRING_MAX];
   int count = 0;
   music_db_list_albums(albums, 10, 0, &count);

   TEST_ASSERT_TRUE_MESSAGE(count == 1, "Album appears once despite two sources");
   if (count >= 1) {
      TEST_ASSERT_TRUE_MESSAGE(strcmp(albums[0], "Shared Album") == 0, "Album name correct");
   }
}

static void test_get_by_artist_dedup(void) {
   printf("\n--- test_get_by_artist_dedup ---\n");
   clear_tracks();

   /* Duplicate track (same artist+album+title) on both sources */
   insert_track("/music/song.flac", "The Song", "The Artist", "The Album", "Rock", 0, 300);
   insert_track("plex:/lib/song.mp3", "The Song", "The Artist", "The Album", "Rock", 1, 300);
   /* Plus a unique plex track */
   insert_track("plex:/lib/other.mp3", "Other Song", "The Artist", "The Album", "Rock", 1, 250);

   music_search_result_t results[10];
   int count = 0;
   music_db_get_by_artist("The Artist", results, 10, &count);

   TEST_ASSERT_TRUE_MESSAGE(count == 2, "Dedup: 2 unique tracks (not 3 rows)");

   /* Verify the duplicate resolved to local */
   int found_local = 0;
   for (int i = 0; i < count; i++) {
      if (strcmp(results[i].title, "The Song") == 0 && results[i].source == MUSIC_SOURCE_LOCAL)
         found_local = 1;
   }
   TEST_ASSERT_TRUE_MESSAGE(found_local == 1, "Duplicate resolved to local source");
}

static void test_stats_dedup(void) {
   printf("\n--- test_stats_dedup ---\n");
   clear_tracks();

   /* 2 unique tracks, one duplicated across sources = 3 rows, 2 deduped */
   insert_track("/music/a.flac", "Song A", "Artist X", "Album 1", "Rock", 0, 200);
   insert_track("plex:/lib/a.mp3", "Song A", "Artist X", "Album 1", "Rock", 1, 200);
   insert_track("/music/b.flac", "Song B", "Artist Y", "Album 2", "Jazz", 0, 180);

   music_db_stats_t stats;
   int rc = music_db_get_stats(&stats);

   TEST_ASSERT_TRUE_MESSAGE(rc == 0, "get_stats succeeds");
   TEST_ASSERT_TRUE_MESSAGE(stats.track_count == 2, "Deduped track count is 2 (not 3)");
   TEST_ASSERT_TRUE_MESSAGE(stats.artist_count == 2, "Artist count is 2");
   TEST_ASSERT_TRUE_MESSAGE(stats.album_count == 2, "Album count is 2");
}

/* =============================================================================
 * Group 5: Path lookup
 * ============================================================================= */

static void test_get_by_path_local(void) {
   printf("\n--- test_get_by_path_local ---\n");
   clear_tracks();

   insert_track("/music/found.flac", "Found It", "Artist", "Album", "Rock", 0, 200);

   music_search_result_t result;
   bool found = false;
   int rc = music_db_get_by_path("/music/found.flac", &result, &found);

   TEST_ASSERT_TRUE_MESSAGE(rc == 0 && found, "get_by_path finds local track");
   TEST_ASSERT_TRUE_MESSAGE(strcmp(result.title, "Found It") == 0, "Title matches");
   TEST_ASSERT_TRUE_MESSAGE(strcmp(result.path, "/music/found.flac") == 0, "Path matches");
}

static void test_get_by_path_plex(void) {
   printf("\n--- test_get_by_path_plex ---\n");
   clear_tracks();

   insert_track("plex:/library/parts/42/file.mp3", "Plex Track", "Plex Art", "Plex Alb", "Pop", 1,
                300);

   music_search_result_t result;
   bool found = false;
   int rc = music_db_get_by_path("plex:/library/parts/42/file.mp3", &result, &found);

   TEST_ASSERT_TRUE_MESSAGE(rc == 0 && found, "get_by_path finds plex track");
   TEST_ASSERT_TRUE_MESSAGE(strcmp(result.title, "Plex Track") == 0, "Plex title matches");
}

static void test_get_by_path_missing(void) {
   printf("\n--- test_get_by_path_missing ---\n");
   clear_tracks();

   music_search_result_t result;
   bool found = false;
   int rc = music_db_get_by_path("/nonexistent/path.flac", &result, &found);

   TEST_ASSERT_TRUE_MESSAGE(rc == 0 && !found, "get_by_path returns not-found for missing track");
}

/* =============================================================================
 * Group 6: Source-scoped stale deletion
 * ============================================================================= */

static void test_stale_deletion_scoped(void) {
   printf("\n--- test_stale_deletion_scoped ---\n");
   clear_tracks();

   /* Insert local and plex tracks */
   insert_track("/music/keep.flac", "Keep", "Artist", "Album", "Rock", 0, 200);
   insert_track("/music/stale.flac", "Stale", "Artist", "Album", "Rock", 0, 180);
   insert_track("plex:/lib/survive.mp3", "Survive", "Plex Art", "Plex Alb", "Pop", 1, 220);

   /* Simulate stale deletion scoped to local source:
    * Only /music/keep.flac is "seen" — stale.flac should be deleted,
    * but plex track should survive. */
   const char *stale_sql = "CREATE TEMP TABLE IF NOT EXISTS seen_paths (path TEXT PRIMARY KEY)";
   sqlite3_exec(g_test_db, stale_sql, NULL, NULL, NULL);
   sqlite3_exec(g_test_db, "DELETE FROM seen_paths", NULL, NULL, NULL);
   sqlite3_exec(g_test_db, "INSERT INTO seen_paths (path) VALUES ('/music/keep.flac')", NULL, NULL,
                NULL);

   char delete_sql[512];
   snprintf(delete_sql, sizeof(delete_sql),
            "DELETE FROM music_metadata WHERE source = %d "
            "AND path NOT IN (SELECT path FROM seen_paths)",
            MUSIC_SOURCE_LOCAL);
   sqlite3_exec(g_test_db, delete_sql, NULL, NULL, NULL);
   sqlite3_exec(g_test_db, "DROP TABLE seen_paths", NULL, NULL, NULL);

   /* Verify: keep.flac and plex track survive, stale.flac deleted */
   music_search_result_t result;
   bool found = false;

   music_db_get_by_path("/music/keep.flac", &result, &found);
   TEST_ASSERT_TRUE_MESSAGE(found, "Kept local track survives");

   music_db_get_by_path("/music/stale.flac", &result, &found);
   TEST_ASSERT_TRUE_MESSAGE(!found, "Stale local track deleted");

   music_db_get_by_path("plex:/lib/survive.mp3", &result, &found);
   TEST_ASSERT_TRUE_MESSAGE(found, "Plex track survives local stale deletion");
}

/* =============================================================================
 * Main
 * ============================================================================= */

void setUp(void) {
   setup_db();
}

void tearDown(void) {
   teardown_db();
}

int main(void) {
   UNITY_BEGIN();
   RUN_TEST(test_source_names);
   RUN_TEST(test_source_prefixes);
   RUN_TEST(test_source_from_path);
   RUN_TEST(test_init_cleanup);
   RUN_TEST(test_schema_migration_idempotent);
   RUN_TEST(test_search_basic);
   RUN_TEST(test_search_by_title);
   RUN_TEST(test_search_by_album);
   RUN_TEST(test_search_by_genre);
   RUN_TEST(test_search_dedup_local_wins);
   RUN_TEST(test_search_dedup_plex_only);
   RUN_TEST(test_search_dedup_case_insensitive);
   RUN_TEST(test_search_dedup_different_titles);
   RUN_TEST(test_search_like_escaping);
   RUN_TEST(test_list_artists_dedup);
   RUN_TEST(test_list_albums_dedup);
   RUN_TEST(test_get_by_artist_dedup);
   RUN_TEST(test_stats_dedup);
   RUN_TEST(test_get_by_path_local);
   RUN_TEST(test_get_by_path_plex);
   RUN_TEST(test_get_by_path_missing);
   RUN_TEST(test_stale_deletion_scoped);
   return UNITY_END();
}
