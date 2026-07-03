#!/usr/bin/env bash
# Shared helpers for the XIAO ESP32C6 test-phase scripts.
# Source this from the other scripts: `source ./lib.sh`

set -euo pipefail

# Board target. XIAO ESP32C6 uses the Espressif esp32 core.
FQBN="${FQBN:-esp32:esp32:XIAO_ESP32C6}"
BAUD="${BAUD:-115200}"
ESP32_INDEX_URL="https://espressif.github.io/arduino-esp32/package_esp32_index.json"

info()  { printf '\033[36m[i]\033[0m  %s\n' "$*"; }
ok()    { printf '\033[32m[ok]\033[0m %s\n' "$*"; }
warn()  { printf '\033[33m[!]\033[0m  %s\n' "$*"; }
err()   { printf '\033[31m[x]\033[0m  %s\n' "$*" >&2; }

require_arduino_cli() {
  if ! command -v arduino-cli >/dev/null 2>&1; then
    err "arduino-cli not found."
    err "Install it with:  brew install arduino-cli"
    exit 1
  fi
}

# Ensure the esp32 core is installed; install it if missing.
require_core() {
  if arduino-cli core list 2>/dev/null | grep -q '^esp32:esp32'; then
    return 0
  fi
  warn "esp32:esp32 core not installed. Installing now..."
  arduino-cli config init 2>/dev/null || true
  arduino-cli config add board_manager.additional_urls "$ESP32_INDEX_URL" 2>/dev/null || true
  arduino-cli core update-index
  arduino-cli core install esp32:esp32
}

# Print the XIAO serial port. Override with:  XIAO_PORT=/dev/cu.xxx ./script.sh
detect_port() {
  if [[ -n "${XIAO_PORT:-}" ]]; then
    echo "$XIAO_PORT"
    return 0
  fi
  local port=""
  # Prefer a USB serial port that arduino-cli reports.
  port="$(arduino-cli board list 2>/dev/null \
    | awk '/usbmodem|usbserial|wchusbserial|SLAB|usbserial-/ {print $1; exit}')"
  # Fallback: scan /dev directly (macOS naming).
  if [[ -z "$port" ]]; then
    port="$(ls /dev/cu.usbmodem* /dev/cu.usbserial* /dev/cu.wchusbserial* 2>/dev/null | head -n1 || true)"
  fi
  echo "$port"
}

# Resolve a sketch directory argument to an absolute path and validate it.
resolve_sketch() {
  local dir="$1"
  if [[ ! -d "$dir" ]]; then
    err "Sketch directory not found: $dir"
    exit 1
  fi
  (cd "$dir" && pwd)
}
