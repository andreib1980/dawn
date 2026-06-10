#ifndef OTA_APPLY_H
#define OTA_APPLY_H

/* OTA device-apply (Tier 2, ESP32-S3) — public API + shared config.  See
 * docs/OTA_DESIGN.md §8 and ota_apply.cpp.  Included by the main sketch (for the
 * three entry points + OTA_ABI_TAG) and by ota_apply.cpp (for everything). */

#include <ArduinoJson.h> /* JsonObject (ota_handle_offer parameter) */

#include "satellite_version.h" /* FIRMWARE_VERSION */

/* abi_tag baked into esp32 release manifests (--abi-tag esp32s3) + reported as
 * hw.platform; the device rejects any image whose manifest abi_tag differs. */
#define OTA_ABI_TAG "esp32s3"
/* app0/app1 slot size from partitions.csv (0x1E0000 = 1,966,080 bytes). */
#define OTA_APP_SLOT_BYTES 0x1E0000UL
/* Runtime reject ceiling: the image must physically fit the destination slot
 * with headroom for the OTA trailer (slot − 64 KB ≈ 1.90 MB).  Separate from the
 * build-time advisory that the .bin stay under ~85% of the slot. */
#define OTA_MAX_IMAGE_BYTES (OTA_APP_SLOT_BYTES - 0x10000UL)
/* Verify-state boots before the NVS guard reverts. */
#define OTA_ESP_ROLLBACK_THRESHOLD 3
/* Verify-boot watchdog: a just-flashed image that reaches setup() but does NOT
 * successfully register within this window is force-rebooted (esp_restart) so the
 * boot-count guard above advances toward a revert.  Without it, a HANG in setup()
 * (e.g. a mis-built image whose audio ps_malloc fails and while(1)-hangs) never
 * reboots, so the guard never fires → soft-brick until a USB reflash.  Generous
 * so a slow-but-healthy network registration isn't false-reverted (90 s × 3 boots
 * ≈ 4.5 min worst-case recovery).  See OTA_DESIGN §8. */
#define OTA_VERIFY_REGISTER_TIMEOUT_MS 90000

/* Run first in setup(): apply a pending download (state 'D') and reboot, run the
 * verify-state boot-count rollback guard (state 'V'), or return for a normal boot. */
void ota_boot_path();

/* Call on a successful satellite_register_ack: commits a freshly-applied update
 * (clears the verify-state guard, marks the app valid). */
void ota_on_registered();

/* Handle a server 'ota_offer': verify + decide + (on accept) persist to NVS and
 * reboot into the apply path.  Rejects via ota_reject on any failure. */
void ota_handle_offer(JsonObject payload);

#endif /* OTA_APPLY_H */
