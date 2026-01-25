#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SCRIPT_DIR="$ROOT_DIR/scripts"

echo "This wrapper now delegates to the arm64 flutter-pi build script." >&2
exec "$SCRIPT_DIR/build_flutter_pi_release_arm64.sh" "$@"
