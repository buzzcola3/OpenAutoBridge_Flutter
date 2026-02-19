#!/usr/bin/env bash
# GDB wrapper for remote Pi debugging via DebugLantern.
#
# Uses -iex (init-early-execute) to set sysroot and solib-search-path
# BEFORE the debug adapter issues "target remote", so GDB resolves
# shared libraries locally instead of fetching them over the network.
#
# Referenced by launch.json: "gdbpath": "${workspaceFolder}/scripts/gdb-remote.sh"

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
WORKSPACE_DIR="$(dirname "$SCRIPT_DIR")"

# Resolve candidate Buildroot staging directories (prefer unstripped staging)
STAGING_CANDIDATES=(
  "$WORKSPACE_DIR/../OpenAutoOpiZ3/buildroot/output/staging/usr"
  "$HOME/Projects/OpenAuto/OpenAutoOpiZ3/buildroot/output/staging/usr"
  "$WORKSPACE_DIR/../buildroot/output/staging/usr"
  "$WORKSPACE_DIR/../../buildroot/output/staging/usr"
)
BUILDROOT_STAGING=""
for c in "${STAGING_CANDIDATES[@]}"; do
  if [[ -d "$c" ]]; then
    BUILDROOT_STAGING="$c"
    break
  fi
done

# Prefer Buildroot staging (unstripped) > DebugLantern-downloaded sysroot > manual pi-debug-symbols
if [[ -n "$BUILDROOT_STAGING" ]]; then
  SYSROOT="$BUILDROOT_STAGING"
  echo "Using Buildroot staging as sysroot: $SYSROOT" >&2
elif [[ -d "$WORKSPACE_DIR/dist/dl-sysroot" ]]; then
  SYSROOT="$WORKSPACE_DIR/dist/dl-sysroot"
  echo "Using DebugLantern-downloaded sysroot: $SYSROOT" >&2
else
  SYSROOT="$WORKSPACE_DIR/dist/pi-debug-symbols/sysroot"
  echo "Using fallback sysroot: $SYSROOT" >&2
fi

DEBUG_SYMS="$WORKSPACE_DIR/dist/pi-debug-symbols"

# Solib search path: include debug symbols dir and typical sysroot lib paths
SOLIB_PATHS=(
  "$DEBUG_SYMS"
  "$DEBUG_SYMS/plugins"
  "$SYSROOT/usr/lib"
  "$SYSROOT/lib64"
  "$SYSROOT/lib"
)
SOLIB_SEARCH_PATH=$(IFS=:; echo "${SOLIB_PATHS[*]}")

# Substitute path mappings: container/CI build paths -> local workspace/cache
SUBST_PATHS=(
  # Docker build container -> workspace
  "/workspace $WORKSPACE_DIR"
  # CI-built flutter-drm-embedder source paths (GitHub Actions __w/ prefix)
  "__w/flutter-drm-embedder/flutter-drm-embedder $WORKSPACE_DIR/.cache/flutter-drm-embedder"
  "__w/flutter-drm-embedder $WORKSPACE_DIR/.cache/flutter-drm-embedder"
  "/__w/flutter-drm-embedder/flutter-drm-embedder $WORKSPACE_DIR/.cache/flutter-drm-embedder"
  "/__w/flutter-drm-embedder $WORKSPACE_DIR/.cache/flutter-drm-embedder"
  # Flutter engine source (e.g. ../../flutter/third_party/dart/runtime/vm/symbols.h)
  "../../flutter $WORKSPACE_DIR/.cache/engine-src/engine/src/flutter"
  "flutter $WORKSPACE_DIR/.cache/engine-src/engine/src/flutter"
)

# Auto-load local executable if present (prefer debug bundle embedder)
LOCAL_EXE=""
EXE_CANDIDATES=(
  "$WORKSPACE_DIR/dist/flutter-drm-arm64-debug/flutter-drm-embedder"
  "$WORKSPACE_DIR/dist/dl-sysroot/flutter-drm-embedder"
  "$WORKSPACE_DIR/dist/pi-debug-symbols/flutter-drm-embedder"
  "$WORKSPACE_DIR/dist/flutter-drm-arm64/flutter-drm-embedder"
)
for e in "${EXE_CANDIDATES[@]}"; do
  if [[ -x "$e" ]]; then
    LOCAL_EXE="$e"
    break
  fi
done

# Build gdb args
GDB_ARGS=(
  -iex "set debuginfod enabled off"
  -iex "set auto-load safe-path /"
  -iex "set sysroot $SYSROOT"
  -iex "set solib-search-path $SOLIB_SEARCH_PATH"
  # Add source directories for resolving relative paths (e.g. build/../src/main.c)
  -iex "set directories $WORKSPACE_DIR/.cache/flutter-drm-embedder:$WORKSPACE_DIR/.cache/flutter-drm-embedder/src:$WORKSPACE_DIR/.cache/engine-src/engine/src/flutter"

# Add substitute-path mappings for GDB so source paths from CI/device map locally
)

for sp in "${SUBST_PATHS[@]}"; do
  GDB_ARGS+=( -iex "set substitute-path $sp" )
done
 

if [[ -n "$LOCAL_EXE" ]]; then
  echo "Auto-loading executable for symbols: $LOCAL_EXE" >&2
  GDB_ARGS+=( -ex "file $LOCAL_EXE" -ex "set breakpoint pending on" -ex "break main.c:main" )
fi

# Stop on C++ exceptions (throw and uncaught)
GDB_ARGS+=( -ex "catch throw" -ex "catch catch" )

exec gdb-multiarch "${GDB_ARGS[@]}" "$@"
