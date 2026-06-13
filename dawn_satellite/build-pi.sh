#!/bin/bash
#
# Build dawn_satellite for the target Raspberry Pi (Debian trixie arm64) from the
# LOCAL working tree — no commit / push / checkout-on-the-Pi needed.
#
# On an arm64 host (the Jetson) this is a NATIVE build — no qemu — and the binary
# links the Pi's libwebsockets.so.19 / glibc (a Jetson-native build links .so.16
# and won't run on the Pi).  Output: dawn_satellite/build-pi/dawn_satellite —
# scp to the Pi.  See docs/OTA_DESIGN.md §10.
#
# Modes (SAT_BUILD env):
#   full      (default) — VAD + ASR + TTS: a deployable voice satellite.  Uses the
#             pre-built base image (heavy ML deps — ONNX/Vosk/Piper/espeak — baked
#             in) so the per-build is just a source compile.  Pulls the base from
#             GHCR; if that fails (not pushed yet / offline), builds it locally
#             from Dockerfile.pibase once (heavy, then cached).
#   headless  — DAP2-only (no VAD/ASR/TTS).  Lightweight, apt-only, built locally
#             from Dockerfile.pibuild.  Good for connect / register / OTA-control-
#             plane tests; runs as a service only with mode=text_only (no models).
#
# Knobs (full mode):
#   SAT_ASR=whisper|vosk   ASR engine (default whisper — matches dawn-kitchen)
#   SAT_SDL=on|off         SDL2 touchscreen UI (default on — Tier 1 sats have screens)
#   BASE_IMAGE_TAG=...     override the base image reference
#
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
JOBS="$(nproc)"
SAT_BUILD="${SAT_BUILD:-full}"
BASE_IMAGE_TAG="${BASE_IMAGE_TAG:-ghcr.io/the-oasis-project/dawn-satellite-build:trixie-arm64}"

if [ "$SAT_BUILD" = "headless" ]; then
   IMAGE="dawn-satellite-pibuild"
   echo "==> [headless] building apt-only build env ($IMAGE)…"
   docker build --platform linux/arm64 -t "$IMAGE" \
       -f "$SCRIPT_DIR/Dockerfile.pibuild" "$SCRIPT_DIR"
   CMAKE_FLAGS="-DENABLE_VAD=OFF -DENABLE_WHISPER_ASR=OFF -DENABLE_VOSK_ASR=OFF \
       -DENABLE_TTS=OFF -DENABLE_SDL_UI=OFF -DENABLE_DISPLAY=OFF"
else
   IMAGE="$BASE_IMAGE_TAG"
   if ! docker image inspect "$IMAGE" >/dev/null 2>&1; then
      echo "==> base image not present locally; pulling $IMAGE…"
      if ! docker pull "$IMAGE"; then
         echo "==> pull failed; building base locally from Dockerfile.pibase (one-time, heavy)…"
         docker build --platform linux/arm64 -t "$IMAGE" \
             -f "$SCRIPT_DIR/Dockerfile.pibase" "$SCRIPT_DIR"
      fi
   fi
   SAT_ASR="${SAT_ASR:-whisper}"
   if [ "$SAT_ASR" = "vosk" ]; then
      ASR_FLAGS="-DENABLE_VOSK_ASR=ON -DENABLE_WHISPER_ASR=OFF"
   else
      ASR_FLAGS="-DENABLE_WHISPER_ASR=ON -DENABLE_VOSK_ASR=OFF"
   fi
   if [ "${SAT_SDL:-on}" = "on" ]; then
      SDL_FLAG="-DENABLE_SDL_UI=ON"
   else
      SDL_FLAG="-DENABLE_SDL_UI=OFF"
   fi
   CMAKE_FLAGS="-DENABLE_VAD=ON -DENABLE_TTS=ON $ASR_FLAGS $SDL_FLAG -DENABLE_DISPLAY=OFF"
fi

echo "==> Building dawn_satellite ($SAT_BUILD) in $IMAGE…"
echo "    cmake flags: $CMAKE_FLAGS"
# Mount the WHOLE repo: the satellite CMake reaches ../common, ../include,
# ../cmake, ../whisper.cpp.  Run as the host uid so build-pi/ is host-owned.
# WORKDIR (/repo/dawn_satellite) is set in the image.
docker run --rm --platform linux/arm64 \
    -u "$(id -u):$(id -g)" \
    -v "$REPO_ROOT":/repo \
    "$IMAGE" \
    bash -c "cmake -S . -B build-pi $CMAKE_FLAGS && make -C build-pi -j${JOBS}"

BIN="$REPO_ROOT/dawn_satellite/build-pi/dawn_satellite"
FW_VER="$(sed -n 's/.*DAWN_SATELLITE_FIRMWARE_VERSION[[:space:]]*"\([^"]*\)".*/\1/p' \
   "$REPO_ROOT/dawn_satellite/include/satellite_version.h" 2>/dev/null)"
echo "==> Done."
file "$BIN" 2>/dev/null || true
echo "    Binary: $BIN"
echo "    Deploy: scp '$BIN' <user>@<pi>:/tmp/dawn_satellite_test"
echo
echo "    OTA release (run on the daemon host): sign + stage this image, then push"
echo "    it to a device from the WebUI satellite panel."
echo "    Build-pi already baked version ${FW_VER:-X.Y.Z} into the binary; --version must match it."
echo "      sudo ./dawn_satellite/ota-release.sh --version ${FW_VER:-X.Y.Z}"
