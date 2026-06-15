#!/bin/bash
#
# Codebase Memory MCP (cbm-mcp) installer for DAWN.
#
# Builds the cbm binary (libgit2 disabled — see README.md), installs mcp-proxy
# into a dedicated venv, and registers cbm-mcp as a systemd service that
# re-exposes cbm over SSE for DAWN's MCP bridge to connect to.
#
# The service runs as the 'dawn' user so it can read DAWN's clone tree
# (/var/lib/dawn/source) and write its graph cache under /var/lib/dawn.
#
# Usage:
#   sudo ./install.sh                      # build + install + enable
#   sudo ./install.sh --cbm-src DIR        # cbm source checkout (default: ~/code/codebase-memory-mcp)
#   sudo ./install.sh --port N --host ADDR # override SSE bind (default 127.0.0.1:9750)
#
set -e

# ── Paths / defaults ─────────────────────────────────────────────
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SERVICE_USER="dawn"
SERVICE_GROUP="dawn"
if [ -n "${SUDO_USER:-}" ] && [ "$SUDO_USER" != "root" ]; then
   CBM_SRC="/home/$SUDO_USER/code/codebase-memory-mcp"
else
   CBM_SRC="$HOME/code/codebase-memory-mcp"
fi
CBM_BIN_DEST="/usr/local/bin/codebase-memory-mcp"
VENV_DIR="/usr/local/lib/cbm-mcp/venv"
CONFIG_DIR="/usr/local/etc/cbm-mcp"
CONFIG_FILE="$CONFIG_DIR/cbm-mcp.conf"
LOG_DIR="/var/log/dawn"
DATA_DIR="/var/lib/dawn"
CACHE_DIR="$DATA_DIR/cbm-cache"
HOST="127.0.0.1"
PORT="9750"
LOCAL_ROOTS=""              # space-separated; --local-roots or interactive prompt (link-local read access)
ASSUME_NO_ROOTS="false"     # --no-local-roots skips the prompt entirely (e.g. non-interactive installs)

# ── Colors / helpers ─────────────────────────────────────────────
RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; NC='\033[0m'
log()   { echo -e "${GREEN}[INFO]${NC} $1"; }
warn()  { echo -e "${YELLOW}[WARNING]${NC} $1"; }
error() { echo -e "${RED}[ERROR]${NC} $1"; exit 1; }

# Run a command as the invoking (non-root) user when possible — used for the
# in-tree cbm build so artifacts aren't left root-owned in the user's repo.
run_as_user() {
   if [ -n "${SUDO_USER:-}" ] && [ "$SUDO_USER" != "root" ]; then
      sudo -u "$SUDO_USER" "$@"
   else
      "$@"
   fi
}

# ── Parse args ───────────────────────────────────────────────────
while [ $# -gt 0 ]; do
   case "$1" in
      --cbm-src)     CBM_SRC="$2"; shift 2 ;;
      --port)        PORT="$2"; shift 2 ;;
      --host)        HOST="$2"; shift 2 ;;
      --local-roots) LOCAL_ROOTS="$2"; shift 2 ;;
      --no-local-roots) ASSUME_NO_ROOTS="true"; shift ;;
      -h|--help)
         echo "Usage: sudo ./install.sh [--cbm-src DIR] [--port N] [--host ADDR]"
         echo "                         [--local-roots \"/path/a /path/b\"] [--no-local-roots]"
         echo ""
         echo "  --local-roots     Grant cbm read access to these dirs for link-local repos"
         echo "                    (writes a systemd drop-in + traverse ACLs). Space-separated."
         echo "  --no-local-roots  Skip the link-local grant prompt (non-interactive installs)."
         exit 0 ;;
      *) error "Unknown argument: $1 (try --help)" ;;
   esac
done

# ── Preconditions ────────────────────────────────────────────────
[ "$(id -u)" -eq 0 ] || error "Run as root: sudo ./install.sh"
command -v python3 >/dev/null || error "python3 not found"
python3 -m venv --help >/dev/null 2>&1 || \
   error "python3 venv module missing — install it (e.g. sudo apt install python3-venv)"

