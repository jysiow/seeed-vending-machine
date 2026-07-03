#!/usr/bin/env bash
# Verify the Emakefun MFRC522 (I2C) RFID module on the connected XIAO.
#
#   ./verify-rfid.sh
#
# It deploys the RFID reader test, then reads the serial output and checks
# that the MFRC522 answered. Place a card on the reader to also see its UID.
#
# Exit code 0 = RFID module responded (firmware 0x91/0x92).

set -euo pipefail
cd "$(dirname "$0")"
source ./lib.sh

RFID_TEST_DIR="../xiao-esp32c6-rfid-test"

require_arduino_cli
require_core

PORT="$(detect_port)"
if [[ -z "$PORT" ]]; then
  err "No XIAO port found. Run ./verify-device.sh first."
  exit 1
fi

info "Deploying RFID reader test to $PORT ..."
./deploy.sh "$RFID_TEST_DIR"

info "Reading serial (place an RFID card on the reader now)..."
if python3 ./read_serial.py --port "$PORT" --baud "$BAUD" --seconds 20 \
     --expect "MFRC522 firmware version: 0x9[12]"; then
  ok "RFID module responded. If you tapped a card you saw its UID + type above."
  info "Live view any time with:  ./monitor.sh"
else
  err "RFID module did NOT respond."
  err "  - Check wiring: SDA=D4 (GPIO22), SCL=D5 (GPIO23), 5V, GND."
  err "  - Confirm the module I2C address is 0x28."
  err "  - The I2C scan line above should list a device at 0x28."
  exit 1
fi
