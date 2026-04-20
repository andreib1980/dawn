#!/bin/bash
#
# DAWN Installation — Satellite (Tier 1) support
# Provides satellite-specific phase implementations for scripts/install.sh.
# Invoked only when INSTALL_TARGET=satellite.
#
# Sourced by install.sh. Do not execute directly.
#

# ─────────────────────────────────────────────────────────────────────────────
# Satellite apt package list
# ─────────────────────────────────────────────────────────────────────────────

SATELLITE_APT_PACKAGES=(
   # Build tools
   build-essential
   cmake
   git
   pkg-config
   autoconf
   automake
   libtool
   # Required runtime libs
   libasound2-dev
   libwebsockets-dev
   libjson-c-dev
   libspdlog-dev
   libopus-dev
   # Utility — needed by libvosk unzip step
   unzip
   wget
   ca-certificates
)

# SDL2 packages added conditionally by present_satellite_choices
SATELLITE_SDL_PACKAGES=(
   libsdl2-dev
   libsdl2-ttf-dev
   libsdl2-gfx-dev
   libdrm-dev
)

# ─────────────────────────────────────────────────────────────────────────────
# Satellite-specific user choices
#
# Preferences set here (mirrored via save_state for resume):
#   SAT_SERVER_HOST, SAT_SERVER_PORT, SAT_SSL, SAT_SSL_VERIFY
#   SAT_NAME, SAT_LOCATION
#   SAT_ASR_ENGINE (vosk|whisper), SAT_VOSK_MODEL (small|large)
#   SAT_WHISPER_MODEL (tiny-q5_1|base)
#   SAT_ENABLE_SDL_UI (true|false), SAT_ENABLE_OPUS (true|false)
#   SAT_CAPTURE_DEVICE, SAT_PLAYBACK_DEVICE
#   SAT_REGISTRATION_KEY (may be empty)
# ─────────────────────────────────────────────────────────────────────────────

# Populate RAM-based defaults. Called from run_satellite_discovery before
# present_satellite_choices so prompts can suggest sensible values.
compute_satellite_recommendations() {
   # Defaults (used when memory detection fails or is reported as 0)
   RECOMMENDED_ASR="vosk"
   RECOMMENDED_VOSK_MODEL="small"
   RECOMMENDED_WHISPER_MODEL="tiny-q5_1"

   if [ "${SYSTEM_MEM_GB:-0}" -ge 8 ]; then
      # Pi 5 8GB or similar — room for the large Vosk model or Whisper base
      RECOMMENDED_ASR="vosk"
      RECOMMENDED_VOSK_MODEL="large"
      RECOMMENDED_WHISPER_MODEL="base"
   elif [ "${SYSTEM_MEM_GB:-0}" -ge 4 ]; then
      # Pi 4 4GB / Pi 5 4GB
      RECOMMENDED_ASR="vosk"
      RECOMMENDED_VOSK_MODEL="small"
      RECOMMENDED_WHISPER_MODEL="tiny-q5_1"
   elif [ "${SYSTEM_MEM_GB:-0}" -ge 2 ]; then
      # Pi 4 2GB / Pi 3B+ — Vosk only, small model
      RECOMMENDED_ASR="vosk"
      RECOMMENDED_VOSK_MODEL="small"
      RECOMMENDED_WHISPER_MODEL="tiny-q5_1"
   fi
}

# Print the satellite-flavoured discovery summary (RAM + recommendation).
# Called after the generic run_discovery() completes.
run_satellite_discovery() {
   compute_satellite_recommendations

   echo ""
   log "Satellite-Specific Recommendations"
   printf "  %-22s %s\n" "Detected RAM:" "${SYSTEM_MEM_GB}GB"
   printf "  %-22s %s\n" "Recommended ASR:" "$RECOMMENDED_ASR"
   if [ "$RECOMMENDED_ASR" = "vosk" ]; then
      printf "  %-22s %s\n" "Recommended model:" "Vosk $RECOMMENDED_VOSK_MODEL"
   else
      printf "  %-22s %s\n" "Recommended model:" "Whisper $RECOMMENDED_WHISPER_MODEL"
   fi

   if [ "${SYSTEM_MEM_GB:-0}" -lt 1 ]; then
      warn "System RAM < 1GB — satellite voice processing is not recommended on this hardware."
   elif [ "${SYSTEM_MEM_GB:-0}" -lt 2 ]; then
      warn "System RAM < 2GB — expect tight memory budget. Vosk small + text-only fallback recommended."
   fi
   echo ""
}

