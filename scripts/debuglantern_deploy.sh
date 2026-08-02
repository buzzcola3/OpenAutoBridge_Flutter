#!/usr/bin/env bash
# Deploy a debug-symbols flutter-drm bundle to a Pi via DebugLantern.
#
# Usage:
#   debuglantern_deploy.sh [--arch arm64|amd64] [--target HOST] [--port PORT]
#                          [--rotation 0|90|180|270] [--no-debug] [--session ID]
#
# Outputs the DebugLantern session ID and gdbserver port on success.
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

# --- defaults ----------------------------------------------------------------
ARCH="arm64"
# Discover the device by mDNS rather than a hard-coded address, which goes stale
# every time DHCP reassigns. Matches OpenAutoCore/.vscode/deploy-debug.sh.
TARGET_HOST="${DL_TARGET:-orangepizero3.local}"
DL_PORT="${DL_PORT:-4444}"
CTL="${DEBUGLANTERNCTL:-$(command -v debuglanternctl 2>/dev/null || echo "$HOME/Downloads/debuglantern-linux-amd64-musl/debuglanternctl")}"
SESSION_ID=""
START_DEBUG=true
# Startup rotation passed to flutter-drm-embedder (-r/--rotation), clockwise
# degrees. The target panel is a 320x960 portrait display, so 270 is the default.
ROTATION="${DL_ROTATION:-270}"

# --- parse args ---------------------------------------------------------------
while [[ $# -gt 0 ]]; do
  case "$1" in
    --arch)   ARCH="$2"; shift 2 ;;
    --target) TARGET_HOST="$2"; shift 2 ;;
    --port)   DL_PORT="$2"; shift 2 ;;
    --no-debug) START_DEBUG=false; shift ;;
    --session) SESSION_ID="$2"; shift 2 ;;
    --rotation) ROTATION="$2"; shift 2 ;;
    *) echo "Unknown arg: $1" >&2; exit 1 ;;
  esac
done

case "$ROTATION" in
  0|90|180|270) ;;
  *) echo "Invalid --rotation '$ROTATION' (expected 0, 90, 180 or 270)" >&2; exit 1 ;;
esac

# --- resolve target -----------------------------------------------------------
# debuglanternctl is statically linked and cannot use NSS/mDNS, so .local names
# must be resolved to an address here before being handed to it.
if [[ "$TARGET_HOST" == *.local ]]; then
  TARGET=$(getent hosts "$TARGET_HOST" | awk '{print $1; exit}')
  if [[ -z "$TARGET" ]]; then
    echo "Could not resolve $TARGET_HOST via mDNS." >&2
    echo "Pass an address explicitly: --target 192.168.1.x" >&2
    exit 1
  fi
  echo "==> Resolved $TARGET_HOST -> $TARGET"
else
  TARGET="$TARGET_HOST"
fi

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

# --- clean up previous sessions -----------------------------------------------
# Sessions are not reclaimed automatically, so without this they accumulate on
# the device and each stale one keeps holding a gdbserver port.
echo "==> Cleaning previous flutter-drm-embedder sessions..."
if [[ -n "$SESSION_ID" ]]; then
  PREV_IDS="$SESSION_ID"
else
  PREV_IDS=$("$CTL" list --target "$TARGET" --port "$DL_PORT" 2>/dev/null \
    | jq -r '.[] | select(.exec_path == "flutter-drm-embedder") | .id' || true)
fi
for id in $PREV_IDS; do
  echo "    stopping/deleting $id"
  "$CTL" kill   "$id" --target "$TARGET" --port "$DL_PORT" >/dev/null 2>&1 || true
  sleep 0.3
  "$CTL" delete "$id" --target "$TARGET" --port "$DL_PORT" >/dev/null 2>&1 || true
done

# --- upload -------------------------------------------------------------------
echo "==> Uploading to DebugLantern at $TARGET:$DL_PORT..."
UPLOAD_RESP=$("$CTL" upload "$TARBALL" --exec-path flutter-drm-embedder --target "$TARGET" --port "$DL_PORT")
echo "    $UPLOAD_RESP"
SESSION_ID=$(echo "$UPLOAD_RESP" | jq -r '.id')
if [[ -z "$SESSION_ID" || "$SESSION_ID" == "null" ]]; then
  echo "Upload failed" >&2
  exit 1
fi
echo "==> Session ID: $SESSION_ID"

# --- configure env & args -----------------------------------------------------
echo "==> Configuring session..."
"$CTL" env "$SESSION_ID" "LD_LIBRARY_PATH=.:./plugins" "GST_PLUGIN_PATH=/usr/lib/gstreamer-1.0" --target "$TARGET" --port "$DL_PORT"
# flutter-drm-embedder wants its flags before the bundle path, and the bundle is
# extracted so that "." is the app directory.
EMBEDDER_ARGS="."
if [[ "$ROTATION" != "0" ]]; then
  EMBEDDER_ARGS="--rotation $ROTATION ."
fi
echo "    args: $EMBEDDER_ARGS"
"$CTL" args "$SESSION_ID" "$EMBEDDER_ARGS" --target "$TARGET" --port "$DL_PORT"

# --- start --------------------------------------------------------------------
if $START_DEBUG; then
  echo "==> Starting under gdbserver..."
  START_RESP=$("$CTL" start "$SESSION_ID" --debug --target "$TARGET" --port "$DL_PORT")
  echo "    $START_RESP"
  DEBUG_PORT=$(echo "$START_RESP" | jq -r '.debug_port')
  if [[ -z "$DEBUG_PORT" || "$DEBUG_PORT" == "null" ]]; then
    echo "Failed to get debug port" >&2
    exit 1
  fi

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
