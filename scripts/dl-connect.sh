#!/usr/bin/env bash
# Bridge stdin/stdout to the DebugLantern gdbserver via TCP.
# Used as a GDB pipe target:  target remote | dl-connect.sh
#
# Reads the target address (host:port) from .debuglantern_target,
# which is written by debuglantern_deploy.sh after each deploy.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TARGET_FILE="$SCRIPT_DIR/../.debuglantern_target"

if [[ ! -f "$TARGET_FILE" ]]; then
  echo "Error: $TARGET_FILE not found. Deploy first." >&2
  exit 1
fi

TARGET=$(cat "$TARGET_FILE")
HOST="${TARGET%%:*}"
PORT="${TARGET##*:}"

exec nc "$HOST" "$PORT"
