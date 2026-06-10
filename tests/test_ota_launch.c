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
 * Host unit tests for the launcher rollback-agent decision (launch_step).
 * Covers section-E rows 7/8 (crash-loop + dead-on-arrival → rollback@threshold),
 * 9 (empty restore slot → keep marker, never blank the binary), 10 (ran_ok →
 * commit, the transient-network guard's decision half), 11 (no marker → normal
 * boot, the phantom-rollback guard), and 13 (fail-safe on a corrupt marker).
 *
 * Compiled with -DOTA_DATA_DIR pointed at a temp tree so all OTA_* paths land
 * under it; no systemd, no real ELF, no device.
 */

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "dawn_error.h"
#include "ota_launch.h"
#include "ota_marker.h"
#include "unity.h"

#define LIVE_BIN OTA_BIN_PATH
#define SLOT_BIN OTA_ROLLBACK_BIN
#define MARKER OTA_PENDING_MARKER

static void rm(const char *p) {
   unlink(p);
}

void setUp(void) {
   /* Build the OTA dir tree under the compile-time temp OTA_DATA_DIR. */
   mkdir(OTA_DATA_DIR, 0755);
   mkdir(OTA_DATA_DIR "/bin", 0755);
   mkdir(OTA_DATA_DIR "/ota", 0755);
   mkdir(OTA_DATA_DIR "/ota/rollback", 0700);
   mkdir(OTA_DATA_DIR "/ota/tmp", 0700);
   /* clean any residue from a prior run */
   rm(MARKER);
   rm(LIVE_BIN);
   rm(SLOT_BIN);
   rm(LIVE_BIN ".restore");
   rm(MARKER ".tmp");
}

void tearDown(void) {
   rm(MARKER);
   rm(LIVE_BIN);
   rm(SLOT_BIN);
   rm(LIVE_BIN ".restore");
   rm(MARKER ".tmp");
}

/* Place a fake executable file with given contents (sets the exec bit). */
static void place_exec(const char *path, const char *content) {
   FILE *f = fopen(path, "w");
   TEST_ASSERT_NOT_NULL(f);
   fputs(content, f);
   fclose(f);
   TEST_ASSERT_EQUAL_INT(0, chmod(path, 0755));
}

static int exists(const char *path) {
   return access(path, F_OK) == 0;
}

static void read_file(const char *path, char *out, size_t n) {
   out[0] = '\0';
   FILE *f = fopen(path, "r");
   if (!f) {
      return;
   }
   size_t r = fread(out, 1, n - 1, f);
   out[r] = '\0';
   fclose(f);
}

/* Write a probation marker via the real codec. */
static void put_marker(int boots,
                       int64_t created,
                       int ran_ok,
                       const char *target,
                       const char *prev) {
   ota_marker_t m = { 0 };
   m.marker_version = OTA_MARKER_VERSION;
   m.boots = boots;
   m.created = created;
   m.ran_ok = ran_ok;
   snprintf(m.target_version, sizeof(m.target_version), "%s", target);
   snprintf(m.prev_version, sizeof(m.prev_version), "%s", prev);
   TEST_ASSERT_EQUAL_INT(SUCCESS, ota_marker_write(MARKER, &m));
}

/* ---- row 11: no marker → normal boot ------------------------------------ */
static void test_no_marker_boots(void) {
   TEST_ASSERT_EQUAL_INT(OTA_LAUNCH_BOOT, launch_step(1000));
}

/* ---- row 13: corrupt/unknown marker → fail-safe normal boot ------------- */
static void test_failsafe_marker_boots(void) {
   FILE *f = fopen(MARKER, "w");
   TEST_ASSERT_NOT_NULL(f);
   fputs("marker_version=999\nboots=5\ncreated=1\nran_ok=0\n", f); /* unknown version */
   fclose(f);
   TEST_ASSERT_EQUAL_INT(OTA_LAUNCH_BOOT, launch_step(1000));
   TEST_ASSERT_TRUE(exists(MARKER)); /* fail-safe does NOT touch the marker */
}

/* ---- row 10: ran_ok=1 → commit (clears marker) -------------------------- */
static void test_ran_ok_commits(void) {
   put_marker(1, 1000, 1, "2.3.0", "2.2.0");
   TEST_ASSERT_EQUAL_INT(OTA_LAUNCH_COMMIT, launch_step(1001));
   TEST_ASSERT_FALSE(exists(MARKER)); /* committed → marker removed */
}

