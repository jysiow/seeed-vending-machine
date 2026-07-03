/*
  XIAO ESP32C6 headless RFID writer for the backend-full vending system.

  This is the screen-less XIAO port of rfid-write-station/wio_rfid_writer.
  The payload format and card block layout match backend-full/src/server.js
  and the vending-machine reader, so cards written here work end to end.

  ------------------------------------------------------------------------
  Two ways to drive it (both are always available):

  1) ONLINE MODE  (set real Wi-Fi + API values below)
     - Polls   GET  /api/rfid-writer/next-job
     - Writes the order payload to a card when one is present
     - Reports POST /api/rfid-writer/job-result
     - Heartbeats POST /api/rfid-writer/status
     This is the "continuously write backend-full/data to cards" flow.

  2) SERIAL MODE  (no Wi-Fi needed, great for testing)
     Send one line over serial @115200:
       PING                 -> print module + link status
       READ                 -> read the payload currently on a card
       WRITE {json}         -> write {json} to the next card presented
     Example:
       WRITE {"user_name":"alice","order_number":"ORD-1","v":1}
     The PC helper scripts/write_card.py builds this line from
     backend-full/data automatically.
  ------------------------------------------------------------------------

  Payload written to the card (from server.js makeWriterPayload):
     {"user_name":"...","order_number":"...","v":1}
  stored in MIFARE Classic 1K data blocks 4,5,6,8,9,10 (16 bytes each,
  null-padded). Sector trailers (blocks 7 and 11) are never written.

  Wiring (Emakefun MFRC522 I2C module):
     RFID 5V  -> XIAO 5V
     RFID GND -> XIAO GND
     RFID SDA -> XIAO D4 (GPIO22)
     RFID SCL -> XIAO D5 (GPIO23)
  Default module I2C address: 0x28.

  Required files in this folder: Emakefun_RFID.h / Emakefun_RFID.cpp
*/

#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include "Emakefun_RFID.h"

// ------------------------- User config -------------------------
// Leave WIFI_SSID as "YOUR_WIFI_SSID" to stay in serial-only mode.
const char* WIFI_SSID     = "YOUR_WIFI_SSID";
const char* WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";
// backend-full base URL, e.g. http://192.168.1.20:3000 (no trailing slash).
const char* API_BASE      = "http://YOUR_PC_IP:3000";
// Must match a row in backend-full/data/devices.csv (device_type=writer, enabled=true).
const char* DEVICE_ID     = "wio-rfid-writer";
const char* API_KEY       = "WIO_WRITER_SECRET";

// ------------------------- Hardware -------------------------
static const int  I2C_SDA   = 22;   // XIAO D4
static const int  I2C_SCL   = 23;   // XIAO D5
static const byte RFID_ADDR = 0x28;

MFRC522 mfrc522(RFID_ADDR);
MFRC522::MIFARE_Key key;

// MIFARE Classic 1K data blocks. Never write sector trailers (7, 11).
const byte DATA_BLOCKS[] = {4, 5, 6, 8, 9, 10};
const int  DATA_BLOCK_COUNT = sizeof(DATA_BLOCKS) / sizeof(DATA_BLOCKS[0]);
const int  MAX_PAYLOAD_LEN = DATA_BLOCK_COUNT * 16;   // 96 bytes

// ------------------------- State -------------------------
String currentJobId, currentOrderNumber, currentPayload, lastCardUid;
bool rfidReady = false, serverConnected = false, cardPresent = false;
unsigned long lastPollMs = 0, lastStatusMs = 0;

bool isOnline() {
  return strcmp(WIFI_SSID, "YOUR_WIFI_SSID") != 0;
}

// ------------------------- RFID helpers -------------------------
String readUidString() {
  String uid;
  for (byte i = 0; i < mfrc522.uid.size; i++) {
    if (mfrc522.uid.uidByte[i] < 0x10) uid += "0";
    uid += String(mfrc522.uid.uidByte[i], HEX);
    if (i + 1 < mfrc522.uid.size) uid += " ";
  }
  uid.toUpperCase();
  return uid;
}

bool authenticateBlock(byte blockAddr) {
  return mfrc522.PCD_Authenticate(MFRC522::PICC_CMD_MF_AUTH_KEY_A, blockAddr, &key, &(mfrc522.uid))
         == MFRC522::STATUS_OK;
}

bool writePayloadToCard(const String& payload) {
  if (payload.length() > MAX_PAYLOAD_LEN) {
    Serial.printf("Payload too long: %d > %d bytes\n", payload.length(), MAX_PAYLOAD_LEN);
    return false;
  }
  for (int i = 0; i < DATA_BLOCK_COUNT; i++) {
    byte buffer[16];
    memset(buffer, 0, 16);
    int start = i * 16;
    for (int j = 0; j < 16; j++) {
      int idx = start + j;
      if (idx < (int)payload.length()) buffer[j] = payload[idx];
    }
    byte block = DATA_BLOCKS[i];
    if (!authenticateBlock(block)) { Serial.printf("Auth failed on block %d\n", block); return false; }
    if (mfrc522.MIFARE_Write(block, buffer, 16) != MFRC522::STATUS_OK) {
      Serial.printf("Write failed on block %d\n", block);
      return false;
    }
  }
  return true;
}

