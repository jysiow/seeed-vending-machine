/*
  Wio Terminal RFID Writer — card payload v2

  Writes JSON to MIFARE blocks 4,5,6,8,9,10:
  {"v":2,"uid":"..","usr":"..","ord":null,"st":null,"bal":0}

  Job types from backend:
  - full_write
  - mark_collected (read-modify-write, set st=collected)
  - mark_partial   (read-modify-write, set st=partial)
*/

#include <Arduino.h>
#include <Wire.h>
#include <TFT_eSPI.h>
#include <rpcWiFi.h>
#include <HTTPClient.h>
#include "Emakefun_RFID.h"

const char* WIFI_SSID = "SEEED-MKT";
const char* WIFI_PASSWORD = "edgemaker2023";
const char* API_BASE = "http://192.168.7.176:3001";
const char* DEVICE_ID = "wio-rfid-writer";
const char* API_KEY = "WIO_WRITER_SECRET";

#define RFID_ADDR 0x28
MFRC522 mfrc522(RFID_ADDR);
MFRC522::MIFARE_Key key;

const byte DATA_BLOCKS[] = {4, 5, 6, 8, 9, 10};
const int DATA_BLOCK_COUNT = sizeof(DATA_BLOCKS) / sizeof(DATA_BLOCKS[0]);
const int MAX_PAYLOAD_LEN = DATA_BLOCK_COUNT * 16;

TFT_eSPI tft;
String currentJobId = "";
String currentJobType = "";
String currentPayload = "";
String lastCardUid = "";
bool rfidReady = false;
bool serverConnected = false;
bool cardPresent = false;
unsigned long lastPollMs = 0;
unsigned long lastStatusMs = 0;

String jsonValue(const String& json, const String& key) {
  String needle = "\"" + key + "\":";
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
  if (json.substring(p, p + 4) == "null") return "";
  int end = json.indexOf(',', p);
  if (end < 0) end = json.indexOf('}', p);
  if (end < 0) end = json.length();
  String val = json.substring(p, end);
  val.trim();
  return val;
}

String jsonFieldOrNull(const String& value) {
  if (value.length() == 0) return "null";
  return "\"" + value + "\"";
}

String buildPayloadV2(
  const String& cardUid,
  const String& userName,
  const String& orderNumber,
  const String& collectionStatus,
  const String& balance
) {
  String payload = "{";
  payload += "\"v\":2,";
  payload += "\"uid\":" + jsonFieldOrNull(cardUid) + ",";
  payload += "\"usr\":" + jsonFieldOrNull(userName) + ",";
  payload += "\"ord\":" + jsonFieldOrNull(orderNumber) + ",";
  payload += "\"st\":" + jsonFieldOrNull(collectionStatus) + ",";
  payload += "\"bal\":" + (balance.length() ? balance : "0");
  payload += "}";
  return payload;
}

void drawDashboard(const String& message) {
  tft.fillScreen(TFT_BLACK);
  tft.setTextSize(2);
  tft.setTextColor(TFT_GREEN, TFT_BLACK);
  tft.drawString("RFID WRITER", 10, 8);
  tft.setTextSize(1);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString(String("WiFi: ") + (WiFi.status() == WL_CONNECTED ? "OK" : "NO"), 10, 42);
  tft.drawString(String("Server: ") + (serverConnected ? "OK" : "NO"), 10, 62);
  tft.drawString(String("RFID: ") + (rfidReady ? "READY" : "NOT READY"), 10, 82);
  tft.drawString(String("Card: ") + (cardPresent ? "PRESENT" : "WAITING"), 10, 102);
  tft.drawString(String("UID: ") + (lastCardUid.length() ? lastCardUid : "-"), 10, 122);
  tft.drawString(String("Job: ") + (currentJobId.length() ? currentJobId : "-"), 10, 142);
  tft.setTextColor(TFT_YELLOW, TFT_BLACK);
  tft.drawString(message.substring(0, 38), 10, 200);
}

bool connectWifi() {
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  drawDashboard("Connecting WiFi...");
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
    delay(500);
  }
  return WiFi.status() == WL_CONNECTED;
}

String httpRequest(const String& method, const String& path, const String& body = "") {
  if (WiFi.status() != WL_CONNECTED) return "";
  HTTPClient http;
  String url = String(API_BASE) + path;
  http.begin(url);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("x-device-id", DEVICE_ID);
  http.addHeader("x-api-key", API_KEY);
  int code = 0;
  if (method == "GET") code = http.GET();
  else code = http.POST(body);
  String response = code > 0 ? http.getString() : "";
  http.end();
  serverConnected = (code >= 200 && code < 300);
  return response;
}

void sendStatus(const String& message) {
  String body = "{";
  body += "\"rfid_ready\":" + String(rfidReady ? "true" : "false") + ",";
  body += "\"card_present\":" + String(cardPresent ? "true" : "false") + ",";
  body += "\"last_card_uid\":\"" + lastCardUid + "\",";
  body += "\"current_job_id\":\"" + currentJobId + "\",";
  body += "\"message\":\"" + message + "\"";
  body += "}";
  httpRequest("POST", "/api/rfid-writer/status", body);
}

String readUidString() {
  String uid = "";
  for (byte i = 0; i < mfrc522.uid.size; i++) {
    if (mfrc522.uid.uidByte[i] < 0x10) uid += "0";
    uid += String(mfrc522.uid.uidByte[i], HEX);
    if (i + 1 < mfrc522.uid.size) uid += " ";
  }
  uid.toUpperCase();
  return uid;
}