/* probation window elapsed → commit even without ran_ok */
static void test_expired_commits(void) {
   put_marker(1, 1000, 0, "2.3.0", "2.2.0");
   TEST_ASSERT_EQUAL_INT(OTA_LAUNCH_COMMIT, launch_step(1000 + OTA_PROBATION_SEC + 1));
   TEST_ASSERT_FALSE(exists(MARKER));
}

/* ---- under threshold → count, persist bumped boots ---------------------- */
static void test_under_threshold_counts(void) {
   put_marker(0, 5000, 0, "2.3.0", "2.2.0");
   TEST_ASSERT_EQUAL_INT(OTA_LAUNCH_COUNT, launch_step(5001)); /* not expired */
   ota_marker_t out;
   TEST_ASSERT_EQUAL_INT(OTA_MARKER_OK, ota_marker_read(MARKER, &out));
   TEST_ASSERT_EQUAL_INT(1, out.boots); /* bumped + persisted */
}

/* ---- rows 7/8: crash-loop ramp → rollback at threshold ------------------ */
static void test_threshold_ramp_then_rollback(void) {
   place_exec(SLOT_BIN, "GOOD-OLD-BINARY"); /* known-good restore target */
   place_exec(LIVE_BIN, "BAD-NEW-BINARY");  /* the image being probated */
   put_marker(0, 7000, 0, "2.3.0", "2.2.0");

   /* Each call = one systemd start (1 boot bump), no ran_ok, not expired. */
   for (int b = 1; b < OTA_ROLLBACK_BOOT_THRESHOLD; b++) {
      TEST_ASSERT_EQUAL_INT(OTA_LAUNCH_COUNT, launch_step(7001));
   }
   /* The start that reaches the threshold restores the rollback slot. */
   TEST_ASSERT_EQUAL_INT(OTA_LAUNCH_ROLLBACK, launch_step(7001));
   TEST_ASSERT_FALSE(exists(MARKER)); /* marker cleared after rollback */

   char live[64];
   read_file(LIVE_BIN, live, sizeof(live));
   TEST_ASSERT_EQUAL_STRING("GOOD-OLD-BINARY", live); /* live binary restored */
   TEST_ASSERT_TRUE(exists(SLOT_BIN));                /* slot survives (copy, not move) */
}

/* ---- row 9: threshold reached but no restore slot → keep marker --------- */
static void test_empty_slot_keeps_marker(void) {
   place_exec(LIVE_BIN, "BAD-NEW-BINARY");
   /* no SLOT_BIN placed → empty restore slot */
   put_marker(OTA_ROLLBACK_BOOT_THRESHOLD - 1, 8000, 0, "2.3.0", "2.2.0");

   TEST_ASSERT_EQUAL_INT(OTA_LAUNCH_NO_TARGET, launch_step(8001));
   TEST_ASSERT_TRUE(exists(MARKER)); /* marker KEPT — keep trying / manual recovery */

   char live[64];
   read_file(LIVE_BIN, live, sizeof(live));
   TEST_ASSERT_EQUAL_STRING("BAD-NEW-BINARY", live); /* live binary NOT blanked */
}

/* ---- row 9 detail: non-executable slot is treated as unusable ----------- */
static void test_nonexec_slot_keeps_marker(void) {
   place_exec(LIVE_BIN, "BAD-NEW-BINARY");
   FILE *f = fopen(SLOT_BIN, "w"); /* present but NOT executable */
   TEST_ASSERT_NOT_NULL(f);
   fputs("not-exec", f);
   fclose(f);
   chmod(SLOT_BIN, 0644);
   put_marker(OTA_ROLLBACK_BOOT_THRESHOLD - 1, 9000, 0, "2.3.0", "2.2.0");

   TEST_ASSERT_EQUAL_INT(OTA_LAUNCH_NO_TARGET, launch_step(9001));
   TEST_ASSERT_TRUE(exists(MARKER));
}

int main(void) {
   UNITY_BEGIN();
   RUN_TEST(test_no_marker_boots);
   RUN_TEST(test_failsafe_marker_boots);
   RUN_TEST(test_ran_ok_commits);
   RUN_TEST(test_expired_commits);
   RUN_TEST(test_under_threshold_counts);
   RUN_TEST(test_threshold_ramp_then_rollback);
   RUN_TEST(test_empty_slot_keeps_marker);
   RUN_TEST(test_nonexec_slot_keeps_marker);
   return UNITY_END();
}