bool readPayloadFromCard(String& out) {
  out = "";
  for (int i = 0; i < DATA_BLOCK_COUNT; i++) {
    byte block = DATA_BLOCKS[i];
    if (!authenticateBlock(block)) return false;
    byte buffer[18];
    byte size = sizeof(buffer);
    if (mfrc522.MIFARE_Read(block, buffer, &size) != MFRC522::STATUS_OK) return false;
    for (int j = 0; j < 16; j++) {
      char c = (char)buffer[j];
      if (c == '\0') return true;   // payload is null-padded; stop at first null
      out += c;
    }
  }
  return true;
}

bool waitForCard(unsigned long timeoutMs) {
  unsigned long start = millis();
  while (millis() - start < timeoutMs) {
    if (mfrc522.PICC_IsNewCardPresent() && mfrc522.PICC_ReadCardSerial()) return true;
    delay(80);
  }
  return false;
}

void endCardSession() {
  mfrc522.PICC_HaltA();
  mfrc522.PCD_StopCrypto1();
}

// Write + read-back verify. Returns true on verified write.
bool writeAndVerify(const String& payload) {
  Serial.println("Place a card on the reader...");
  if (!waitForCard(15000)) { Serial.println("No card detected (timeout)."); return false; }

  lastCardUid = readUidString();
  Serial.print("Card UID: "); Serial.println(lastCardUid);

  digitalWrite(LED_BUILTIN, HIGH);
  bool ok = writePayloadToCard(payload);
  digitalWrite(LED_BUILTIN, LOW);

  if (ok) {
    String back;
    if (readPayloadFromCard(back) && back == payload) {
      Serial.println("WRITE OK (verified).");
    } else {
      Serial.print("WRITE done but verify mismatch. Read back: "); Serial.println(back);
      ok = false;
    }
  } else {
    Serial.println("WRITE FAILED. Check card type (MIFARE Classic 1K) and key.");
  }
  endCardSession();
  return ok;
}

// ------------------------- Serial command interface -------------------------
void handleReadCommand() {
  Serial.println("Place a card to read...");
  if (!waitForCard(15000)) { Serial.println("No card detected (timeout)."); return; }
  lastCardUid = readUidString();
  Serial.print("Card UID: "); Serial.println(lastCardUid);
  String payload;
  if (readPayloadFromCard(payload)) {
    Serial.print("Payload: "); Serial.println(payload.length() ? payload : "(empty)");
  } else {
    Serial.println("Read failed (auth/key/card type).");
  }
  endCardSession();
}

void handleSerialLine(String line) {
  line.trim();
  if (line.length() == 0) return;

  if (line.equalsIgnoreCase("PING")) {
    Serial.printf("PONG rfid=%s online=%s server=%s uid=%s\n",
                  rfidReady ? "ready" : "down",
                  isOnline() ? "yes" : "no",
                  serverConnected ? "up" : "down",
                  lastCardUid.length() ? lastCardUid.c_str() : "-");
  } else if (line.equalsIgnoreCase("READ")) {
    handleReadCommand();
  } else if (line.startsWith("WRITE ")) {
    String payload = line.substring(6);
    payload.trim();
    if (payload.length() == 0) { Serial.println("Usage: WRITE {json}"); return; }
    writeAndVerify(payload);
  } else {
    Serial.println("Commands: PING | READ | WRITE {json}");
  }
}

void pumpSerial() {
  static String buf;
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\n' || c == '\r') {
      if (buf.length()) { handleSerialLine(buf); buf = ""; }
    } else {
      buf += c;
      if (buf.length() > 200) buf = "";   // guard against runaway input
    }
  }
}

// ------------------------- Backend (online mode) -------------------------
String jsonValue(const String& json, const String& k) {
  String needle = "\"" + k + "\":";
  int p = json.indexOf(needle);
  if (p < 0) return "";
  p += needle.length();
  while (p < (int)json.length() && (json[p] == ' ' || json[p] == '\t')) p++;
  if (p >= (int)json.length()) return "";
  if (json[p] == '"') {
    int start = p + 1;
    int end = json.indexOf('"', start);
    if (end < 0) return "";
    return json.substring(start, end);
  }
  int end = json.indexOf(',', p);
  if (end < 0) end = json.indexOf('}', p);
  if (end < 0) end = json.length();
  return json.substring(p, end);
}

String httpRequest(const String& method, const String& path, const String& body = "") {
  if (WiFi.status() != WL_CONNECTED) return "";
  HTTPClient http;
  http.begin(String(API_BASE) + path);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("x-device-id", DEVICE_ID);
  http.addHeader("x-api-key", API_KEY);
  int code = (method == "GET") ? http.GET() : http.POST(body);
  String response = code > 0 ? http.getString() : "";
  http.end();
  serverConnected = (code >= 200 && code < 300);
  return response;
}

