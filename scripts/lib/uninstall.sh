#!/bin/bash
#
# DAWN Installation - Uninstall
# Removes DAWN-installed components from the system.
#
# Sourced by install.sh. Do not execute directly.
#
# What is removed (with confirmation prompts):
#   - systemd service (dawn-server and/or dawn-satellite)
#   - cmake-installed binaries (dawn, dawn-admin)
#   - cmake-installed shared libraries (whisper, ggml)
#   - DAWN-specific libraries (piper-phonemize, ONNX Runtime, espeak-ng rhasspy fork)
#   - CA certificate from system trust store
#   - Headless audio config (snd-dummy)
#   - Docker containers (SearXNG, FlareSolverr)
#   - Installer state file
#   - dawn system user
#
# What is NOT removed:
#   - apt packages (shared system dependencies)
#   - Project source tree (the git repo)
#   - dawn.toml / secrets.toml in the project directory
#   - CMake upgrades (shared tool)
#   - Build directories (in-tree, user can rm manually)
#

# ─────────────────────────────────────────────────────────────────────────────
# Counters
# ─────────────────────────────────────────────────────────────────────────────

_REMOVED=0
_SKIPPED=0

_count_removed() { ((_REMOVED++)) || true; }
_count_skipped() { ((_SKIPPED++)) || true; }

# ─────────────────────────────────────────────────────────────────────────────
# Step 0: Stop running dawn processes
# ─────────────────────────────────────────────────────────────────────────────

stop_running_processes() {
   local pids
   pids=$(pgrep -x "dawn|dawn_satellite" 2>/dev/null || true)

   if [ -z "$pids" ]; then
      log "Running processes: none found"
      return 0
   fi

   log "Found running DAWN process(es): $pids"
   if ask_yes_no "Stop running DAWN processes?"; then
      for pid in $pids; do
         local cmd
         cmd=$(ps -p "$pid" -o comm= 2>/dev/null || echo "unknown")
         log "  Stopping $cmd (PID $pid)..."
         kill "$pid" 2>/dev/null || true
      done

      # Wait briefly for graceful shutdown
      local tries=0
      while [ $tries -lt 5 ]; do
         pids=$(pgrep -x "dawn|dawn_satellite" 2>/dev/null || true)
         [ -z "$pids" ] && break
         sleep 1
         ((tries++)) || true
      done

      # Force kill if still running
      pids=$(pgrep -x "dawn|dawn_satellite" 2>/dev/null || true)
      if [ -n "$pids" ]; then
         warn "Processes still running after 5s — sending SIGKILL"
         for pid in $pids; do
            kill -9 "$pid" 2>/dev/null || true
         done
      fi

      log "  All DAWN processes stopped"
      _count_removed
   else
      warn "Skipping — running processes may cause port conflicts on reinstall"
      _count_skipped
   fi
}

# ─────────────────────────────────────────────────────────────────────────────
# Step 1: Systemd services
# ─────────────────────────────────────────────────────────────────────────────

uninstall_services() {
   local has_server=false has_satellite=false

   [ -f "/etc/systemd/system/dawn-server.service" ] && has_server=true
   [ -f "/etc/systemd/system/dawn-satellite.service" ] && has_satellite=true

   if [ "$has_server" = false ] && [ "$has_satellite" = false ]; then
      log "Services: none installed"
      return 0
   fi

   # Delegate to existing service uninstallers if available
   if [ "$has_server" = true ]; then
      local server_script="$PROJECT_ROOT/services/dawn-server/install.sh"
      if [ -f "$server_script" ]; then
         log "Uninstalling dawn-server service..."
         sudo_begin_phase "uninstall-service"
         run_sudo bash "$server_script" --uninstall
         _count_removed
      else
         warn "dawn-server service found but install.sh missing — removing manually"
         _uninstall_service_manual "dawn-server"
      fi
   fi

   if [ "$has_satellite" = true ]; then
      local sat_script="$PROJECT_ROOT/services/dawn-satellite/install.sh"
      if [ -f "$sat_script" ]; then
         log "Uninstalling dawn-satellite service..."
         sudo_begin_phase "uninstall-service"
         run_sudo bash "$sat_script" --uninstall
         _count_removed
      else
         warn "dawn-satellite service found but install.sh missing — removing manually"
         _uninstall_service_manual "dawn-satellite"
      fi
   fi
}

