#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
APP_DIR="${1:-$ROOT_DIR/example}"
OUT_DIR="$ROOT_DIR/dist/flutter-pi-amd64"

if [[ ! -d "$APP_DIR" ]]; then
  echo "App directory not found: $APP_DIR" >&2
  exit 1
fi

FLUTTERPI_CMD=()
FLUTTERPI_TOOL_GIT_URL="${FLUTTERPI_TOOL_GIT_URL:-https://github.com/buzzcola3/FlutterPi-plugin-bridge-tool}"
FLUTTERPI_TOOL_GIT_REF="${FLUTTERPI_TOOL_GIT_REF:-main}"
FLUTTERPI_TOOL_PATH="${FLUTTERPI_TOOL_PATH:-}"

resolve_flutterpi_tool() {
  if [[ -n "$FLUTTERPI_TOOL_PATH" && -x "$FLUTTERPI_TOOL_PATH" ]]; then
    FLUTTERPI_CMD=("$FLUTTERPI_TOOL_PATH")
    return
  fi

  local candidate_dirs=(
    "$ROOT_DIR/FlutterPi-plugin-bridge-tool"
    "$ROOT_DIR/../FlutterPi-plugin-bridge-tool"
  )

  for dir in "${candidate_dirs[@]}"; do
    if [[ -x "$dir/build/flutterpi_tool" ]]; then
      FLUTTERPI_CMD=("$dir/build/flutterpi_tool")
      return
    fi
  done

  local tool_dir="$ROOT_DIR/.cache/flutterpi-tool"
  if [[ ! -d "$tool_dir/.git" ]]; then
    rm -rf "$tool_dir"
    git clone --depth 1 --branch "$FLUTTERPI_TOOL_GIT_REF" "$FLUTTERPI_TOOL_GIT_URL" "$tool_dir"
  fi

  pushd "$tool_dir" >/dev/null
  flutter pub get
  mkdir -p build
  dart compile exe bin/flutterpi_tool.dart -o build/flutterpi_tool
  popd >/dev/null

  if [[ -x "$tool_dir/build/flutterpi_tool" ]]; then
    FLUTTERPI_CMD=("$tool_dir/build/flutterpi_tool")
    return
  fi

  echo "Failed to build FlutterPi-plugin-bridge-tool." >&2
  exit 1
}

resolve_flutterpi_tool

pushd "$APP_DIR" >/dev/null
flutter pub get
"${FLUTTERPI_CMD[@]}" build --arch=x64 --release
popd >/dev/null

rm -rf "$OUT_DIR"
mkdir -p "$OUT_DIR"

BUNDLE_DIR="$APP_DIR/build/flutter-pi/x64-generic"
if [[ ! -d "$BUNDLE_DIR" ]]; then
  echo "Flutter-pi bundle not found: $BUNDLE_DIR" >&2
  exit 1
fi

cp -a "$BUNDLE_DIR/." "$OUT_DIR/"

echo "Flutter-pi amd64 release bundle created at: $OUT_DIR"