present_satellite_choices() {
   header "Satellite Configuration"

   # ── 1. Daemon connection ──
   if [ -z "${SAT_SERVER_HOST:-}" ]; then
      echo "Daemon connection:"
      SAT_SERVER_HOST=$(ask_value "DAWN daemon IP or hostname" "")
      if [ -z "$SAT_SERVER_HOST" ]; then
         warn "No daemon host set — you'll need to edit satellite.toml after install."
      fi
   fi
   : "${SAT_SERVER_PORT:=3000}"
   : "${SAT_SSL:=true}"
   : "${SAT_SSL_VERIFY:=true}"
   log "Daemon: ${SAT_SERVER_HOST:-<unset>}:${SAT_SERVER_PORT} (SSL: $SAT_SSL)"

   # ── 2. Identity ──
   if [ -z "${SAT_NAME:-}" ]; then
      local default_name
      default_name="$(hostname -s 2>/dev/null || echo "satellite")"
      SAT_NAME=$(ask_value "Satellite name (shown in daemon logs/WebUI)" "$default_name")
   fi
   if [ -z "${SAT_LOCATION:-}" ]; then
      SAT_LOCATION=$(ask_value "Room/location (e.g. kitchen, bedroom)" "")
   fi
   log "Identity: name=$SAT_NAME, location=${SAT_LOCATION:-<unset>}"

   # ── 3. ASR engine + model ──
   if [ -z "${SAT_ASR_ENGINE:-}" ]; then
      echo ""
      echo "ASR engine:"
      echo "  vosk     streaming, near-instant finalize, lower latency (recommended on Pi)"
      echo "  whisper  batch, slightly higher accuracy, ~4s finalize on Pi 4"
      SAT_ASR_ENGINE=$(ask_value "ASR engine" "$RECOMMENDED_ASR")
   fi
   case "$SAT_ASR_ENGINE" in
      vosk | whisper) ;;
      *) error "Invalid ASR engine: $SAT_ASR_ENGINE (must be vosk or whisper)" ;;
   esac

   if [ "$SAT_ASR_ENGINE" = "vosk" ] && [ -z "${SAT_VOSK_MODEL:-}" ]; then
      echo ""
      echo "Vosk model size:"
      echo "  small   ~40MB,  good for commands, low RAM"
      echo "  large   ~1.8GB, higher accuracy, 4GB+ RAM recommended"
      SAT_VOSK_MODEL=$(ask_value "Vosk model" "$RECOMMENDED_VOSK_MODEL")
   fi

   if [ "$SAT_ASR_ENGINE" = "whisper" ] && [ -z "${SAT_WHISPER_MODEL:-}" ]; then
      echo ""
      echo "Whisper model:"
      echo "  tiny-q5_1  ~30MB,  fastest"
      echo "  base       ~142MB, better accuracy"
      SAT_WHISPER_MODEL=$(ask_value "Whisper model" "$RECOMMENDED_WHISPER_MODEL")
   fi
   log "ASR: engine=$SAT_ASR_ENGINE, model=${SAT_VOSK_MODEL:-${SAT_WHISPER_MODEL:-?}}"

   # ── 4. Optional features ──
   if [ -z "${SAT_ENABLE_OPUS:-}" ]; then
      echo ""
      echo "  Music streaming plays audio from the daemon over a dedicated Opus WebSocket."
      if ask_yes_no "  Enable music streaming?" "default_yes"; then
         SAT_ENABLE_OPUS=true
      else
         SAT_ENABLE_OPUS=false
      fi
   fi

   if [ -z "${SAT_ENABLE_SDL_UI:-}" ]; then
      echo ""
      echo "  The SDL2 touchscreen UI adds an orb visualizer, transcript, and music panel."
      echo "  Requires an attached display (HDMI or DSI) and Pi OS Lite with KMSDRM."
      if ask_yes_no "  Enable SDL touchscreen UI?"; then
         SAT_ENABLE_SDL_UI=true
      else
         SAT_ENABLE_SDL_UI=false
      fi
   fi
   log "Features: music=$SAT_ENABLE_OPUS, sdl_ui=$SAT_ENABLE_SDL_UI"

   # ── 5. Audio devices ──
   if [ -z "${SAT_CAPTURE_DEVICE:-}" ] || [ -z "${SAT_PLAYBACK_DEVICE:-}" ]; then
      echo ""
      echo "Available capture devices (arecord -l):"
      arecord -l 2>/dev/null | grep -E '^card [0-9]' || echo "  (none detected)"
      echo ""
      echo "Available playback devices (aplay -l):"
      aplay -l 2>/dev/null | grep -E '^card [0-9]' || echo "  (none detected)"
      echo ""
      echo "Device format: plughw:CARD,DEVICE (e.g. plughw:1,0 for card 1 device 0)"
   fi
   : "${SAT_CAPTURE_DEVICE:=$(ask_value "Capture device" "plughw:1,0")}"
   : "${SAT_PLAYBACK_DEVICE:=$(ask_value "Playback device" "plughw:1,0")}"
   log "Audio: capture=$SAT_CAPTURE_DEVICE, playback=$SAT_PLAYBACK_DEVICE"

   # ── 6. Registration key ──
   if [ -z "${SAT_REGISTRATION_KEY:-}" ] && [ "${INTERACTIVE:-true}" = true ]; then
      echo ""
      echo "If the daemon requires a registration key (./generate_ssl_cert.sh --gen-key"
      echo "was run on the daemon host), paste it here. Leave blank to skip."
      SAT_REGISTRATION_KEY=$(ask_secret "Registration key (or Enter to skip)")
   fi

   echo ""
}