bool authenticateBlock(byte blockAddr) {
  return mfrc522.PCD_Authenticate(MFRC522::PICC_CMD_MF_AUTH_KEY_A, blockAddr, &key, &(mfrc522.uid)) == MFRC522::STATUS_OK;
}

bool readPayloadFromCard(String& out) {
  out = "";
  for (int i = 0; i < DATA_BLOCK_COUNT; i++) {
    byte buffer[18];
    byte size = sizeof(buffer);
    byte block = DATA_BLOCKS[i];
    if (!authenticateBlock(block)) return false;
    if (mfrc522.MIFARE_Read(block, buffer, &size) != MFRC522::STATUS_OK) return false;
    for (int j = 0; j < 16; j++) {
      if (buffer[j] != 0) out += (char)buffer[j];
    }
  }
  out.trim();
  return out.length() > 0 && out.startsWith("{");
}

bool writePayloadToCard(const String& payload) {
  if (payload.length() > MAX_PAYLOAD_LEN) return false;
  for (int i = 0; i < DATA_BLOCK_COUNT; i++) {
    byte buffer[16];
    memset(buffer, 0, 16);
    int start = i * 16;
    for (int j = 0; j < 16; j++) {
      int idx = start + j;
      if (idx < (int)payload.length()) buffer[j] = payload[idx];
    }
    byte block = DATA_BLOCKS[i];
    if (!authenticateBlock(block)) return false;
    if (mfrc522.MIFARE_Write(block, buffer, 16) != MFRC522::STATUS_OK) return false;
  }
  return true;
}

void reportJobResult(bool success, const String& message) {
  String body = "{";
  body += "\"job_id\":\"" + currentJobId + "\",";
  body += "\"success\":" + String(success ? "true" : "false") + ",";
  body += "\"rfid_card_uid\":\"" + lastCardUid + "\",";
  body += "\"message\":\"" + message + "\"";
  body += "}";
  httpRequest("POST", "/api/rfid-writer/job-result", body);
}

void clearCurrentJob() {
  currentJobId = "";
  currentJobType = "";
  currentPayload = "";
}

void pollJob() {
  if (currentJobId.length() > 0) return;
  String res = httpRequest("GET", "/api/rfid-writer/next-job");
  if (res.length() == 0) return;
  if (jsonValue(res, "has_job") != "true") return;

  currentJobId = jsonValue(res, "job_id");
  currentJobType = jsonValue(res, "job_type");
  String cardUid = jsonValue(res, "card_uid");
  String userName = jsonValue(res, "user_name");
  String orderNumber = jsonValue(res, "order_number");
  String collectionStatus = jsonValue(res, "collection_status");
  String balance = jsonValue(res, "balance");

  if (currentJobType == "full_write") {
    currentPayload = buildPayloadV2(cardUid, userName, orderNumber, collectionStatus, balance);
  } else {
    currentPayload = "";
  }

  if (currentJobId.length() == 0) {
    clearCurrentJob();
  }
}

bool executeJobOnCard() {
  if (currentJobType == "full_write") {
    return writePayloadToCard(currentPayload);
  }

  String existing = "";
  if (!readPayloadFromCard(existing)) return false;

  String cardUid = jsonValue(existing, "uid");
  String userName = jsonValue(existing, "usr");
  String orderNumber = jsonValue(existing, "ord");
  String balance = jsonValue(existing, "bal");
  if (balance.length() == 0) balance = "0";

  String newStatus = "";
  if (currentJobType == "mark_collected") newStatus = "collected";
  else if (currentJobType == "mark_partial") newStatus = "partial";
  else return false;

  String payload = buildPayloadV2(cardUid, userName, orderNumber, newStatus, balance);
  return writePayloadToCard(payload);
}

void setup() {
  Serial.begin(115200);
  tft.begin();
  tft.setRotation(3);
  drawDashboard("Booting...");
  Wire.begin();
  mfrc522.PCD_Init();
  for (byte i = 0; i < 6; i++) key.keyByte[i] = 0xFF;
  rfidReady = true;
  connectWifi();
  sendStatus("Wio RFID writer booted");
  drawDashboard("Ready. Waiting for job/card.");
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) connectWifi();

  if (millis() - lastPollMs > 2000) {
    lastPollMs = millis();
    pollJob();
  }

  cardPresent = false;
  if (mfrc522.PICC_IsNewCardPresent() && mfrc522.PICC_ReadCardSerial()) {
    cardPresent = true;
    lastCardUid = readUidString();
    drawDashboard("Card detected.");

    if (currentJobId.length() > 0) {
      drawDashboard("Writing card...");
      bool ok = executeJobOnCard();
      if (ok) {
        reportJobResult(true, "RFID card written successfully");
        drawDashboard("Write OK. Remove card.");
      } else {
        reportJobResult(false, "RFID write failed");
        drawDashboard("Write failed.");
      }
      clearCurrentJob();
    }

    mfrc522.PICC_HaltA();
    mfrc522.PCD_StopCrypto1();
    delay(1200);
  }

  if (millis() - lastStatusMs > 3000) {
    lastStatusMs = millis();
    sendStatus(currentJobId.length() ? "Waiting for card to write job" : "Idle. Waiting for server job");
    drawDashboard(currentJobId.length() ? "Place card to write." : "Ready. Create job on server.");
  }
  delay(100);
}
