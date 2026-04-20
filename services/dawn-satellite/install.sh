#!/bin/bash
#
# DAWN Satellite Installation Script
# Installs dawn_satellite as a systemd service on Raspberry Pi
#

set -e

# Default configuration
BINARY_PATH=""
MODELS_DIR=""
FONTS_DIR=""
CONFIG_SRC=""
SYMLINK_MODELS=false
NO_DISPLAY=false
UNINSTALL=false
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
SERVICE_USER="dawn"
SERVICE_NAME="dawn-satellite"
DATA_DIR="/var/lib/dawn-satellite"
CONFIG_DIR="/usr/local/etc/dawn-satellite"
LOG_DIR="/var/log/dawn-satellite"

# Colors for output
GREEN='\033[0;32m'
YELLOW='\033[0;33m'
RED='\033[0;31m'
NC='\033[0m'

# Helper functions
log() {
    echo -e "${GREEN}[INFO]${NC} $1"
}

warn() {
    echo -e "${YELLOW}[WARNING]${NC} $1"
}

error() {
    echo -e "${RED}[ERROR]${NC} $1"
    exit 1
}

ask_yes_no() {
    local answer
    read -rp "$1 [y/N] " answer
    [[ "$answer" =~ ^[Yy] ]]
}

uninstall() {
    log "Uninstalling dawn-satellite..."

    # 1. Stop and disable service
    if systemctl is-active --quiet "$SERVICE_NAME.service" 2>/dev/null; then
        log "Stopping $SERVICE_NAME service"
        systemctl stop "$SERVICE_NAME.service"
    fi
    if systemctl is-enabled --quiet "$SERVICE_NAME.service" 2>/dev/null; then
        log "Disabling $SERVICE_NAME service"
        systemctl disable "$SERVICE_NAME.service"
    fi

    # 2. Remove systemd unit file
    if [ -f "/etc/systemd/system/$SERVICE_NAME.service" ]; then
        log "Removing systemd service file"
        rm -f "/etc/systemd/system/$SERVICE_NAME.service"
        systemctl daemon-reload
    fi

    # 3. Remove logrotate config
    if [ -f "/etc/logrotate.d/dawn-satellite" ]; then
        log "Removing logrotate configuration"
        rm -f "/etc/logrotate.d/dawn-satellite"
    fi

    # 4. Remove binary
    if [ -f "/usr/local/bin/dawn_satellite" ]; then
        log "Removing /usr/local/bin/dawn_satellite"
        rm -f "/usr/local/bin/dawn_satellite"
    fi

    # 5. Remove data directory (models, fonts, identity)
    if [ -d "$DATA_DIR" ]; then
        log "Removing data directory: $DATA_DIR"
        rm -rf "$DATA_DIR"
    fi

    # 6. Remove log directory
    if [ -d "$LOG_DIR" ]; then
        log "Removing log directory: $LOG_DIR"
        rm -rf "$LOG_DIR"
    fi

    # 7. Prompt for configuration removal
    if [ -d "$CONFIG_DIR" ]; then
        if ask_yes_no "Remove configuration files? ($CONFIG_DIR/)"; then
            log "Removing configuration directory: $CONFIG_DIR"
            rm -rf "$CONFIG_DIR"
        else
            log "Keeping configuration: $CONFIG_DIR"
        fi
    fi

    # 8. Remove whisper/ggml libs and ld.so.conf.d entry — only if the
    #    server is not installed (it may share these libraries).
    if [ ! -f "/etc/systemd/system/dawn-server.service" ]; then
        local removed=0
        for lib in /usr/local/lib/libwhisper.so* /usr/local/lib/libggml*.so*; do
            [ -e "$lib" ] || continue
            rm -f "$lib"
            removed=$((removed + 1))
        done
        if [ "$removed" -gt 0 ]; then
            log "Removed $removed whisper/ggml library file(s) from /usr/local/lib"
        fi

        if [ -f "/etc/ld.so.conf.d/dawn.conf" ]; then
            log "Removing /etc/ld.so.conf.d/dawn.conf"
            rm -f "/etc/ld.so.conf.d/dawn.conf"
        fi
        ldconfig
    else
        log "Keeping /etc/ld.so.conf.d/dawn.conf and whisper/ggml libs (dawn-server is still installed)"
    fi

    log ""
    log "dawn-satellite has been uninstalled."
    log "Note: dawn system user was not removed (may be shared)."
    log "  Remove manually if desired: userdel dawn"
}

