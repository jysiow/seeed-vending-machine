#!/usr/bin/env bash
# Compile and upload an Arduino sketch to the connected XIAO ESP32C6.
#
#   ./deploy.sh [sketch-dir]
#
# Defaults to the RFID reader test. Examples:
#   ./deploy.sh                              # deploy xiao-esp32c6-rfid-test
#   ./deploy.sh ../xiao-esp32c6-rfid-writer  # deploy the backend RFID writer
#   ./deploy.sh ../xiao-esp32c6-test         # deploy the blink test
#
# Override the port with:  XIAO_PORT=/dev/cu.usbmodemXXXX ./deploy.sh <dir>

set -euo pipefail
cd "$(dirname "$0")"
source ./lib.sh

SKETCH_DIR="${1:-../xiao-esp32c6-rfid-test}"

require_arduino_cli
require_core

SKETCH_DIR="$(resolve_sketch "$SKETCH_DIR")"
PORT="$(detect_port)"
if [[ -z "$PORT" ]]; then
  err "No XIAO port found. Run ./verify-device.sh first."
  exit 1
fi

info "Sketch: $SKETCH_DIR"
info "Port:   $PORT"
info "FQBN:   $FQBN"

info "Compiling..."
arduino-cli compile --fqbn "$FQBN" "$SKETCH_DIR"
ok "Compiled."

info "Uploading..."
arduino-cli upload -p "$PORT" --fqbn "$FQBN" "$SKETCH_DIR"
ok "Uploaded to $PORT."
info "Watch output with:  ./monitor.sh"
