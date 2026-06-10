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
 * Host unit tests for the OTA pending-update marker codec (ota_marker.c).
 * Covers section-E row 13's marker half: round-trip fidelity, atomic replace,
 * and the defensive fail-safe parse (corrupt / truncated / unknown-version
 * markers must read as FAILSAFE so the frozen launcher never rolls back on
 * garbage).  Pure libc — runs on any host.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "dawn_error.h"
#include "ota_marker.h"
#include "unity.h"

static char g_dir[256];
static char g_path[320];

void setUp(void) {
   snprintf(g_dir, sizeof(g_dir), "/tmp/ota_marker_test_XXXXXX");
   TEST_ASSERT_NOT_NULL(mkdtemp(g_dir));
   snprintf(g_path, sizeof(g_path), "%s/pending", g_dir);
}

void tearDown(void) {
   unlink(g_path);
   rmdir(g_dir);
}

/* Write raw bytes to the marker path (for crafting malformed inputs). */
static void write_raw(const char *content) {
   FILE *f = fopen(g_path, "w");
   TEST_ASSERT_NOT_NULL(f);
   if (content[0] != '\0') {
      fputs(content, f);
   }
   fclose(f);
}

/* ---- absence ------------------------------------------------------------- */
static void test_absent(void) {
   ota_marker_t m;
   TEST_ASSERT_EQUAL_INT(OTA_MARKER_ABSENT, ota_marker_read(g_path, &m));
}

/* ---- round-trip ---------------------------------------------------------- */
static void test_write_read_roundtrip(void) {
   ota_marker_t in = { 0 };
   in.marker_version = OTA_MARKER_VERSION;
   snprintf(in.target_version, sizeof(in.target_version), "2.3.0");
   snprintf(in.prev_version, sizeof(in.prev_version), "2.2.0");
   in.boots = 2;
   in.created = 1750000000LL; /* > INT32_MAX-era ok: 64-bit field */
   in.ran_ok = 1;
   TEST_ASSERT_EQUAL_INT(SUCCESS, ota_marker_write(g_path, &in));

   ota_marker_t out;
   TEST_ASSERT_EQUAL_INT(OTA_MARKER_OK, ota_marker_read(g_path, &out));
   TEST_ASSERT_EQUAL_INT(OTA_MARKER_VERSION, out.marker_version);
   TEST_ASSERT_EQUAL_STRING("2.3.0", out.target_version);
   TEST_ASSERT_EQUAL_STRING("2.2.0", out.prev_version);
   TEST_ASSERT_EQUAL_INT(2, out.boots);
   TEST_ASSERT_EQUAL_INT64(1750000000LL, out.created);
   TEST_ASSERT_EQUAL_INT(1, out.ran_ok);
}

/* created beyond 32-bit must survive (no 2038 truncation). */
static void test_created_64bit(void) {
   ota_marker_t in = { 0 };
   in.marker_version = OTA_MARKER_VERSION;
   in.created = 4102444800LL; /* 2100-01-01, > INT32_MAX */
   TEST_ASSERT_EQUAL_INT(SUCCESS, ota_marker_write(g_path, &in));
   ota_marker_t out;
   TEST_ASSERT_EQUAL_INT(OTA_MARKER_OK, ota_marker_read(g_path, &out));
   TEST_ASSERT_EQUAL_INT64(4102444800LL, out.created);
}

/* ---- atomic replace ------------------------------------------------------ */
static void test_atomic_replace(void) {
   ota_marker_t a = { 0 };
   a.marker_version = OTA_MARKER_VERSION;
   a.boots = 1;
   TEST_ASSERT_EQUAL_INT(SUCCESS, ota_marker_write(g_path, &a));

   ota_marker_t b = { 0 };
   b.marker_version = OTA_MARKER_VERSION;
   b.boots = 3;
   TEST_ASSERT_EQUAL_INT(SUCCESS, ota_marker_write(g_path, &b));

   ota_marker_t out;
   TEST_ASSERT_EQUAL_INT(OTA_MARKER_OK, ota_marker_read(g_path, &out));
   TEST_ASSERT_EQUAL_INT(3, out.boots); /* second write fully replaced the first */
   /* no leftover temp file in the dir */
   char tmp[400];
   snprintf(tmp, sizeof(tmp), "%s.tmp", g_path);
   TEST_ASSERT_NOT_EQUAL(0, access(tmp, F_OK)); /* .tmp must not linger */
}

