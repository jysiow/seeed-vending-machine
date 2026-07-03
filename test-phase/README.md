# test-phase — XIAO ESP32C6 + RFID bring-up and deploy kit

This folder holds the hardware bring-up sketches, helper scripts, and the
headless RFID **writer** for the XIAO ESP32C6, all driven from **Arduino CLI**.

Use it to:

1. Verify a XIAO ESP32C6 is connected and the toolchain works.
2. Verify the Emakefun MFRC522 (I2C) RFID module + read a card.
3. Deploy RFID code to the board with one command.
4. Write order payloads from `backend-full/data` onto RFID cards.

## Layout

```text
test-phase/
├── README.md
├── xiao-esp32c6-test/            # Blink + serial "is the board alive?" test
│   └── xiao-esp32c6-test.ino
├── xiao-esp32c6-rfid-test/       # RFID reader test (I2C scan, firmware, read UID)
│   ├── xiao-esp32c6-rfid-test.ino
│   └── Emakefun_RFID.{h,cpp}
├── xiao-esp32c6-rfid-writer/     # Headless writer: backend polling + serial commands
│   ├── xiao-esp32c6-rfid-writer.ino
│   └── Emakefun_RFID.{h,cpp}
└── scripts/
    ├── lib.sh            # shared helpers (FQBN, port auto-detect, core check)
    ├── verify-device.sh  # is a XIAO connected? is the core installed?
    ├── verify-rfid.sh    # deploy the RFID test + confirm the module answers
    ├── deploy.sh         # compile + upload any sketch folder to the XIAO
    ├── monitor.sh        # live serial monitor
    ├── read_serial.py    # reset board + read serial for N seconds (used by verify-rfid)
    └── write_card.py     # build a payload from backend-full/data and write it to a card
```

## Board facts

| Item | Value |
| ---- | ----- |
| FQBN | `esp32:esp32:XIAO_ESP32C6` |
| Serial baud | `115200` |
| User LED | `GPIO15` (`LED_BUILTIN`) |
| I2C SDA | `D4` = `GPIO22` |
| I2C SCL | `D5` = `GPIO23` |
| RFID module I2C address | `0x28` |

### RFID wiring (Emakefun MFRC522 I2C)

| RFID module | XIAO ESP32C6 |
| ----------- | ------------ |
| 5V  | 5V |
| GND | GND |
| SDA | D4 (GPIO22) |
| SCL | D5 (GPIO23) |

## One-time setup

```bash
# Arduino CLI (macOS)
brew install arduino-cli

# ESP32 core (XIAO ESP32C6 lives here). The scripts auto-install it if missing.
arduino-cli config init
arduino-cli config add board_manager.additional_urls https://espressif.github.io/arduino-esp32/package_esp32_index.json
arduino-cli core update-index
arduino-cli core install esp32:esp32

# Python serial helpers (read_serial.py / write_card.py)
python3 -m pip install pyserial
```

Use a **data-capable** USB-C cable. If the port never appears, enter bootloader
mode: hold **BOOT**, plug in USB, then release **BOOT**.

## Quick start

```bash
cd test-phase/scripts

# 1. Is the board there?
./verify-device.sh

# 2. Is the RFID module there? (tap a card to also see its UID)
./verify-rfid.sh
```

Expected `verify-rfid.sh` output includes:

```text
Found device at 0x28
MFRC522 firmware version: 0x92 (OK)
Card detected! UID: BD 66 56 2F  Type: MIFARE 1KB
```

## Deploying code to the XIAO (Arduino CLI)

`deploy.sh` compiles and uploads any sketch folder. The port is auto-detected
(override with `XIAO_PORT=/dev/cu.usbmodemXXXX`).

```bash
cd test-phase/scripts

./deploy.sh                              # default: the RFID reader test
./deploy.sh ../xiao-esp32c6-test         # blink test
./deploy.sh ../xiao-esp32c6-rfid-writer  # the backend RFID writer

./monitor.sh                             # watch serial output
```

Under the hood each deploy is just:

```bash
arduino-cli compile --fqbn esp32:esp32:XIAO_ESP32C6 <sketch-dir>
arduino-cli upload  -p <port> --fqbn esp32:esp32:XIAO_ESP32C6 <sketch-dir>
```

---

## Writing `backend-full/data` to RFID cards

This is the core of the vending flow. The backend and the writer agree on one
payload format and one card layout.

### Payload format

`backend-full/src/server.js` (`makeWriterPayload`) writes exactly:

```json
{"user_name":"<name>","order_number":"<ORD-...>","v":1}
```

### Card layout

- Card type: **MIFARE Classic 1K**
- Key: factory default `FF FF FF FF FF FF` (Key A)
- Payload stored in data blocks **4, 5, 6, 8, 9, 10** (16 bytes each,
  null-padded → **96 bytes max**)
- Sector trailers (blocks **7** and **11**) are never written
- The vending-machine reader reads these same blocks to redeem the order

### Where the data comes from

| File | Role |
| ---- | ---- |
| `backend-full/data/orders.csv` | one row per order (`user_name`, `order_number`, `status`, ...) |
| `backend-full/data/writer_jobs.csv` | queue of cards to write (`status` = PENDING → CLAIMED → WRITTEN) |
| `backend-full/data/devices.csv` | device credentials (`device_id`, `api_key`, `device_type=writer`, `enabled`) |