void sendStatus(const String& message) {
  if (!isOnline()) return;
  String body = "{";
  body += "\"rfid_ready\":" + String(rfidReady ? "true" : "false") + ",";
  body += "\"card_present\":" + String(cardPresent ? "true" : "false") + ",";
  body += "\"last_card_uid\":\"" + lastCardUid + "\",";
  body += "\"current_job_id\":\"" + currentJobId + "\",";
  body += "\"message\":\"" + message + "\"}";
  httpRequest("POST", "/api/rfid-writer/status", body);
}

void reportJobResult(bool success, const String& message) {
  String body = "{";
  body += "\"job_id\":\"" + currentJobId + "\",";
  body += "\"success\":" + String(success ? "true" : "false") + ",";
  body += "\"rfid_card_uid\":\"" + lastCardUid + "\",";
  body += "\"message\":\"" + message + "\"}";
  httpRequest("POST", "/api/rfid-writer/job-result", body);
}

void pollJob() {
  if (currentJobId.length() > 0) return;
  String res = httpRequest("GET", "/api/rfid-writer/next-job");
  if (res.length() == 0) return;
  if (jsonValue(res, "has_job") != "true") return;
  currentJobId = jsonValue(res, "job_id");
  String userName = jsonValue(res, "user_name");
  currentOrderNumber = jsonValue(res, "order_number");
  // Compose the exact payload locally (server stores escaped JSON in CSV).
  currentPayload = "{\"user_name\":\"" + userName + "\",\"order_number\":\"" +
                   currentOrderNumber + "\",\"v\":1}";
  if (currentJobId.length() == 0 || userName.length() == 0 || currentOrderNumber.length() == 0) {
    currentJobId = ""; currentOrderNumber = ""; currentPayload = "";
    return;
  }
  Serial.printf("Job %s for %s (%s). Present a card to write.\n",
                currentJobId.c_str(), userName.c_str(), currentOrderNumber.c_str());
}

bool connectWifi() {
  Serial.printf("Connecting to Wi-Fi '%s'...\n", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) delay(500);
  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("Wi-Fi OK, IP "); Serial.println(WiFi.localIP());
    return true;
  }
  Serial.println("Wi-Fi connect failed (serial mode still works).");
  return false;
}

void onlineLoop() {
  if (WiFi.status() != WL_CONNECTED) { connectWifi(); return; }

  if (millis() - lastPollMs > 2000) { lastPollMs = millis(); pollJob(); }

  cardPresent = false;
  if (mfrc522.PICC_IsNewCardPresent() && mfrc522.PICC_ReadCardSerial()) {
    cardPresent = true;
    lastCardUid = readUidString();
    if (currentJobId.length() > 0 && currentPayload.length() > 0) {
      digitalWrite(LED_BUILTIN, HIGH);
      bool ok = writePayloadToCard(currentPayload);
      String back;
      if (ok && (!readPayloadFromCard(back) || back != currentPayload)) ok = false;
      digitalWrite(LED_BUILTIN, LOW);
      reportJobResult(ok, ok ? "RFID card written successfully" : "RFID write/verify failed");
      Serial.printf("Job %s -> %s\n", currentJobId.c_str(), ok ? "WRITTEN" : "FAILED");
      currentJobId = ""; currentOrderNumber = ""; currentPayload = "";
    }
    endCardSession();
    delay(1000);
  }

  if (millis() - lastStatusMs > 3000) {
    lastStatusMs = millis();
    sendStatus(currentJobId.length() ? "Waiting for card to write job" : "Idle. Waiting for server job");
  }
}

// ------------------------- Arduino entry points -------------------------
void setup() {
  Serial.begin(115200);
  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, LOW);
  delay(500);
  Serial.println();
  Serial.println("=== XIAO ESP32C6 RFID Writer ===");

  Wire.begin(I2C_SDA, I2C_SCL);
  Wire.setClock(100000);
  mfrc522.PCD_Init();
  for (byte i = 0; i < 6; i++) key.keyByte[i] = 0xFF;   // factory default key

  byte version = mfrc522.PCD_ReadRegister(MFRC522::VersionReg);
  rfidReady = (version == 0x91 || version == 0x92);
  Serial.printf("MFRC522 firmware: 0x%02X (%s)\n", version, rfidReady ? "OK" : "NOT RESPONDING");
  if (rfidReady) digitalWrite(LED_BUILTIN, HIGH);

  if (isOnline()) {
    connectWifi();
    sendStatus("XIAO RFID writer booted");
  } else {
    Serial.println("Serial mode (no Wi-Fi configured).");
  }
  Serial.println("Commands over serial @115200: PING | READ | WRITE {json}");
}

void loop() {
  pumpSerial();
  if (isOnline()) onlineLoop();
  delay(20);
}