# Parse command line arguments
parse_args() {
    while [[ $# -gt 0 ]]; do
        case $1 in
            --binary)
                BINARY_PATH="$2"
                shift 2
                ;;
            --models-dir)
                MODELS_DIR="$2"
                shift 2
                ;;
            --fonts-dir)
                FONTS_DIR="$2"
                shift 2
                ;;
            --config)
                CONFIG_SRC="$2"
                shift 2
                ;;
            --symlink-models)
                SYMLINK_MODELS=true
                shift
                ;;
            --no-display)
                NO_DISPLAY=true
                shift
                ;;
            -u|--uninstall)
                UNINSTALL=true
                shift
                ;;
            -h|--help)
                echo "Usage: $0 [options]"
                echo ""
                echo "Options:"
                echo "  --binary PATH        Path to dawn_satellite binary"
                echo "                       (default: searches build dir and project root)"
                echo "  --models-dir PATH    Path to models directory"
                echo "                       (default: dawn_satellite/models/)"
                echo "  --fonts-dir PATH     Path to fonts directory"
                echo "                       (default: dawn_satellite/assets/fonts/)"
                echo "  --config PATH        Path to a pre-configured satellite.toml to install"
                echo "                       (default: the template at $SCRIPT_DIR/satellite.toml)"
                echo "  --symlink-models     Symlink models instead of copying (saves disk)"
                echo "  --no-display         Skip video/render/input groups (headless satellite)"
                echo "  -u, --uninstall      Remove installed dawn-satellite components"
                echo "  -h, --help           Show this help message"
                echo ""
                echo "Installed paths:"
                echo "  Binary:  /usr/local/bin/dawn_satellite"
                echo "  Config:  $CONFIG_DIR/satellite.toml"
                echo "  Data:    $DATA_DIR/"
                echo "  Logs:    $LOG_DIR/"
                exit 0
                ;;
            *)
                error "Unknown option: $1"
                ;;
        esac
    done
}

# Find the satellite binary
find_binary() {
    if [ -n "$BINARY_PATH" ]; then
        if [ ! -f "$BINARY_PATH" ]; then
            error "Binary not found: $BINARY_PATH"
        fi
        return
    fi

    # Search common build locations
    local search_paths=(
        "$PROJECT_ROOT/dawn_satellite/build/dawn_satellite"
        "$PROJECT_ROOT/build-debug/dawn_satellite/dawn_satellite"
        "$PROJECT_ROOT/build-release/dawn_satellite/dawn_satellite"
    )

    for path in "${search_paths[@]}"; do
        if [ -f "$path" ]; then
            BINARY_PATH="$path"
            log "Found binary: $BINARY_PATH"
            return
        fi
    done

    error "dawn_satellite binary not found. Build it first or use --binary PATH.

Build instructions:
  cd dawn_satellite && mkdir -p build && cd build
  cmake .. -DENABLE_VOICE=ON -DENABLE_VOSK_ASR=ON
  make -j\$(nproc)"
}

# Find models directory
find_models() {
    if [ -n "$MODELS_DIR" ]; then
        if [ ! -d "$MODELS_DIR" ]; then
            error "Models directory not found: $MODELS_DIR"
        fi
        return
    fi

    local search_paths=(
        "$PROJECT_ROOT/dawn_satellite/models"
        "$PROJECT_ROOT/models"
    )

    for path in "${search_paths[@]}"; do
        if [ -d "$path" ]; then
            MODELS_DIR="$path"
            log "Found models: $MODELS_DIR"
            return
        fi
    done

    warn "No models directory found. You will need to copy models manually to $DATA_DIR/models/"
}

# Find fonts directory
find_fonts() {
    if [ -n "$FONTS_DIR" ]; then
        if [ ! -d "$FONTS_DIR" ]; then
            error "Fonts directory not found: $FONTS_DIR"
        fi
        return
    fi

    local search_paths=(
        "$PROJECT_ROOT/dawn_satellite/assets/fonts"
        "$PROJECT_ROOT/assets/fonts"
    )

    for path in "${search_paths[@]}"; do
        if [ -d "$path" ]; then
            FONTS_DIR="$path"
            log "Found fonts: $FONTS_DIR"
            return
        fi
    done

    if [ "$NO_DISPLAY" = false ]; then
        warn "No fonts directory found. SDL UI will not have fonts until you copy them to $DATA_DIR/assets/fonts/"
    fi
}