Relevant backend endpoints (see `backend-full/src/server.js`):

| Method + path | Purpose |
| ------------- | ------- |
| `POST /api/orders/create-and-prepare-card` | operator creates an order → makes a PENDING writer job |
| `GET  /api/rfid-writer/next-job` | writer claims the next job |
| `POST /api/rfid-writer/job-result` | writer reports WRITTEN / FAILED |
| `POST /api/rfid-writer/status` | writer heartbeat (shown on the dashboard) |

### The writer sketch has two modes

`xiao-esp32c6-rfid-writer/xiao-esp32c6-rfid-writer.ino` runs both at once:

- **ONLINE MODE** — set real Wi-Fi/API values → the XIAO polls the backend and
  writes cards automatically. This is the "continuously write" production flow.
- **SERIAL MODE** — always on. Send a one-line command over serial:
  `PING`, `READ`, or `WRITE {json}`. No Wi-Fi needed. Great for bench testing
  and for the `write_card.py` helper.

---

## How to implement it (step by step)

### Option A — ONLINE MODE (automatic, matches production)

1. **Run the backend** (in another terminal):

   ```bash
   cd backend-full
   npm install
   npm start          # serves http://localhost:3000 and the dashboard
   ```

   Find your PC's LAN IP (the XIAO needs it): `ipconfig getifaddr en0`.

2. **Register the device.** The writer defaults to the existing row
   `wio-rfid-writer` / `WIO_WRITER_SECRET` in `backend-full/data/devices.csv`,
   so it works out of the box. To give the XIAO its own identity, add a row:

   ```csv
   xiao-rfid-writer,writer,XIAO_WRITER_SECRET,true,XIAO ESP32C6 RFID writer
   ```

3. **Configure the sketch.** Edit the top of
   `xiao-esp32c6-rfid-writer/xiao-esp32c6-rfid-writer.ino`:

   ```cpp
   const char* WIFI_SSID     = "YOUR_WIFI_SSID";
   const char* WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";
   const char* API_BASE      = "http://192.168.1.20:3000";   // your PC IP, no trailing slash
   const char* DEVICE_ID     = "wio-rfid-writer";            // or xiao-rfid-writer
   const char* API_KEY       = "WIO_WRITER_SECRET";          // or XIAO_WRITER_SECRET
   ```

4. **Deploy and watch:**

   ```bash
   cd test-phase/scripts
   ./deploy.sh ../xiao-esp32c6-rfid-writer
   ./monitor.sh
   ```

5. **Drive it from the dashboard.** Open `http://localhost:3000`, create an
   order (→ a PENDING writer job). Place a card on the reader; the XIAO writes
   the payload, verifies it, and reports `WRITTEN`. The order/job status updates
   in `backend-full/data`.

### Option B — SERIAL MODE (manual, no Wi-Fi)

Leave `WIFI_SSID` as `"YOUR_WIFI_SSID"`, deploy the writer, then use the helper
that reads `backend-full/data` and sends the write command:

```bash
cd test-phase/scripts
./deploy.sh ../xiao-esp32c6-rfid-writer

# See queued jobs from backend-full/data/writer_jobs.csv
python3 write_card.py --list

# Write the newest PENDING job to a card
python3 write_card.py

# Write a specific order (looked up in orders.csv)
python3 write_card.py --order ORD-20260101-ABCDEF

# Write an explicit payload without any CSV lookup
python3 write_card.py --user alice --order ORD-1

# Just show the payload, don't write
python3 write_card.py --user alice --order ORD-1 --dry-run
```

`write_card.py` builds the byte-identical payload
(`{"user_name":...,"order_number":...,"v":1}`) and sends `WRITE <json>` to the
board. It only writes the card; the backend API remains the source of truth for
CSV status. You can confirm a write with the on-device `READ` command:

```bash
./monitor.sh
# then type:  READ   (and tap the card)
```

---

## Troubleshooting

| Symptom | Fix |
| ------- | --- |
| No port in `verify-device.sh` | Data USB-C cable; bootloader mode (hold BOOT, plug in, release); `XIAO_PORT=/dev/cu.usbmodemXXXX` |
| `verify-rfid.sh` fails, no `0x28` | Check SDA=D4, SCL=D5, 5V, GND; confirm module address is `0x28` |
| Firmware reads `0x00`/`0xFF` | Module not powered / I2C not wired; re-seat the Grove/PH2.0 cable |
| Upload fails | Enter bootloader mode, retry `./deploy.sh` |
| `pyserial not installed` | `python3 -m pip install pyserial` |
| Write "verify mismatch" | Card is not MIFARE Classic 1K, or key changed from default `FFFFFFFFFFFF` |
| Payload > 96 bytes | Shorten `user_name`/`order_number`; only 6 blocks × 16 bytes are used |

## Notes

- The `Emakefun_RFID.{h,cpp}` files are copied into each sketch folder because
  Arduino compiles the `.ino` together with sources in the same folder. They are
  the same library used by `rfid-write-station/wio_rfid_writer`.
- The writer is the headless XIAO port of
  `rfid-write-station/wio_rfid_writer/wio_rfid_writer.ino` (no TFT; status via
  serial + the user LED).
