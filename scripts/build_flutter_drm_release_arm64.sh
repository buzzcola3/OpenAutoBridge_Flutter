#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
APP_DIR="${1:-$ROOT_DIR/example}"
OUT_DIR="$ROOT_DIR/dist/flutter-drm-arm64"

if [[ ! -d "$APP_DIR" ]]; then
  echo "App directory not found: $APP_DIR" >&2
  exit 1
fi

BUNDLER_CMD=()
BUNDLER_GIT_URL="${BUNDLER_GIT_URL:-https://github.com/buzzcola3/flutter-drm-bundler}"
BUNDLER_GIT_REF="${BUNDLER_GIT_REF:-main}"
BUNDLER_PATH="${BUNDLER_PATH:-}"
BUNDLER_ALWAYS_CHECK_UPDATES="${BUNDLER_ALWAYS_CHECK_UPDATES:-1}"

# The bundler is a compiled native exe, so a copy built on another architecture
# is unusable. It must be detected explicitly: with qemu binfmt registered on the
# host, containers inherit it and a stale aarch64 exe will *silently* run under
# emulation inside the amd64 cross image, then fail deep inside flutter_tools.
# ELF e_machine lives at offset 18 (little endian): 3e00 = x86-64, b700 = aarch64.
bundler_arch_matches_host() {
  local exe="$1" want
  case "$(uname -m)" in
    x86_64)  want="3e00" ;;
    aarch64) want="b700" ;;
    *)       return 0 ;;  # unknown host; don't force a rebuild
  esac
  [[ "$(od -An -tx1 -j18 -N2 "$exe" 2>/dev/null | tr -d ' \n')" == "$want" ]]
}

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
    if [[ -x "$dir/build/flutter_drm_bundler" ]] \
       && bundler_arch_matches_host "$dir/build/flutter_drm_bundler"; then
      BUNDLER_CMD=("$dir/build/flutter_drm_bundler")
      return
    fi
  done

  local tool_dir="$ROOT_DIR/.cache/flutter-drm-bundler"
  local rebuild_bundler=0
  if [[ ! -d "$tool_dir/.git" ]]; then
    rm -rf "$tool_dir"
    git clone --depth 1 --branch "$BUNDLER_GIT_REF" "$BUNDLER_GIT_URL" "$tool_dir"
    rebuild_bundler=1
  fi

  pushd "$tool_dir" >/dev/null
  git remote set-url origin "$BUNDLER_GIT_URL"
  if [[ "$BUNDLER_ALWAYS_CHECK_UPDATES" == "1" ]]; then
    local old_head="$(git rev-parse HEAD 2>/dev/null || true)"
    git fetch --depth 1 origin "$BUNDLER_GIT_REF"
    local new_head="$(git rev-parse FETCH_HEAD 2>/dev/null || true)"
    if [[ -n "$new_head" && "$new_head" != "$old_head" ]]; then
      git checkout -B "$BUNDLER_GIT_REF" FETCH_HEAD
      rebuild_bundler=1
      echo "[bundler] updated to $new_head"
    fi
  fi

  if [[ ! -x build/flutter_drm_bundler ]]; then
    rebuild_bundler=1
  elif ! bundler_arch_matches_host build/flutter_drm_bundler; then
    echo "[bundler] cached exe is for another architecture; rebuilding for $(uname -m)"
    rebuild_bundler=1
  fi

  if [[ "$rebuild_bundler" == "1" ]]; then
    flutter pub get
    mkdir -p build
    dart compile exe bin/flutter_drm_bundler.dart -o build/flutter_drm_bundler
  fi
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

# Plugin symlinks under linux/flutter/ephemeral/ are generated state holding
# absolute paths, so they are only valid in the environment that created them.
# A previous run's host-side rewrite (see the fixup at the end of this script)
# leaves them pointing at paths that do not exist inside the container, which
# breaks add_subdirectory() in generated_plugins.cmake. Drop them and let
# `flutter pub get` regenerate them for wherever we are running now.
SYMLINK_DIR="$APP_DIR/linux/flutter/ephemeral/.plugin_symlinks"
rm -rf "$SYMLINK_DIR"

