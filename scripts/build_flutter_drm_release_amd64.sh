#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
APP_DIR="${1:-$ROOT_DIR/example}"
OUT_DIR="$ROOT_DIR/dist/flutter-drm-amd64"

if [[ ! -d "$APP_DIR" ]]; then
  echo "App directory not found: $APP_DIR" >&2
  exit 1
fi

BUNDLER_CMD=()
BUNDLER_GIT_URL="${BUNDLER_GIT_URL:-https://github.com/buzzcola3/flutter-drm-bundler}"
BUNDLER_GIT_REF="${BUNDLER_GIT_REF:-main}"
BUNDLER_PATH="${BUNDLER_PATH:-}"

resolve_bundler() {
  if [[ -n "$BUNDLER_PATH" && -x "$BUNDLER_PATH" ]]; then
    BUNDLER_CMD=("$BUNDLER_PATH")
    return
  fi

  local candidate_dirs=(
    "$ROOT_DIR/flutter-drm-bundler"
    "$ROOT_DIR/../flutter-drm-bundler"
  )

  for dir in "${candidate_dirs[@]}"; do
    if [[ -x "$dir/build/flutter_drm_bundler" ]]; then
      BUNDLER_CMD=("$dir/build/flutter_drm_bundler")
      return
    fi
  done

  local tool_dir="$ROOT_DIR/.cache/flutter-drm-bundler"
  if [[ ! -d "$tool_dir/.git" ]]; then
    rm -rf "$tool_dir"
    git clone --depth 1 --branch "$BUNDLER_GIT_REF" "$BUNDLER_GIT_URL" "$tool_dir"
  fi

  pushd "$tool_dir" >/dev/null
  flutter pub get
  mkdir -p build
  dart compile exe bin/flutter_drm_bundler.dart -o build/flutter_drm_bundler
  popd >/dev/null

  if [[ -x "$tool_dir/build/flutter_drm_bundler" ]]; then
    BUNDLER_CMD=("$tool_dir/build/flutter_drm_bundler")
    return
  fi

  echo "Failed to build flutter-drm-bundler." >&2
  exit 1
}

resolve_bundler

# Clean previous flutter-drm bundle output to force rebuild.
# The bundler's incremental build system doesn't track the gtk-shim
# (libflutter_linux_gtk.so) as an input/output, so stale copies persist.
rm -rf "$APP_DIR/build/flutter-drm"

pushd "$APP_DIR" >/dev/null
flutter pub get
"${BUNDLER_CMD[@]}" build --arch=x64 --release
popd >/dev/null

rm -rf "$OUT_DIR"
mkdir -p "$OUT_DIR"

BUNDLE_DIR="$APP_DIR/build/flutter-drm/x64-generic"
if [[ ! -d "$BUNDLE_DIR" ]]; then
  BUNDLE_DIR="$APP_DIR/build/flutter-pi/x64-generic"
fi
if [[ ! -d "$BUNDLE_DIR" ]]; then
  echo "Flutter-drm bundle not found: $BUNDLE_DIR" >&2
  exit 1
fi

cp -a "$BUNDLE_DIR/." "$OUT_DIR/"

echo "Flutter-drm amd64 release bundle created at: $OUT_DIR"

# --- Debug-symbols build ---
DEBUG_OUT_DIR="$ROOT_DIR/dist/flutter-drm-amd64-debug"

echo "Building Flutter DRM debug-symbols bundle..."
pushd "$APP_DIR" >/dev/null
"${BUNDLER_CMD[@]}" build --arch=x64 --debug-symbols
popd >/dev/null

rm -rf "$DEBUG_OUT_DIR"
mkdir -p "$DEBUG_OUT_DIR"

DEBUG_BUNDLE_DIR="$APP_DIR/build/flutter-drm/x64-generic"
if [[ ! -d "$DEBUG_BUNDLE_DIR" ]]; then
  DEBUG_BUNDLE_DIR="$APP_DIR/build/flutter-pi/x64-generic"
fi
if [[ ! -d "$DEBUG_BUNDLE_DIR" ]]; then
  echo "Flutter-drm debug-symbols bundle not found: $DEBUG_BUNDLE_DIR" >&2
  exit 1
fi

cp -a "$DEBUG_BUNDLE_DIR/." "$DEBUG_OUT_DIR/"

echo "Flutter-drm amd64 debug-symbols bundle created at: $DEBUG_OUT_DIR"
