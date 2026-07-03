#!/usr/bin/env bash
# Open a live serial monitor to the connected XIAO ESP32C6 (Ctrl+C to stop).
#
#   ./monitor.sh
#
# Override the port with:  XIAO_PORT=/dev/cu.usbmodemXXXX ./monitor.sh

set -euo pipefail
cd "$(dirname "$0")"
source ./lib.sh

PORT="$(detect_port)"
if [[ -z "$PORT" ]]; then
  err "No XIAO port found. Run ./verify-device.sh first."
  exit 1
fi

info "Monitoring $PORT @ ${BAUD} baud (Ctrl+C to stop)"
exec arduino-cli monitor -p "$PORT" -c baudrate="$BAUD"