# ─────────────────────────────────────────────────────────────────────────────
# Phase 1 (satellite) — system dependencies
# ─────────────────────────────────────────────────────────────────────────────

run_deps_satellite() {
   header "Phase 1: System Dependencies (satellite)"
   CURRENT_PHASE="deps"
   sudo_begin_phase "deps"

   # Assemble the package list based on chosen features
   local -a pkgs=("${SATELLITE_APT_PACKAGES[@]}")
   if [ "${SAT_ENABLE_SDL_UI:-false}" = true ]; then
      pkgs+=("${SATELLITE_SDL_PACKAGES[@]}")
   fi

   log "Updating apt index..."
   run_sudo apt-get update -qq || error "apt-get update failed"

   log "Installing ${#pkgs[@]} packages..."
   run_sudo apt-get install -y "${pkgs[@]}" || error "Failed to install required apt packages"

   check_cmake_version

   log "Phase 1 complete"
}

# ─────────────────────────────────────────────────────────────────────────────
# Phase 2 (satellite) — core libraries
# ─────────────────────────────────────────────────────────────────────────────

run_libs_satellite() {
   header "Phase 2: Core Libraries (satellite)"
   CURRENT_PHASE="libs"
   sudo_begin_phase "libs"

   install_spdlog
   install_espeak_ng
   sudo_keepalive
   # Satellite always uses pre-built ONNX Runtime (no CUDA on Pi)
   install_onnxruntime_prebuilt
   sudo_keepalive
   install_piper_phonemize
   install_libvosk
   setup_library_path

   # Verify
   local failed=()
   has_lib "libonnxruntime.so" || [ -f /usr/local/lib/libonnxruntime.so.1 ] || failed+=("onnxruntime")
   has_lib "libpiper_phonemize.so" || failed+=("piper-phonemize")
   has_lib "libvosk.so" || failed+=("libvosk")

   if [ ${#failed[@]} -gt 0 ]; then
      error "Core libraries missing after install: ${failed[*]}"
   fi

   log "Phase 2 complete — all satellite libraries verified"
}

# ─────────────────────────────────────────────────────────────────────────────
# Phase 3 (satellite) — build the satellite binary
# ─────────────────────────────────────────────────────────────────────────────

run_build_satellite() {
   header "Phase 3: Build Satellite"
   CURRENT_PHASE="build"

   ensure_submodules

   local sat_src="$PROJECT_ROOT/dawn_satellite"
   if [ ! -d "$sat_src" ]; then
      error "Satellite source not found at $sat_src"
   fi

   BUILD_DIR="$sat_src/build"
   mkdir -p "$BUILD_DIR"
   cd "$BUILD_DIR" || error "Cannot enter build dir $BUILD_DIR"

   # Compose CMake flags from user choices
   local -a cmake_flags=(
      -DENABLE_DAP2=ON
      -DENABLE_NEOPIXEL=OFF
      -DCMAKE_BUILD_TYPE=Release
   )
   if [ "${SAT_ASR_ENGINE:-vosk}" = "whisper" ]; then
      cmake_flags+=(-DENABLE_WHISPER_ASR=ON -DENABLE_VOSK_ASR=OFF)
   fi
   if [ "${SAT_ENABLE_SDL_UI:-false}" = true ]; then
      cmake_flags+=(-DENABLE_SDL_UI=ON)
   fi

   log "Configuring CMake: ${cmake_flags[*]}"
   local cmake_log
   cmake_log=$(mktemp)
   register_cleanup "$cmake_log"
   if ! cmake .. "${cmake_flags[@]}" 2>&1 | tee "$cmake_log"; then
      error "CMake configuration failed"
   fi

   # Hard-fail if user-requested features landed as OFF. This catches the
   # exact failure mode that motivated the installer: silent CMake warnings
   # leading to a build without VAD/ASR/TTS.
   local -a required_on=(
      "VAD (Silero): ON"
      "TTS (Piper): ON"
   )
   if [ "${SAT_ASR_ENGINE:-vosk}" = "vosk" ]; then
      required_on+=("ASR (Vosk): ON")
   else
      required_on+=("ASR (Whisper): ON")
   fi
   if [ "${SAT_ENABLE_OPUS:-true}" = true ]; then
      required_on+=("Music (Opus): ON")
   fi

   local missing=()
   local feature
   for feature in "${required_on[@]}"; do
      if ! grep -qF "$feature" "$cmake_log"; then
         missing+=("$feature")
      fi
   done
   if [ ${#missing[@]} -gt 0 ]; then
      warn "CMake did not enable required features:"
      for feature in "${missing[@]}"; do
         warn "   expected: $feature"
      done
      error "Build aborted. See docs/DAP2_SATELLITE.md → Troubleshooting → 'Build fails with DISABLED warnings'"
   fi

   log "Building satellite (parallelism: $(nproc))..."
   if ! make -j"$(nproc)"; then
      error "Satellite build failed"
   fi

   if [ ! -f "$BUILD_DIR/dawn_satellite" ]; then
      error "Build completed but dawn_satellite binary not found at $BUILD_DIR/dawn_satellite"
   fi

   local binary_size
   binary_size=$(du -sh "$BUILD_DIR/dawn_satellite" | cut -f1)
   log "Binary built: $BUILD_DIR/dawn_satellite ($binary_size)"

   cd "$PROJECT_ROOT" || return
   log "Phase 3 complete"
}

# ─────────────────────────────────────────────────────────────────────────────
# Phase 4 (satellite) — download models
# ─────────────────────────────────────────────────────────────────────────────

run_models_satellite() {
   header "Phase 4: Download Models (satellite)"
   CURRENT_PHASE="models"

   local model_script="$PROJECT_ROOT/setup_models.sh"
   [ -x "$model_script" ] || chmod +x "$model_script" 2>/dev/null || true
   if [ ! -f "$model_script" ]; then
      error "setup_models.sh not found at $PROJECT_ROOT"
   fi

   local -a args=()
   if [ "${SAT_ASR_ENGINE:-vosk}" = "vosk" ]; then
      if [ "${SAT_VOSK_MODEL:-small}" = "small" ]; then
         args+=(--vosk-small)
      else
         args+=(--vosk)
      fi
   else
      # Whisper branch
      local wm="${SAT_WHISPER_MODEL:-tiny-q5_1}"
      args+=(--whisper-model "$wm")
   fi

   log "Running setup_models.sh ${args[*]}"
   "$model_script" "${args[@]}" || error "Model download failed"

   log "Phase 4 complete"
}

# ─────────────────────────────────────────────────────────────────────────────
# Phase 5 (satellite) — write satellite.toml
# ─────────────────────────────────────────────────────────────────────────────

run_configure_satellite() {
   header "Phase 5: Configure Satellite"
   CURRENT_PHASE="configure"

   local cfg_src="$PROJECT_ROOT/dawn_satellite/config/satellite.toml"
   local cfg_dst="$PROJECT_ROOT/dawn_satellite/build/satellite.toml"
   if [ ! -f "$cfg_src" ]; then
      error "Default config not found at $cfg_src"
   fi

   cp "$cfg_src" "$cfg_dst" || error "Failed to copy satellite.toml into build dir"

   # Apply user choices via sed_safe_set. It auto-quotes string values and
   # leaves booleans/numbers bare — pass raw values, no manual quoting.
   if [ -n "${SAT_SERVER_HOST:-}" ]; then
      sed_safe_set "$cfg_dst" "server" "host" "$SAT_SERVER_HOST"
   fi
   sed_safe_set "$cfg_dst" "server" "port" "${SAT_SERVER_PORT:-3000}"
   sed_safe_set "$cfg_dst" "server" "ssl" "${SAT_SSL:-true}"
   sed_safe_set "$cfg_dst" "server" "ssl_verify" "${SAT_SSL_VERIFY:-true}"
   if [ -n "${SAT_REGISTRATION_KEY:-}" ]; then
      sed_safe_set "$cfg_dst" "server" "registration_key" "$SAT_REGISTRATION_KEY"
   fi

   if [ -n "${SAT_NAME:-}" ]; then
      sed_safe_set "$cfg_dst" "identity" "name" "$SAT_NAME"
   fi
   if [ -n "${SAT_LOCATION:-}" ]; then
      sed_safe_set "$cfg_dst" "identity" "location" "$SAT_LOCATION"
   fi

   sed_safe_set "$cfg_dst" "audio" "capture_device" "${SAT_CAPTURE_DEVICE:-plughw:1,0}"
   sed_safe_set "$cfg_dst" "audio" "playback_device" "${SAT_PLAYBACK_DEVICE:-plughw:1,0}"

   sed_safe_set "$cfg_dst" "asr" "engine" "${SAT_ASR_ENGINE:-vosk}"
   local model_path
   if [ "${SAT_ASR_ENGINE:-vosk}" = "vosk" ]; then
      if [ "${SAT_VOSK_MODEL:-small}" = "small" ]; then
         model_path="$PROJECT_ROOT/models/vosk-model-small-en-us-0.15"
      else
         model_path="$PROJECT_ROOT/models/vosk-model-en-us-0.22"
      fi
   else
      model_path="$PROJECT_ROOT/models/whisper.cpp/ggml-${SAT_WHISPER_MODEL:-tiny-q5_1}.en.bin"
   fi
   sed_safe_set "$cfg_dst" "asr" "model_path" "$model_path"

   if [ "${SAT_ENABLE_SDL_UI:-false}" = true ]; then
      sed_safe_set "$cfg_dst" "sdl_ui" "enabled" "true"
   fi

   log "Wrote $cfg_dst"
   log "Phase 5 complete"
}

# ─────────────────────────────────────────────────────────────────────────────
# Phase: deploy — install the systemd service via services/dawn-satellite
# Uses the existing run_deploy "satellite" function in install.sh.
# ─────────────────────────────────────────────────────────────────────────────

# ─────────────────────────────────────────────────────────────────────────────
# Phase: verify — satellite-specific verification
# ─────────────────────────────────────────────────────────────────────────────

run_verify_satellite() {
   header "Phase 10: Verification (satellite)"
   CURRENT_PHASE="verify"

   local pass=0 fail=0

   local bin="$PROJECT_ROOT/dawn_satellite/build/dawn_satellite"
   [ -f "/usr/local/bin/dawn_satellite" ] && bin="/usr/local/bin/dawn_satellite"

   if [ -f "$bin" ]; then
      log "Binary: $bin"
      ((++pass))
   else
      warn "Binary not found"
      ((++fail))
   fi

   if has_lib "libvosk.so"; then
      log "libvosk: OK"
      ((++pass))
   else
      warn "libvosk: missing"
      ((++fail))
   fi

   if has_lib "libonnxruntime.so" || [ -f /usr/local/lib/libonnxruntime.so.1 ]; then
      log "ONNX Runtime: OK"
      ((++pass))
   else
      warn "ONNX Runtime: missing"
      ((++fail))
   fi

   if has_lib "libpiper_phonemize.so"; then
      log "piper-phonemize: OK"
      ((++pass))
   else
      warn "piper-phonemize: missing"
      ((++fail))
   fi

   local svc_status
   svc_status=$(check_service_satellite)
   case "$svc_status" in
      PASS*) log "dawn-satellite service: active" ; ((++pass)) ;;
      SKIP*) info "dawn-satellite service: not installed yet (run deploy)" ;;
      FAIL*) warn "dawn-satellite service: enabled but not active" ; ((++fail)) ;;
   esac

   echo ""
   log "Satellite verify: $pass passed, $fail failed"
   [ "$fail" -eq 0 ]
}
