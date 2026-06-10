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
 * OTA pending-update marker codec + shared path/policy constants.
 *
 * Compiled into BOTH the satellite binary (dawn_satellite, via ota_apply.c)
 * and the standalone rollback launcher (dawn-satellite-launch, via
 * ota_launch.c), so the two agents can never disagree on the on-disk format.
 * Pure libc — no libsodium, no libcurl, no SDL — so it links into the tiny
 * launcher with zero dependency surface.  See docs/OTA_DESIGN.md §7.
 */

#ifndef OTA_MARKER_H
#define OTA_MARKER_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Filesystem layout (all under the one dawn-owned ReadWritePaths root) ----
 * Everything lives on a single filesystem so rename(2) of the binary and the
 * marker is atomic. */
#ifndef OTA_DATA_DIR /* overridable only for host unit tests; production = the default */
#define OTA_DATA_DIR "/var/lib/dawn-satellite"
#endif
#define OTA_BIN_DIR OTA_DATA_DIR "/bin"
#define OTA_BIN_PATH OTA_BIN_DIR "/dawn_satellite"
#define OTA_TMP_DIR OTA_DATA_DIR "/ota/tmp"
#define OTA_ROLLBACK_DIR OTA_DATA_DIR "/ota/rollback"
#define OTA_ROLLBACK_BIN OTA_ROLLBACK_DIR "/dawn_satellite"
#define OTA_PENDING_MARKER OTA_DATA_DIR "/ota/pending"

/* ---- Policy constants ---- */
#define OTA_MARKER_VERSION 1 /* on-disk marker schema version */
#define OTA_ROLLBACK_BOOT_THRESHOLD \
   3                          /* boots before the launcher rolls back (< StartLimitBurst=5) */
#define OTA_PROBATION_SEC 900 /* commit backstop: an update older than this is deemed good */
#define OTA_MAX_IMAGE_BYTES                                                                        \
   (8 * 1024 * 1024)                           /* sanity cap (binary ~835 KB; headroom for growth) \
                                                */
#define OTA_FREE_SLACK_BYTES (8 * 1024 * 1024) /* statvfs preflight slack for FTL free blocks */
#define OTA_IO_CHUNK_BYTES (64 * 1024)         /* streaming copy/hash buffer (download, rollback) */

#define OTA_VERSION_STR_MAX 32 /* semver incl. NUL; mirrors OTA_VERSION_MAX */

/* Parsed pending-update marker.  Written atomically (temp + fsync + rename);
 * its absence means "no update on probation". */
typedef struct {
   int marker_version;                       /* must equal OTA_MARKER_VERSION */
   char target_version[OTA_VERSION_STR_MAX]; /* version the new binary should report */
   char prev_version[OTA_VERSION_STR_MAX];   /* version the rollback copy reports (log/trace) */
   int boots;                                /* startup count since the swap (launcher-owned) */
   int64_t created;                          /* unix ts at apply time (64-bit: 2038/armhf-safe) */
   int ran_ok; /* 1 once the new binary proved it runs (binary-owned) */
} ota_marker_t;

/* Read result.  The launcher must FAIL SAFE: anything other than OK/ABSENT is
 * treated as "no probation, boot normally, do not roll back" — a frozen
 * launcher must never brick the device on an unrecognized or corrupt marker. */
typedef enum {
   OTA_MARKER_ABSENT = 0, /* no marker file (normal boot) */
   OTA_MARKER_OK,         /* parsed cleanly, marker_version recognized */
   OTA_MARKER_FAILSAFE,   /* unknown version / corrupt / unparsable → do not roll back */
} ota_marker_status_t;

/**
 * @brief Read + defensively parse the marker at @p path.
 * @return ABSENT, OK (fills @p out), or FAILSAFE.  @p out may be NULL.
 */
ota_marker_status_t ota_marker_read(const char *path, ota_marker_t *out);

/**
 * @brief Atomically replace the marker at @p path (temp + fsync + rename + dir fsync).
 * @return SUCCESS or FAILURE.
 */
int ota_marker_write(const char *path, const ota_marker_t *m);

/**
 * @brief Remove the marker (commit).  Idempotent — a missing marker is success.
 * @return SUCCESS or FAILURE.
 */
int ota_marker_remove(const char *path);

/**
 * @brief fsync the directory containing @p path so a preceding rename/unlink is
 *        durable across power loss.  Shared by every OTA atomic-replace site
 *        (marker codec, apply worker, launcher restore); pure libc, so it links
 *        into the dependency-free launcher too.  Best-effort (silent on error).
 */
void ota_fsync_dir_of(const char *path);

#ifdef __cplusplus
}
#endif

#endif /* OTA_MARKER_H */
