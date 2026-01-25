#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
APP_DIR="${1:-$ROOT_DIR/example}"
OUT_DIR="$ROOT_DIR/dist/flutter-pi-arm64"

if [[ ! -d "$APP_DIR" ]]; then
  echo "App directory not found: $APP_DIR" >&2
  exit 1
fi

FLUTTERPI_CMD=(flutterpi_tool)
if ! command -v flutterpi_tool >/dev/null 2>&1; then
  PUB_CACHE_DIR="${PUB_CACHE:-$HOME/.pub-cache}"
  PUB_BIN="$PUB_CACHE_DIR/bin/flutterpi_tool"
  if [[ -x "$PUB_BIN" ]]; then
    FLUTTERPI_CMD=("$PUB_BIN")
  elif command -v flutter >/dev/null 2>&1; then
    echo "flutterpi_tool not found. Activating..." >&2
    flutter pub global activate flutterpi_tool
    if [[ -x "$PUB_BIN" ]]; then
      FLUTTERPI_CMD=("$PUB_BIN")
    else
      FLUTTERPI_CMD=(flutter pub global run flutterpi_tool)
    fi
  else
    echo "flutterpi_tool not found in PATH or $PUB_CACHE_DIR/bin." >&2
    echo "Install with: flutter pub global activate flutterpi_tool" >&2
    exit 1
  fi
fi

pushd "$APP_DIR" >/dev/null
flutter pub get
"${FLUTTERPI_CMD[@]}" build --arch=arm64 --release
popd >/dev/null

rm -rf "$OUT_DIR"
mkdir -p "$OUT_DIR"

BUNDLE_DIR="$APP_DIR/build/flutter-pi/aarch64-generic"
if [[ ! -d "$BUNDLE_DIR" ]]; then
  echo "Flutter-pi bundle not found: $BUNDLE_DIR" >&2
  exit 1
fi

cp -a "$BUNDLE_DIR/." "$OUT_DIR/"

echo "Flutter-pi arm64 release bundle created at: $OUT_DIR"