# ── 1. Build cbm if not already built ───────────────────────────
if [ ! -x "$CBM_SRC/build/c/codebase-memory-mcp" ]; then
   [ -f "$CBM_SRC/scripts/build.sh" ] || \
      error "cbm source not found at $CBM_SRC — clone it or pass --cbm-src DIR"
   log "Building cbm (libgit2 disabled) in $CBM_SRC"
   run_as_user bash -c "cd '$CBM_SRC' && scripts/build.sh LIBGIT2_FLAGS= LIBGIT2_LIBS="
fi
[ -x "$CBM_SRC/build/c/codebase-memory-mcp" ] || error "cbm build produced no binary"

# ── 2. Install cbm binary ───────────────────────────────────────
log "Installing cbm binary -> $CBM_BIN_DEST"
install -m 755 "$CBM_SRC/build/c/codebase-memory-mcp" "$CBM_BIN_DEST"

# ── 3. mcp-proxy venv ───────────────────────────────────────────
log "Creating mcp-proxy venv -> $VENV_DIR"
mkdir -p "$(dirname "$VENV_DIR")"
python3 -m venv "$VENV_DIR"
"$VENV_DIR/bin/pip" install --quiet --upgrade pip
"$VENV_DIR/bin/pip" install --quiet mcp-proxy
[ -x "$VENV_DIR/bin/mcp-proxy" ] || error "mcp-proxy install failed"

# ── 4. Service user + directories ───────────────────────────────
if ! id -u "$SERVICE_USER" &>/dev/null; then
   log "Creating system user '$SERVICE_USER'"
   useradd --system --no-create-home --shell /usr/sbin/nologin "$SERVICE_USER"
fi
mkdir -p "$DATA_DIR" "$CACHE_DIR" "$LOG_DIR"
chown "$SERVICE_USER:$SERVICE_GROUP" "$DATA_DIR" "$CACHE_DIR" "$LOG_DIR"
chmod 755 "$DATA_DIR" "$CACHE_DIR" "$LOG_DIR"

# ── 5. Config file (preserve existing edits) ────────────────────
mkdir -p "$CONFIG_DIR"
if [ -f "$CONFIG_FILE" ]; then
   warn "Config exists, leaving as-is: $CONFIG_FILE"
else
   log "Installing config -> $CONFIG_FILE"
   sed -e "s|^HOST=.*|HOST=$HOST|" -e "s|^PORT=.*|PORT=$PORT|" \
      "$SCRIPT_DIR/cbm-mcp.conf" > "$CONFIG_FILE"
   chmod 644 "$CONFIG_FILE"
fi

# ── 6. systemd unit + logrotate ─────────────────────────────────
log "Installing systemd unit + logrotate"
cp "$SCRIPT_DIR/cbm-mcp.service" /etc/systemd/system/
cp "$SCRIPT_DIR/cbm-mcp-logrotate" /etc/logrotate.d/cbm-mcp
chmod 644 /etc/logrotate.d/cbm-mcp

# ── 6.5. Link-local read access (optional) ──────────────────────
# cbm runs sandboxed (ProtectHome=true hides all of /home). To index repos that
# live under a developer home (DAWN's link-local feature, or a coding assistant
# sharing this cbm), the service needs ProtectHome=tmpfs + a read-only bind per
# allowed root, plus traverse (--x) ACLs so the 'dawn' user can reach them. This
# writes a drop-in (survives reinstalls) rather than editing the shipped unit.
# Keep the chosen roots in sync with [code_projects].allowed_local_roots in dawn.toml.
DROPIN_DIR="/etc/systemd/system/cbm-mcp.service.d"
DROPIN_FILE="$DROPIN_DIR/10-local-roots.conf"

if [ -z "$LOCAL_ROOTS" ] && [ "$ASSUME_NO_ROOTS" != "true" ] && [ -t 0 ]; then
   echo ""
   echo "Link-local repos let DAWN/Claude Code index code outside /var/lib/dawn/source."
   echo "Their file CONTENTS reach the LLM, so only grant secret-free trees."
   read -r -p "Local code root(s) to grant cbm read access (space-separated, blank to skip): " LOCAL_ROOTS
fi