/* ---- remove (commit) ----------------------------------------------------- */
static void test_remove_then_absent(void) {
   ota_marker_t in = { 0 };
   in.marker_version = OTA_MARKER_VERSION;
   TEST_ASSERT_EQUAL_INT(SUCCESS, ota_marker_write(g_path, &in));
   TEST_ASSERT_EQUAL_INT(SUCCESS, ota_marker_remove(g_path));
   TEST_ASSERT_EQUAL_INT(OTA_MARKER_ABSENT, ota_marker_read(g_path, NULL));
   /* remove is idempotent — a missing marker is success */
   TEST_ASSERT_EQUAL_INT(SUCCESS, ota_marker_remove(g_path));
}

/* ---- defensive parse → FAILSAFE ----------------------------------------- */
static void test_unknown_marker_version_failsafe(void) {
   write_raw("marker_version=999\ntarget_version=9.9.9\nboots=0\ncreated=1\nran_ok=0\n");
   TEST_ASSERT_EQUAL_INT(OTA_MARKER_FAILSAFE, ota_marker_read(g_path, NULL));
}

static void test_missing_version_failsafe(void) {
   write_raw("target_version=2.3.0\nboots=1\ncreated=1\nran_ok=0\n");
   TEST_ASSERT_EQUAL_INT(OTA_MARKER_FAILSAFE, ota_marker_read(g_path, NULL));
}

static void test_nonnumeric_field_failsafe(void) {
   write_raw("marker_version=1\nboots=lots\ncreated=1\nran_ok=0\n");
   TEST_ASSERT_EQUAL_INT(OTA_MARKER_FAILSAFE, ota_marker_read(g_path, NULL));
}

static void test_empty_file_failsafe(void) {
   write_raw(""); /* zero-byte marker: no version seen → fail-safe */
   TEST_ASSERT_EQUAL_INT(OTA_MARKER_FAILSAFE, ota_marker_read(g_path, NULL));
}

static void test_overlong_line_failsafe(void) {
   /* A single line longer than the parser's buffer is treated as corruption. */
   char buf[1024];
   int n = snprintf(buf, sizeof(buf), "marker_version=1\ntarget_version=");
   memset(buf + n, 'x', sizeof(buf) - n - 2);
   buf[sizeof(buf) - 2] = '\n';
   buf[sizeof(buf) - 1] = '\0';
   write_raw(buf);
   TEST_ASSERT_EQUAL_INT(OTA_MARKER_FAILSAFE, ota_marker_read(g_path, NULL));
}

/* Unknown keys are tolerated (forward-compat) as long as the version matches. */
static void test_unknown_keys_tolerated(void) {
   write_raw("marker_version=1\ntarget_version=2.3.0\nboots=1\ncreated=5\nran_ok=0\n"
             "future_field=whatever\n# a comment-ish line\n");
   ota_marker_t out;
   TEST_ASSERT_EQUAL_INT(OTA_MARKER_OK, ota_marker_read(g_path, &out));
   TEST_ASSERT_EQUAL_STRING("2.3.0", out.target_version);
   TEST_ASSERT_EQUAL_INT(1, out.boots);
}

int main(void) {
   UNITY_BEGIN();
   RUN_TEST(test_absent);
   RUN_TEST(test_write_read_roundtrip);
   RUN_TEST(test_created_64bit);
   RUN_TEST(test_atomic_replace);
   RUN_TEST(test_remove_then_absent);
   RUN_TEST(test_unknown_marker_version_failsafe);
   RUN_TEST(test_missing_version_failsafe);
   RUN_TEST(test_nonnumeric_field_failsafe);
   RUN_TEST(test_empty_file_failsafe);
   RUN_TEST(test_overlong_line_failsafe);
   RUN_TEST(test_unknown_keys_tolerated);
   return UNITY_END();
}
