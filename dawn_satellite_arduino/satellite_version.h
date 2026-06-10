#ifndef SATELLITE_VERSION_H
#define SATELLITE_VERSION_H

/* Single source of truth for the Tier-2 ESP32 satellite firmware version.
 * Reported to the daemon at registration (firmware_version) for OTA fleet
 * visibility, and checked against the pushed target at apply time.
 *
 * BUMP THIS on every released build — the server only finalizes an OTA push as
 * 'success' when the device re-registers reporting this == the pushed version
 * (there is no build-time gate on ESP32, unlike the Tier-1 ota-release.sh
 * marker check). */
#define FIRMWARE_VERSION "2.0.0"

/* Greppable marker embedded in the .bin so ota-release.sh can read the EXACT
 * version compiled into the image being signed (same mechanism as the Tier-1
 * RPi binary).  ota_apply.cpp emits this as a `used` global. */
#define DAWN_SAT_FW_VERSION_MARKER "DAWN_SAT_FW_VERSION=" FIRMWARE_VERSION

#endif /* SATELLITE_VERSION_H */