# ============================================================================
# Main
# ============================================================================

# Check if running as root
if [ "$(id -u)" -ne 0 ]; then
    error "This script must be run as root"
fi

# Parse command line arguments
parse_args "$@"

# Handle uninstall
if [ "$UNINSTALL" = true ]; then
    uninstall
    exit 0
fi

# Find files
find_binary
find_models
find_fonts

# Create service user if it doesn't exist
if ! id -u "$SERVICE_USER" &>/dev/null; then
    log "Creating service user: $SERVICE_USER"
    useradd --system --home-dir "$DATA_DIR" --no-create-home --shell /usr/sbin/nologin "$SERVICE_USER"
else
    log "Service user $SERVICE_USER already exists"
fi

# Add user to hardware groups
log "Configuring group membership"
for group in audio; do
    if getent group "$group" >/dev/null; then
        if ! groups "$SERVICE_USER" 2>/dev/null | grep -q "\b$group\b"; then
            usermod -a -G "$group" "$SERVICE_USER"
            log "Added $SERVICE_USER to $group group"
        fi
    fi
done

if [ "$NO_DISPLAY" = false ]; then
    for group in video render input; do
        if getent group "$group" >/dev/null; then
            if ! groups "$SERVICE_USER" 2>/dev/null | grep -q "\b$group\b"; then
                usermod -a -G "$group" "$SERVICE_USER"
                log "Added $SERVICE_USER to $group group"
            fi
        fi
    done
fi

# Create directory structure
log "Creating directory structure"
mkdir -p "$DATA_DIR/models"
mkdir -p "$DATA_DIR/assets/fonts"
mkdir -p "$CONFIG_DIR"
mkdir -p "$LOG_DIR"

# Stop the running service before touching binaries / libraries. Linux
# refuses `cp` over a running executable (Text file busy), and stopping
# here also means we don't have to worry about the service reloading a
# half-installed library set between steps.
if systemctl is-active --quiet "$SERVICE_NAME.service" 2>/dev/null; then
    log "Stopping running $SERVICE_NAME service before replacing files"
    systemctl stop "$SERVICE_NAME.service"
fi

# Install binary
log "Installing binary to /usr/local/bin/dawn_satellite"
cp "$BINARY_PATH" /usr/local/bin/dawn_satellite
chmod 755 /usr/local/bin/dawn_satellite

# Install whisper.cpp / ggml shared libraries.
#
# These are built inside the satellite build tree (dawn_satellite/build/
# contains whisper.cpp/src/libwhisper.so* and ggml/src/libggml*.so*) and
# the binary links against them dynamically. Without this step the service
# fails on startup with:
#   "libwhisper.so.1: cannot open shared object file"
# even though the binary itself is installed.
BUILD_DIR="$(dirname "$BINARY_PATH")"
log "Installing whisper/ggml shared libraries from $BUILD_DIR"
installed_libs=0
while IFS= read -r -d '' lib; do
    cp -a "$lib" /usr/local/lib/
    installed_libs=$((installed_libs + 1))
done < <(find "$BUILD_DIR" \
    \( -name 'libwhisper.so*' -o -name 'libggml*.so*' \) \
    -print0 2>/dev/null)
if [ "$installed_libs" -eq 0 ]; then
    warn "No whisper/ggml shared libraries found under $BUILD_DIR"
    warn "If this build uses Whisper ASR, the service will fail to start."
else
    log "Installed $installed_libs whisper/ggml library file(s)"
fi

# Install models
if [ -n "$MODELS_DIR" ]; then
    if [ "$SYMLINK_MODELS" = true ]; then
        log "Symlinking models from $MODELS_DIR"
        # Remove existing models dir if it's not a symlink
        if [ -d "$DATA_DIR/models" ] && [ ! -L "$DATA_DIR/models" ]; then
            rmdir "$DATA_DIR/models" 2>/dev/null || true
        fi
        ln -sfn "$MODELS_DIR" "$DATA_DIR/models"
    else
        log "Copying models from $MODELS_DIR"
        # cp -rL (dereference symlinks): $PROJECT_ROOT/models/whisper.cpp is
        # a symlink to ../whisper.cpp/models. A plain `cp -r` would copy the
        # symlink as-is, leaving a broken link under /var/lib/dawn-satellite/
        # (it would point at ../whisper.cpp/models relative to DATA_DIR,
        # which doesn't exist there). -L materializes the actual model
        # files into the destination.
        cp -rL "$MODELS_DIR"/. "$DATA_DIR/models/"
    fi