pushd "$APP_DIR" >/dev/null
flutter pub get

# The bundler only *locates* an existing plugin .so - it never invokes cmake or
# ninja - so the native plugin must be compiled here. Without this step a stale
# libopen_auto_bridge_flutter_plugin.so from an earlier run is silently bundled
# and shipped to the Pi, so linux/*.cc changes never take effect.
flutter_build_args=(linux --release)
if [[ "$(uname -m)" == "x86_64" ]]; then
  # Cross-compiling from the amd64 builder image (see Dockerfile.arm64-cross).
  flutter_build_args+=(--target-platform=linux-arm64)
fi
flutter build "${flutter_build_args[@]}"

"${BUNDLER_CMD[@]}" build --arch=arm64 --release
popd >/dev/null

rm -rf "$OUT_DIR"
mkdir -p "$OUT_DIR"

BUNDLE_DIR="$APP_DIR/build/flutter-drm/aarch64-generic"
if [[ ! -d "$BUNDLE_DIR" ]]; then
  BUNDLE_DIR="$APP_DIR/build/flutter-pi/aarch64-generic"
fi
if [[ ! -d "$BUNDLE_DIR" ]]; then
  echo "Flutter-drm bundle not found: $BUNDLE_DIR" >&2
  exit 1
fi

cp -a "$BUNDLE_DIR/." "$OUT_DIR/"

echo "Flutter-drm arm64 release bundle created at: $OUT_DIR"

# --- Debug-symbols build ---
DEBUG_OUT_DIR="$ROOT_DIR/dist/flutter-drm-arm64-debug"

echo "Building Flutter DRM debug-symbols bundle..."
pushd "$APP_DIR" >/dev/null
"${BUNDLER_CMD[@]}" build --arch=arm64 --debug-symbols
popd >/dev/null

rm -rf "$DEBUG_OUT_DIR"
mkdir -p "$DEBUG_OUT_DIR"

DEBUG_BUNDLE_DIR="$APP_DIR/build/flutter-drm/aarch64-generic"
if [[ ! -d "$DEBUG_BUNDLE_DIR" ]]; then
  DEBUG_BUNDLE_DIR="$APP_DIR/build/flutter-pi/aarch64-generic"
fi
if [[ ! -d "$DEBUG_BUNDLE_DIR" ]]; then
  echo "Flutter-drm debug-symbols bundle not found: $DEBUG_BUNDLE_DIR" >&2
  exit 1
fi

cp -a "$DEBUG_BUNDLE_DIR/." "$DEBUG_OUT_DIR/"

echo "Flutter-drm arm64 debug-symbols bundle created at: $DEBUG_OUT_DIR"

# --- Fix plugin symlinks for host debugging ---
# `flutter pub get` runs inside the container, where the workspace is mounted at
# /workspace, so it writes absolute symlinks pointing at container paths. On the
# host those dangle and the debugger cannot resolve plugin sources.
#
# The container cannot discover the host path on its own, so the caller passes it
# in as HOST_ROOT_DIR. Rewriting against $ROOT_DIR instead would be a no-op here,
# since ROOT_DIR is itself /workspace inside the container.
HOST_ROOT_DIR="${HOST_ROOT_DIR:-}"
CONTAINER_ROOT="${CONTAINER_ROOT:-/workspace}"

if [[ -n "$HOST_ROOT_DIR" ]]; then
  if [[ -d "$SYMLINK_DIR" ]]; then
    for link in "$SYMLINK_DIR"/*; do
      [[ -L "$link" ]] || continue
      target="$(readlink "$link")"
      if [[ "$target" == "$CONTAINER_ROOT" || "$target" == "$CONTAINER_ROOT"/* ]]; then
        host_target="${HOST_ROOT_DIR}${target#"$CONTAINER_ROOT"}"
        echo "Fixing symlink: $(basename "$link") -> $host_target"
        ln -snf "$host_target" "$link"
      fi
    done
  fi
fi
