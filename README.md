# Seeed Vending System

Wio Terminal vending machine with CSV-backed Node.js backends, RFID card programming, and Arduino firmware.

## Project layout

```text
backend-full/
  Main operator dashboard + APIs (port 3000) — npm start
  Card-flow vending API (port 3001)       — npm run start:vending
  data/          Dashboard CSVs (orders, inventory, customers, …)
  data-vending/  Wio vending CSVs (cards, orders, inventory)

frontend-vending-machine/wio_backend_full_reader/
  Customer-facing Wio sketch — scan card, shop, dispense

rfid-write-station/wio_rfid_writer/
  Staff Wio sketch — programs RFID cards from backend write jobs
```

## Two backends (same folder, different ports)

| Server | Command | Port | Purpose |
|--------|---------|------|---------|
| **Dashboard** | `npm start` | 3000 | Browser UI, order management, device tools (`src/server.mjs`) |
| **Vending** | `npm run start:vending` | 3001 | Wio card-check, purchase, redeem, RFID writer jobs (`server-vending.js`) |

`server.js` (legacy, port 3000) is kept for reference; the dashboard uses `server.mjs`.

The vending Wio and write station must point at **port 3001** on your laptop IP.

## Quick start (local booth)

```bash
cd backend-full
npm install

# Terminal 1 — dashboard (optional for operators)
npm start

# Terminal 2 — vending API (required for Wio)
npm run start:vending
```

Test vending API:

```bash
curl http://localhost:3001/health
sh test_requests.sh
```

Find laptop IP (`ipconfig` on Windows). Both machines must be on the same WiFi (e.g. `SEEED-MKT`).

## Wio firmware setup

### Vending machine

Open `frontend-vending-machine/wio_backend_full_reader/wio_backend_full_reader.ino`

```cpp
const char* WIFI_SSID = "SEEED-MKT";
const char* WIFI_PASSWORD = "your_password";
const char* BACKEND_BASE_URL = "http://192.168.7.176:3001";  // laptop IP, port 3001
```

Upload to the vending Wio (Seeeduino Wio Terminal).

### RFID write station

Open `rfid-write-station/wio_rfid_writer/wio_rfid_writer.ino`

```cpp
const char* API_BASE = "http://192.168.7.176:3001";
const char* API_KEY = "WIO_WRITER_SECRET";
```

Upload to the writer Wio. Create write jobs via `POST /api/rfid-writer/jobs` (see below).

Do not add a trailing slash to backend URLs.

## RFID card payload (v2)

Cards store JSON across MIFARE blocks **4, 5, 6, 8, 9, 10** (96 bytes max):

```json
{"v":2,"uid":"11 4A 47 A0","usr":"Matthew","ord":null,"st":null,"bal":20.00}
```

| Field | Meaning |
|-------|---------|
| `uid` | Card identity (`card_uid`) |
| `usr` | User name — must match backend |
| `ord` | Order number, or `null` for direct wallet |
| `st` | `ready`, `partial`, `collected`, or `null` |
| `bal` | Balance on card (used for direct purchases) |

## Runtime flows

### Direct wallet (no order on card)

```text
1. Write station programs card: ord=null, st=null, bal=20.00
2. Customer scans card at vending Wio
3. Wio reads JSON → POST /machine/card-check (order_number empty)
4. Backend verifies uid + usr → DIRECT|20.00
5. Customer shops → POST /machine/direct-purchase (balance from card)
6. Wio writes updated balance back to card
```

### Prepaid order

```text
1. Order exists in data-vending/orders.csv (status=ready)
2. Write station programs card: ord=ORD-001, st=ready
3. Customer scans → POST /machine/card-check with order_number
4. Backend returns PREPAID|… or PREPAID_PARTIAL|… (if stock limited)
5. Wio dispenses → POST /machine/redeem-order
6. If fully collected: Wio clears ord and st on card (bal unchanged)
   If partial: Wio sets st=partial on card
```

### Already collected

If card still has `st=collected` with an order, Wio shows “Already collected” then retries as **direct** (wallet only).

Invalid or missing JSON on scan → **Unauthorised card**.

## Vending API (`server-vending.js`, port 3001)

### Machine endpoints (form-encoded)

| Method | Path | Purpose |
|--------|------|---------|
| POST | `/machine/card-check` | Verify card; direct or prepaid plan |
| POST | `/machine/redeem-order` | Confirm dispense; update order CSV |
| POST | `/machine/direct-purchase` | Deduct card balance; dispense cart |

**card-check** body fields: `hardware_uid`, `card_uid`, `user_name`, `order_number`, `collection_status`, `balance`

### RFID writer endpoints (JSON, header `x-api-key: WIO_WRITER_SECRET`)

| Method | Path | Purpose |
|--------|------|---------|
| POST | `/api/rfid-writer/jobs` | Queue a write job |
| GET | `/api/rfid-writer/next-job` | Writer Wio polls for work |
| POST | `/api/rfid-writer/job-result` | Writer reports success/failure |

**Job types:** `full_write`, `mark_collected`, `mark_partial`

Example — program a direct wallet card:

```bash
curl -X POST http://localhost:3001/api/rfid-writer/jobs \
  -H "Content-Type: application/json" \
  -H "x-api-key: WIO_WRITER_SECRET" \
  -d '{"job_type":"full_write","card_uid":"11 4A 47 A0","user_name":"Matthew","balance":20}'
```

Place a card on the write station; it will poll and write the JSON payload.

## Data files

### `data-vending/` (vending server only)

| File | Purpose |
|------|---------|
| `cards.csv` | Registered `card_uid` + `user_name` (identity) |
| `orders.csv` | Prepaid orders: `q1–q4`, `c1–c4` collected, `status` |
| `inventory.csv` | Stock and price for 4 products |
| `transactions.csv` | Auto-created action log |

### `data/` (dashboard server)

Used by `server.mjs` for the operator UI — separate schema. Do not mix the two servers writing the same files at once.

## Sample test cards (`data-vending/cards.csv`)

| card_uid | user_name | Use |
|----------|-----------|-----|
| `11 4A 47 A0` | Matthew | Direct wallet testing |
| `17 6C EE EA` | Alice | Prepaid order `ORD-001` |

## Render deployment

For the **dashboard** only:

```text
Root Directory: backend-full
Build Command:  npm install
Start Command:  npm start
```

The vending server (`server-vending.js`) is intended for **local same-WiFi** use with the Wio terminals. For cloud deploy, run it on a host reachable from your booth network and update `BACKEND_BASE_URL` / `API_BASE` in the sketches.

## Hardware

- **Vending:** Seeeduino Wio Terminal + Grove RFID (I2C `0x28`) + STS servos
- **Writer:** Wio Terminal + MFRC522 RFID module
- **Network:** Wio and laptop on the same SSID (e.g. `SEEED-MKT`)