fi

# Install fonts
if [ -n "$FONTS_DIR" ]; then
    log "Copying fonts from $FONTS_DIR"
    cp -r "$FONTS_DIR"/. "$DATA_DIR/assets/fonts/"
fi

# Resolve config source: prefer --config, fall back to the shipped template.
if [ -n "$CONFIG_SRC" ]; then
    if [ ! -f "$CONFIG_SRC" ]; then
        error "Config source not found: $CONFIG_SRC"
    fi
    log "Using custom config: $CONFIG_SRC"
else
    CONFIG_SRC="$SCRIPT_DIR/satellite.toml"
fi

# Install configuration (never overwrite existing — preserve user edits)
if [ -f "$CONFIG_DIR/satellite.toml" ]; then
    warn "Config file already exists: $CONFIG_DIR/satellite.toml (not overwriting)"
    warn "New version saved to: $CONFIG_DIR/satellite.toml.new"
    cp "$CONFIG_SRC" "$CONFIG_DIR/satellite.toml.new"
else
    log "Installing satellite.toml from $CONFIG_SRC"
    cp "$CONFIG_SRC" "$CONFIG_DIR/satellite.toml"
fi
# Config must be writable by dawn user (satellite saves UI prefs back)
chmod 664 "$CONFIG_DIR/satellite.toml"
chown "$SERVICE_USER:$SERVICE_USER" "$CONFIG_DIR/satellite.toml"

# Install environment file
log "Installing environment configuration"
cp "$SCRIPT_DIR/dawn-satellite.conf" "$CONFIG_DIR/"
chmod 644 "$CONFIG_DIR/dawn-satellite.conf"

# Set permissions
log "Setting permissions"
chown -R "$SERVICE_USER:$SERVICE_USER" "$DATA_DIR"
chmod -R 755 "$DATA_DIR"
chown "$SERVICE_USER:$SERVICE_USER" "$LOG_DIR"
chmod 755 "$LOG_DIR"
chown -R root:root "$CONFIG_DIR"
chown "$SERVICE_USER:$SERVICE_USER" "$CONFIG_DIR/satellite.toml"

# Ensure library path is configured
log "Configuring library path"
if [ ! -f /etc/ld.so.conf.d/dawn.conf ]; then
    echo "/usr/local/lib" > /etc/ld.so.conf.d/dawn.conf
    log "Added /usr/local/lib to library path"
fi
# Always rebuild the linker cache — we may have just installed new
# libwhisper.so* / libggml*.so* files above.
ldconfig

# Install systemd service
log "Installing systemd service"
cp "$SCRIPT_DIR/dawn-satellite.service" /etc/systemd/system/

# Install logrotate configuration
log "Installing logrotate configuration"
cp "$SCRIPT_DIR/dawn-satellite-logrotate" /etc/logrotate.d/dawn-satellite
chmod 644 /etc/logrotate.d/dawn-satellite

# Enable and start service
log "Enabling and starting service"
systemctl daemon-reload
systemctl enable "$SERVICE_NAME.service"
systemctl restart "$SERVICE_NAME.service"

# Check service status
sleep 2
if systemctl is-active --quiet "$SERVICE_NAME.service"; then
    log "Service successfully started"
    log ""
    log "Management commands:"
    log "  Status:  systemctl status $SERVICE_NAME"
    log "  Logs:    tail -f $LOG_DIR/satellite.log"
    log "  Restart: systemctl restart $SERVICE_NAME"
    log "  Stop:    systemctl stop $SERVICE_NAME"
    log ""
    log "Next steps:"
    log "  1. Edit $CONFIG_DIR/satellite.toml"
    log "     - Set [server] host to your DAWN daemon IP"
    log "     - Set [identity] name and location"
    log "     - Configure [audio] capture/playback devices"
    log "  2. Restart: sudo systemctl restart $SERVICE_NAME"
else
    warn "Service failed to start. Check logs:"
    warn "  journalctl -u $SERVICE_NAME -n 50"
    warn "  cat $LOG_DIR/satellite.log"
    exit 1
fi
