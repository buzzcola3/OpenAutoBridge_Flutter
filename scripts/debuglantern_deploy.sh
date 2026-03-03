#!/usr/bin/env bash
# Deploy a debug-symbols flutter-drm bundle to a Pi via DebugLantern.
#
# Usage:
#   debuglantern_deploy.sh [--arch arm64|amd64] [--target HOST] [--port PORT]
#
# Outputs the DebugLantern session ID and gdbserver port on success.
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

# --- defaults ----------------------------------------------------------------
ARCH="arm64"
TARGET="${DL_TARGET:-192.168.1.28}"
DL_PORT="${DL_PORT:-4444}"
CTL="${DEBUGLANTERNCTL:-$(command -v debuglanternctl 2>/dev/null || echo "$HOME/Downloads/debuglantern-linux-amd64-musl/debuglanternctl")}"
SESSION_ID=""
START_DEBUG=true

# --- parse args ---------------------------------------------------------------
while [[ $# -gt 0 ]]; do
  case "$1" in
    --arch)   ARCH="$2"; shift 2 ;;
    --target) TARGET="$2"; shift 2 ;;
    --port)   DL_PORT="$2"; shift 2 ;;
    --no-debug) START_DEBUG=false; shift ;;
    --session) SESSION_ID="$2"; shift 2 ;;
    *) echo "Unknown arg: $1" >&2; exit 1 ;;
  esac
done

DIST_DIR="$ROOT_DIR/dist/flutter-drm-${ARCH}-debug"
TARBALL="$ROOT_DIR/dist/flutter-drm-${ARCH}-debug.tar.gz"

if [[ ! -d "$DIST_DIR" ]]; then
  echo "Debug bundle not found at $DIST_DIR" >&2
  echo "Run the build script first: bash scripts/build_flutter_drm_release_${ARCH}.sh example" >&2
  exit 1
fi

if [[ ! -x "$CTL" ]]; then
  echo "debuglanternctl not found. Set DEBUGLANTERNCTL env or put it on PATH." >&2
  exit 1
fi

# --- package ------------------------------------------------------------------
echo "==> Packaging debug bundle..."
tar -C "$DIST_DIR" -czf "$TARBALL" .
echo "    $TARBALL ($(du -h "$TARBALL" | cut -f1))"

# --- upload / reuse session ---------------------------------------------------
if [[ -z "$SESSION_ID" ]]; then
  echo "==> Uploading to DebugLantern at $TARGET:$DL_PORT..."
  UPLOAD_RESP=$("$CTL" upload "$TARBALL" --exec-path flutter-drm-embedder --target "$TARGET" --port "$DL_PORT")
  echo "    $UPLOAD_RESP"
  SESSION_ID=$(echo "$UPLOAD_RESP" | grep -oP '"id"\s*:\s*"\K[^"]+')
  echo "==> Session ID: $SESSION_ID"
else
  echo "==> Reusing session $SESSION_ID — re-uploading..."
  # Stop any running instance first
  "$CTL" kill "$SESSION_ID" --target "$TARGET" --port "$DL_PORT" 2>/dev/null || true
  "$CTL" delete "$SESSION_ID" --target "$TARGET" --port "$DL_PORT" 2>/dev/null || true
  UPLOAD_RESP=$("$CTL" upload "$TARBALL" --exec-path flutter-drm-embedder --target "$TARGET" --port "$DL_PORT")
  echo "    $UPLOAD_RESP"
  SESSION_ID=$(echo "$UPLOAD_RESP" | grep -oP '"id"\s*:\s*"\K[^"]+')
  echo "==> New session ID: $SESSION_ID"
fi

# --- configure env & args -----------------------------------------------------
echo "==> Configuring session..."
"$CTL" env "$SESSION_ID" "LD_LIBRARY_PATH=.:./plugins" "GST_PLUGIN_PATH=/usr/lib/gstreamer-1.0" --target "$TARGET" --port "$DL_PORT"
"$CTL" args "$SESSION_ID" "." --target "$TARGET" --port "$DL_PORT"

# --- start --------------------------------------------------------------------
if $START_DEBUG; then
  echo "==> Starting under gdbserver..."
  START_RESP=$("$CTL" start "$SESSION_ID" --debug --target "$TARGET" --port "$DL_PORT")
  echo "    $START_RESP"
  DEBUG_PORT=$(echo "$START_RESP" | grep -oP '"debug_port"\s*:\s*\K[0-9]+')

  # Write target file for VS Code pipe-based auto-connect
  TARGET_FILE="$ROOT_DIR/.debuglantern_target"
  echo "$TARGET:$DEBUG_PORT" > "$TARGET_FILE"
  echo "    Wrote $TARGET_FILE"

  # Write session metadata for later queries
  SESSION_FILE="$ROOT_DIR/.debuglantern_session"
  cat > "$SESSION_FILE" <<EOF
SESSION_ID=$SESSION_ID
TARGET=$TARGET
DL_PORT=$DL_PORT
DEBUG_PORT=$DEBUG_PORT
EOF
  echo "    Wrote $SESSION_FILE"

  echo ""
  echo "============================================"
  echo "  Session : $SESSION_ID"
  echo "  GDB Port: $DEBUG_PORT"
  echo "  Connect : target remote $TARGET:$DEBUG_PORT"
  echo "============================================"
else
  echo "==> Starting (no debugger)..."
  "$CTL" start "$SESSION_ID" --target "$TARGET" --port "$DL_PORT"
fi
