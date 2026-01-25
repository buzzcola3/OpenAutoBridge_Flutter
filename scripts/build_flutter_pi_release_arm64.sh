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
FLUTTERPI_TOOL_GIT_URL="${FLUTTERPI_TOOL_GIT_URL:-https://github.com/ardera/flutterpi_tool.git}"
FLUTTERPI_TOOL_GIT_REF="${FLUTTERPI_TOOL_GIT_REF:-main}"
FLUTTERPI_TOOL_PUB_VERSION="${FLUTTERPI_TOOL_PUB_VERSION:-}"
PUB_CACHE_DIR="${PUB_CACHE:-$HOME/.pub-cache}"

activate_flutterpi_tool() {
  if [[ -n "$FLUTTERPI_TOOL_PUB_VERSION" ]]; then
    flutter pub global activate flutterpi_tool "$FLUTTERPI_TOOL_PUB_VERSION"
    return
  fi

  if flutter pub global activate -sgit "$FLUTTERPI_TOOL_GIT_URL" --git-ref "$FLUTTERPI_TOOL_GIT_REF"; then
    return
  fi

  echo "flutterpi_tool activation failed; trying patched local clone..." >&2

  local tmp_dir
  tmp_dir="$(mktemp -d)"
  trap "rm -rf '$tmp_dir'" EXIT
  git clone --depth 1 --branch "$FLUTTERPI_TOOL_GIT_REF" "$FLUTTERPI_TOOL_GIT_URL" "$tmp_dir/flutterpi_tool"

  local device_file
  local os_file
  device_file="$tmp_dir/flutterpi_tool/lib/src/devices/flutterpi_ssh/device_discovery.dart"
  os_file="$tmp_dir/flutterpi_tool/lib/src/more_os_utils.dart"

  if [[ -f "$device_file" ]] && ! grep -q "forWirelessDiscovery" "$device_file"; then
    perl -0777 -i -pe 's/Future<List<Device>> pollingGetDevices\(\{Duration\? timeout\}\)/Future<List<Device>> pollingGetDevices\(\{Duration\? timeout, bool forWirelessDiscovery = false\}\)/' "$device_file"
  fi

  if [[ -f "$os_file" ]] && ! grep -q "linux_riscv64" "$os_file"; then
    perl -0777 -i -pe 's/HostPlatform\.linux_arm64 => FlutterpiHostPlatform\.linuxARM64,\n/HostPlatform.linux_arm64 => FlutterpiHostPlatform.linuxARM64,\n      HostPlatform.linux_riscv64 => FlutterpiHostPlatform.linuxRV64,\n/' "$os_file"
  fi

  flutter pub global activate -s path "$tmp_dir/flutterpi_tool"
}

patch_flutterpi_tool() {
  local device_file
  local os_file

  device_file="$(find "$PUB_CACHE_DIR" -type f -path "*flutterpi_tool*/lib/src/devices/flutterpi_ssh/device_discovery.dart" 2>/dev/null | head -n 1)"
  os_file="$(find "$PUB_CACHE_DIR" -type f -path "*flutterpi_tool*/lib/src/more_os_utils.dart" 2>/dev/null | head -n 1)"

  if [[ -f "$device_file" ]] && ! grep -q "forWirelessDiscovery" "$device_file"; then
    perl -0777 -i -pe 's/Future<List<Device>> pollingGetDevices\(\{Duration\? timeout\}\)/Future<List<Device>> pollingGetDevices\(\{Duration\? timeout, bool forWirelessDiscovery = false\}\)/' "$device_file"
  fi

  if [[ -f "$os_file" ]] && ! grep -q "linux_riscv64" "$os_file"; then
    perl -0777 -i -pe 's/HostPlatform\.linux_arm64 => FlutterpiHostPlatform\.linuxARM64,\n/HostPlatform.linux_arm64 => FlutterpiHostPlatform.linuxARM64,\n      HostPlatform.linux_riscv64 => FlutterpiHostPlatform.linuxRV64,\n/' "$os_file"
  fi
}
if ! command -v flutterpi_tool >/dev/null 2>&1; then
  PUB_BIN="$PUB_CACHE_DIR/bin/flutterpi_tool"
  if [[ -x "$PUB_BIN" ]]; then
    FLUTTERPI_CMD=("$PUB_BIN")
  elif command -v flutter >/dev/null 2>&1; then
    echo "flutterpi_tool not found. Activating..." >&2
    activate_flutterpi_tool
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

patch_flutterpi_tool

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
