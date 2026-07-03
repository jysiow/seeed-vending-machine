#!/usr/bin/env bash
# Verify that a XIAO ESP32C6 is connected and the toolchain is ready.
#
#   ./verify-device.sh
#
# Exit code 0 = a XIAO serial port was found.

set -euo pipefail
cd "$(dirname "$0")"
source ./lib.sh

require_arduino_cli
info "$(arduino-cli version)"

require_core
ok "esp32:esp32 core: $(arduino-cli core list | awk '/^esp32:esp32/{print $2}')"

info "Connected boards (arduino-cli board list):"
arduino-cli board list

PORT="$(detect_port)"
if [[ -z "$PORT" ]]; then
  err "No XIAO serial port detected."
  err "  - Use a DATA-capable USB-C cable (not charge-only)."
  err "  - Bootloader mode: hold BOOT, plug USB in, then release BOOT."
  err "  - Or set the port manually:  XIAO_PORT=/dev/cu.usbmodemXXXX ./verify-device.sh"
  exit 1
fi

ok "XIAO detected on port: $PORT"
info "FQBN: $FQBN"
info "Next: ./verify-rfid.sh  (checks the RFID module + reads a card)"