# Fallback manual service removal if the service install script is missing
_uninstall_service_manual() {
   local name="$1"
   sudo_begin_phase "uninstall-service"
   if systemctl is-active --quiet "$name" 2>/dev/null; then
      run_sudo systemctl stop "$name"
   fi
   if systemctl is-enabled --quiet "$name" 2>/dev/null; then
      run_sudo systemctl disable "$name"
   fi
   run_sudo rm -f "/etc/systemd/system/${name}.service"
   run_sudo systemctl daemon-reload
   run_sudo rm -f "/etc/logrotate.d/${name}"
   _count_removed
}

# ─────────────────────────────────────────────────────────────────────────────
# Step 2: cmake-installed binaries
# ─────────────────────────────────────────────────────────────────────────────

uninstall_binaries() {
   local bins=(/usr/local/bin/dawn /usr/local/bin/dawn-admin)
   local found=()

   for bin in "${bins[@]}"; do
      [ -f "$bin" ] && found+=("$bin")
   done

   if [ ${#found[@]} -eq 0 ]; then
      log "Binaries: none installed in /usr/local/bin/"
      return 0
   fi

   log "Found DAWN binaries: ${found[*]}"
   if ask_yes_no "Remove DAWN binaries from /usr/local/bin/?"; then
      sudo_begin_phase "uninstall-binaries"
      for bin in "${found[@]}"; do
         run_sudo rm -f "$bin"
         log "  Removed $bin"
      done
      _count_removed
   else
      _count_skipped
   fi
}

# ─────────────────────────────────────────────────────────────────────────────
# Step 3: cmake-installed shared libraries (whisper, ggml)
# ─────────────────────────────────────────────────────────────────────────────

uninstall_cmake_libs() {
   # Libraries installed by whisper.cpp/ggml cmake install() rules
   local patterns=(
      "libwhisper.so*"
      "libggml.so*"
      "libggml-base.so*"
      "libggml-cpu.so*"
      "libggml-cuda.so*"
   )
   local found=()

   for pat in "${patterns[@]}"; do
      # shellcheck disable=SC2206
      local matches=(/usr/local/lib/$pat)
      for f in "${matches[@]}"; do
         [ -f "$f" ] && found+=("$f")
      done
   done

   # Also check for whisper cmake config and pkg-config files
   local extras=(
      /usr/local/lib/cmake/whisper
      /usr/local/lib/pkgconfig/whisper.pc
      /usr/local/include/whisper.h
      /usr/local/include/ggml.h
      /usr/local/include/ggml-alloc.h
      /usr/local/include/ggml-backend.h
      /usr/local/include/ggml-cpp.h
   )
   for f in "${extras[@]}"; do
      [ -e "$f" ] && found+=("$f")
   done

   if [ ${#found[@]} -eq 0 ]; then
      log "Whisper/GGML libs: none found"
      return 0
   fi

   log "Found ${#found[@]} whisper/ggml files in /usr/local/"
   if ask_yes_no "Remove whisper/ggml libraries and headers?"; then
      sudo_begin_phase "uninstall-cmake-libs"
      for f in "${found[@]}"; do
         if [ -d "$f" ]; then
            run_sudo rm -rf "$f"
         else
            run_sudo rm -f "$f"
         fi
      done
      run_sudo ldconfig
      log "  Removed ${#found[@]} whisper/ggml files"
      _count_removed
   else
      _count_skipped
   fi
}

# ─────────────────────────────────────────────────────────────────────────────
# Step 4: DAWN-specific libraries (piper-phonemize, ONNX Runtime)
# ─────────────────────────────────────────────────────────────────────────────

uninstall_dawn_libs() {
   local found=()
   local descriptions=()

   # piper-phonemize
   local piper_files=()
   # shellcheck disable=SC2206
   piper_files=(/usr/local/lib/libpiper_phonemize.so*)
   for f in "${piper_files[@]}"; do
      [ -f "$f" ] && found+=("$f")
   done
   [ -d /usr/local/include/piper-phonemize ] && found+=("/usr/local/include/piper-phonemize")
   if [ ${#found[@]} -gt 0 ]; then
      descriptions+=("piper-phonemize")
   fi

   # ONNX Runtime
   local onnx_files=()
   # shellcheck disable=SC2206
   onnx_files=(/usr/local/lib/libonnxruntime*)
   local onnx_found=false
   for f in "${onnx_files[@]}"; do
      if [ -f "$f" ]; then
         found+=("$f")
         onnx_found=true
      fi
   done
   # Headers (only if libs were found)
   if [ "$onnx_found" = true ]; then
      for h in /usr/local/include/onnxruntime_c_api.h \
               /usr/local/include/onnxruntime_cxx_api.h \
               /usr/local/include/onnxruntime_cxx_inline.h \
               /usr/local/include/onnxruntime_float16.h \
               /usr/local/include/onnxruntime_session_options_config_keys.h \
               /usr/local/include/onnxruntime_run_options_config_keys.h; do
         [ -f "$h" ] && found+=("$h")
      done
      # Core subdirectory (some ONNX versions install here)
      [ -d /usr/local/include/core ] && found+=("/usr/local/include/core")
      descriptions+=("ONNX Runtime")
   fi

   # libvosk (installed by satellite builds via install_libvosk)
   local vosk_files=()
   # shellcheck disable=SC2206
   vosk_files=(/usr/local/lib/libvosk.so*)
   local vosk_found=false
   for f in "${vosk_files[@]}"; do
      if [ -f "$f" ]; then
         found+=("$f")
         vosk_found=true
      fi
   done
   if [ "$vosk_found" = true ]; then
      [ -f /usr/local/include/vosk_api.h ] && found+=("/usr/local/include/vosk_api.h")
      descriptions+=("libvosk")
   fi

   # whisper.cpp / ggml shared libs (installed by dawn-satellite service installer)
   local whisper_files=()
   # shellcheck disable=SC2206
   whisper_files=(/usr/local/lib/libwhisper.so* /usr/local/lib/libggml*.so*)
   local whisper_found=false
   for f in "${whisper_files[@]}"; do
      if [ -f "$f" ]; then
         found+=("$f")
         whisper_found=true
      fi
   done
   if [ "$whisper_found" = true ]; then
      descriptions+=("whisper.cpp/ggml libs")
   fi

   # Daemon CA cert staged by services/dawn-satellite/install.sh.
   # Only remove when the daemon service itself is NOT installed — the two
   # may share the same private CA on a co-hosted deployment.
   if [ -f /etc/dawn/ca.crt ] && [ ! -f /etc/systemd/system/dawn-server.service ]; then
      found+=("/etc/dawn/ca.crt")
      descriptions+=("/etc/dawn/ca.crt")
   fi

   if [ ${#found[@]} -eq 0 ]; then
      log "DAWN libraries: none found"
      return 0
   fi

   log "Found DAWN-specific libraries: ${descriptions[*]}"
   if ask_yes_no "Remove ${descriptions[*]}?"; then
      sudo_begin_phase "uninstall-libs"
      for f in "${found[@]}"; do
         if [ -d "$f" ]; then
            run_sudo rm -rf "$f"
         else
            run_sudo rm -f "$f"
         fi
      done
      run_sudo ldconfig
      log "  Removed ${#found[@]} files"
      _count_removed
   else
      _count_skipped
   fi
}

# ─────────────────────────────────────────────────────────────────────────────
# Step 5: espeak-ng rhasspy fork
# ─────────────────────────────────────────────────────────────────────────────

uninstall_espeak_ng() {
   # Check if rhasspy fork is installed (has the unique symbol)
   local multiarch
   multiarch=$(dpkg-architecture -qDEB_HOST_MULTIARCH 2>/dev/null || echo "")
   local rhasspy_found=false

   for so_path in /usr/local/lib/libespeak-ng.so* "/usr/lib/${multiarch}"/libespeak-ng.so*; do
      if [ -f "$so_path" ] && nm -D "$so_path" 2>/dev/null | grep -q TextToPhonemesWithTerminator; then
         rhasspy_found=true
         break
      fi
   done

   if [ "$rhasspy_found" = false ]; then
      log "espeak-ng (rhasspy fork): not found"
      return 0
   fi

   log "Found espeak-ng (rhasspy fork) — required by Piper TTS"
   warn "Removing this will break text-to-speech until reinstalled"
   if ask_yes_no "Remove espeak-ng rhasspy fork?"; then
      sudo_begin_phase "uninstall-espeak"

      # The rhasspy fork installs to the system lib path via make install
      # We need to remove its files but can offer to reinstall system espeak-ng
      run_sudo rm -f "/usr/lib/${multiarch}"/libespeak-ng.so* 2>/dev/null || true
      run_sudo rm -f /usr/local/lib/libespeak-ng.so* 2>/dev/null || true
      run_sudo rm -rf /usr/local/include/espeak-ng 2>/dev/null || true
      run_sudo rm -f /usr/local/bin/espeak-ng 2>/dev/null || true
      run_sudo ldconfig

      log "  espeak-ng rhasspy fork removed"

      if ask_yes_no "Reinstall system espeak-ng from apt?"; then
         run_sudo apt-get install -y espeak-ng-data libespeak-ng1 libespeak-ng-dev 2>/dev/null || true
         run_sudo ldconfig
         log "  System espeak-ng reinstalled (including dev headers)"
      fi
      _count_removed
   else
      _count_skipped
   fi
}

# ─────────────────────────────────────────────────────────────────────────────
# Step 6: CA certificate
# ─────────────────────────────────────────────────────────────────────────────

uninstall_ca_cert() {
   local ca_path="/usr/local/share/ca-certificates/dawn-ca.crt"
   if [ ! -f "$ca_path" ]; then
      log "CA certificate: not installed in system trust store"
      return 0
   fi

   log "Found DAWN CA certificate in system trust store"
   if ask_yes_no "Remove CA certificate from system trust store?"; then
      sudo_begin_phase "uninstall-ca"
      run_sudo rm -f "$ca_path"
      run_sudo update-ca-certificates 2>/dev/null || true
      log "  CA certificate removed from system trust store"
      log "  Note: ssl/ directory in the project tree was not touched"
      _count_removed
   else
      _count_skipped
   fi
}

# ─────────────────────────────────────────────────────────────────────────────
# Step 7: Headless audio (snd-dummy)
# ─────────────────────────────────────────────────────────────────────────────

uninstall_headless_audio() {
   local mod_conf="/etc/modules-load.d/snd-dummy.conf"
   if [ ! -f "$mod_conf" ]; then
      log "Headless audio: not configured"
      return 0
   fi

   if ! grep -q "snd-dummy" "$mod_conf" 2>/dev/null; then
      return 0
   fi

   log "Found snd-dummy module auto-load config"
   if ask_yes_no "Remove headless audio config (snd-dummy)?"; then
      sudo_begin_phase "uninstall-headless"
      run_sudo rm -f "$mod_conf"
      log "  Removed $mod_conf"
      log "  Note: snd-dummy module is still loaded until next reboot"
      _count_removed
   else
      _count_skipped
   fi
}

# ─────────────────────────────────────────────────────────────────────────────
# Step 8: ldconfig entry
# ─────────────────────────────────────────────────────────────────────────────

uninstall_ldconfig() {
   local ld_conf="/etc/ld.so.conf.d/dawn.conf"
   if [ ! -f "$ld_conf" ]; then
      log "ldconfig: no dawn.conf entry"
      return 0
   fi

   log "Found /etc/ld.so.conf.d/dawn.conf"
   if ask_yes_no "Remove ldconfig entry?"; then
      sudo_begin_phase "uninstall-ldconfig"
      run_sudo rm -f "$ld_conf"
      run_sudo ldconfig
      log "  Removed ldconfig entry"
      _count_removed
   else
      _count_skipped
   fi
}

# ─────────────────────────────────────────────────────────────────────────────
# Step 9: Docker containers (SearXNG, FlareSolverr)
# ─────────────────────────────────────────────────────────────────────────────

uninstall_docker_containers() {
   if ! has_command docker; then
      return 0
   fi

   # Use sudo for docker commands if user isn't in docker group
   local _docker="docker"
   if ! docker info >/dev/null 2>&1; then
      _docker="sudo docker"
   fi
   local _docker_compose="docker compose"
   if ! docker compose version >/dev/null 2>&1 || ! docker info >/dev/null 2>&1; then
      _docker_compose="sudo docker compose"
   fi

   local found=()

   # SearXNG (docker compose)
   local searx_dir="$HOME/docker/searxng"
   if [ -f "$searx_dir/docker-compose.yml" ]; then
      found+=("searxng")
   fi

   # FlareSolverr (standalone container)
   if $_docker ps -a --filter name=flaresolverr --format '{{.Names}}' 2>/dev/null | grep -q flaresolverr; then
      found+=("flaresolverr")
   fi

   if [ ${#found[@]} -eq 0 ]; then
      log "Docker containers: none found"
      return 0
   fi

   log "Found Docker containers: ${found[*]}"
   if ask_yes_no "Remove DAWN Docker containers (${found[*]})?"; then
      for container in "${found[@]}"; do
         case "$container" in
            searxng)
               log "  Stopping SearXNG..."
               (cd "$searx_dir" && $_docker_compose down 2>/dev/null) || true
               if ask_yes_no "  Remove SearXNG data directory ($searx_dir)?"; then
                  sudo rm -rf "$searx_dir"
                  log "  Removed $searx_dir"
               fi
               ;;
            flaresolverr)
               log "  Stopping FlareSolverr..."
               $_docker stop flaresolverr 2>/dev/null || true
               $_docker rm flaresolverr 2>/dev/null || true
               log "  Removed flaresolverr container"
               ;;
         esac
      done
      _count_removed
   else
      _count_skipped
   fi
}

# ─────────────────────────────────────────────────────────────────────────────
# Step 10: Installer state file
# ─────────────────────────────────────────────────────────────────────────────

uninstall_state() {
   local state_dir="${XDG_STATE_HOME:-$HOME/.local/state}/dawn"
   if [ ! -d "$state_dir" ]; then
      log "Installer state: not found"
      return 0
   fi

   log "Removing installer state: $state_dir"
   rm -rf "$state_dir"
   _count_removed
}

# ─────────────────────────────────────────────────────────────────────────────
# Step 11: User data directory (dev mode)
# ─────────────────────────────────────────────────────────────────────────────

uninstall_user_data() {
   local data_dir="$HOME/.local/share/dawn"
   if [ ! -d "$data_dir" ]; then
      log "User data: not found"
      return 0
   fi

   # Check for databases
   local db_count=0
   db_count=$(find "$data_dir" -name "*.db" 2>/dev/null | wc -l)

   if [ "$db_count" -gt 0 ]; then
      warn "Found $db_count database(s) in $data_dir"
      warn "These contain user accounts, memory, calendar, email, and document data"
      if ask_yes_no "Remove user data directory ($data_dir)?"; then
         rm -rf "$data_dir"
         log "  Removed $data_dir"
         _count_removed
      else
         _count_skipped
      fi
   else
      log "Removing empty user data directory: $data_dir"
      rm -rf "$data_dir"
      _count_removed
   fi
}

# ─────────────────────────────────────────────────────────────────────────────
# Step 12: dawn system user
# ─────────────────────────────────────────────────────────────────────────────

uninstall_system_user() {
   if ! id -u dawn &>/dev/null; then
      log "System user 'dawn': not found"
      return 0
   fi

   log "Found system user 'dawn'"
   if ask_yes_no "Remove system user 'dawn'?"; then
      sudo_begin_phase "uninstall-user"
      run_sudo userdel dawn 2>/dev/null || warn "Failed to remove user 'dawn'"
      log "  System user 'dawn' removed"
      _count_removed
   else
      _count_skipped
   fi
}

# ─────────────────────────────────────────────────────────────────────────────
# Entry point
# ─────────────────────────────────────────────────────────────────────────────

run_uninstall() {
   echo ""
   echo -e "${BOLD}╔═══════════════════════════════════════════════╗${NC}"
   echo -e "${BOLD}║         D.A.W.N. Uninstall                    ║${NC}"
   echo -e "${BOLD}╚═══════════════════════════════════════════════╝${NC}"
   echo ""

   warn "This will remove DAWN components installed by this script."
   warn "Your project source tree and configuration files (dawn.toml,"
   warn "secrets.toml) in the project directory will NOT be touched."
   echo ""

   if ! ask_yes_no "Continue with uninstall?"; then
      log "Uninstall cancelled."
      exit 0
   fi

   echo ""
   detect_sudo

   header "Step 0: Running Processes"
   stop_running_processes

   header "Step 1: Systemd Services"
   uninstall_services

   header "Step 2: Binaries"
   uninstall_binaries

   header "Step 3: Whisper/GGML Libraries"
   uninstall_cmake_libs

   header "Step 4: DAWN Libraries (piper-phonemize, ONNX Runtime)"
   uninstall_dawn_libs

   header "Step 5: espeak-ng (rhasspy fork)"
   uninstall_espeak_ng

   header "Step 6: CA Certificate"
   uninstall_ca_cert

   header "Step 7: Headless Audio"
   uninstall_headless_audio

   header "Step 8: ldconfig"
   uninstall_ldconfig

   header "Step 9: Docker Containers"
   uninstall_docker_containers

   header "Step 10: Installer State"
   uninstall_state

   header "Step 11: User Data"
   uninstall_user_data

   header "Step 12: System User"
   uninstall_system_user

   # Summary
   echo ""
   echo -e "${BOLD}═══════════════════════════════════════════════${NC}"
   echo ""
   log "Uninstall complete: $_REMOVED removed, $_SKIPPED skipped"
   echo ""

   if [ $_SKIPPED -gt 0 ]; then
      info "Some components were kept. Re-run to remove them."
   fi

   echo "Not removed (by design):"
   echo "  - apt packages (shared system dependencies)"
   if has_command docker; then
      echo "  - Docker Engine (shared system service)"
   fi
   echo "  - Project source: $PROJECT_ROOT"
   echo "  - Build directories: $PROJECT_ROOT/build-*/"
   echo "  - Project config: $PROJECT_ROOT/dawn.toml, secrets.toml"
   echo "  - Project SSL certs: $PROJECT_ROOT/ssl/"
   echo "  - Downloaded models: $PROJECT_ROOT/models/"
   echo ""
   info "To fully remove DAWN, delete the project directory:"
   info "  rm -rf $PROJECT_ROOT"
}