if [ -n "$LOCAL_ROOTS" ]; then
   command -v setfacl >/dev/null || error "setfacl not found — install 'acl' (sudo apt install acl) or re-run with --no-local-roots"
   # Validate every root before writing anything (fail closed).
   declare -a VALID_ROOTS=()
   for raw in $LOCAL_ROOTS; do
      root="$(readlink -f -- "$raw" 2>/dev/null || true)"
      [ -n "$root" ] && [ -d "$root" ] || error "Local root not an existing directory: $raw"
      case "$root" in
         *[$'\n\t']*)
            error "Refusing local root containing a newline/tab (drop-in injection guard)" ;;
         /dev|/dev/*|/proc|/proc/*|/sys|/sys/*|/run|/run/*)
            error "Refusing pseudo-filesystem root: $root" ;;
         /|/home|/root|/etc|/usr|/var|/bin|/sbin|/lib*|/boot|/opt|/srv|/mnt|/media|/tmp)
            error "Refusing unsafe local root: $root (bind a specific project dir under it instead)" ;;
      esac
      VALID_ROOTS+=("$root")
   done

   log "Writing link-local bind drop-in -> $DROPIN_FILE"
   mkdir -p "$DROPIN_DIR"
   {
      echo "# Generated by cbm-mcp/install.sh — link-local read access."
      echo "# Keep in sync with [code_projects].allowed_local_roots in dawn.toml."
      echo "[Service]"
      echo "ProtectHome=tmpfs"
      for root in "${VALID_ROOTS[@]}"; do
         echo "BindReadOnlyPaths=$root:$root"
      done
   } > "$DROPIN_FILE"
   chmod 644 "$DROPIN_FILE"

   # Grant traverse (--x) on each ancestor so 'dawn' can reach the bound root.
   # --x is traverse-only: it cannot list or read the intermediate dirs.
   for root in "${VALID_ROOTS[@]}"; do
      anc="$(dirname "$root")"
      while [ "$anc" != "/" ] && [ -n "$anc" ]; do
         setfacl -m u:"$SERVICE_USER":--x "$anc" 2>/dev/null || \
            warn "Could not set traverse ACL on $anc — cbm may not reach $root"
         anc="$(dirname "$anc")"
      done
      log "Granted cbm read access to $root"
   done
   warn "Re-running with fewer roots does NOT revoke prior traverse ACLs — remove stale"
   warn "  ones manually: setfacl -x u:$SERVICE_USER <dir>"
   warn "Add these to dawn.toml so DAWN allows linking under them:"
   warn "  [code_projects]"
   warn "  allowed_local_roots = [$(printf '"%s", ' "${VALID_ROOTS[@]}" | sed 's/, $//')]"
fi

# ── 7. Enable + start ───────────────────────────────────────────
log "Enabling and starting cbm-mcp"
systemctl daemon-reload
systemctl enable cbm-mcp.service
systemctl restart cbm-mcp.service

sleep 2
echo ""
if systemctl is-active --quiet cbm-mcp.service; then
   echo -e "${GREEN}cbm-mcp is running on http://$HOST:$PORT/sse${NC}"
   echo ""
   log "Add this to dawn.toml so DAWN connects to it:"
   cat <<EOF

    [mcp]
    enabled = true
    dev_mode = true            # required for tls_verify = false on plain-http localhost

    [[mcp.server]]
    alias = "cbm"              # MUST be exactly "cbm" — the code-graph provider keys on it
    url = "http://$HOST:$PORT/sse"
    transport = "http+sse"
    enabled = true
    capabilities = "dangerous"
    tls_verify = false

EOF
   log "Then in dawn.toml also set: [code_projects] enabled = true"
   echo ""
   log "Management commands:"
   log "  Status:   systemctl status cbm-mcp"
   log "  Logs:     journalctl -u cbm-mcp -f   (or /var/log/dawn/cbm-mcp.log)"
   log "  Restart:  systemctl restart cbm-mcp"
   log "  Stop:     systemctl stop cbm-mcp"
   echo ""
   log "Verify from DAWN:  dawn-admin mcp status   (expect: cbm: connected)"
else
   warn "Service failed to start. Check logs:"
   warn "  journalctl -u cbm-mcp -f"
   exit 1
fi
