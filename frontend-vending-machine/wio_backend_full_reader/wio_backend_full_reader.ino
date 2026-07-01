#include <Wire.h>
#include <TFT_eSPI.h>
#include "Emakefun_RFID.h"
#include "SCServo.h"
#include <rpcWiFi.h>
#include <HTTPClient.h>

// =====================================================
// WIO VENDING MACHINE READER FOR backend-full
// =====================================================
// This sketch is for the uploaded backend-full project.
// It reads the JSON payload written by the backend-full RFID writer:
// {"user_name":"...","order_number":"...","v":1}
//
// The writer stores the JSON across MIFARE blocks 4,5,6,8,9,10.
// This reader reads those same blocks, sends user_name + order_number
// to /api/frontend/verify-card, dispenses if approved, then reports
// /api/frontend/dispense-complete.
//
// This backend-full flow is prepaid-order redemption.
// Cards without a written order payload are rejected.
// =====================================================

// ---------------- WIFI / BACKEND ----------------
const char* WIFI_SSID = "SEEED-MKT";
const char* WIFI_PASSWORD = "edgemaker2023";

// For Render, example:
// const char* BACKEND_BASE_URL = "https://your-app-name.onrender.com";
// For local same-WiFi testing, example:
// const char* BACKEND_BASE_URL = "http://192.168.1.23:3000";
const char* BACKEND_BASE_URL = "https://opulent-goldfish-7vjxpj9wv7c6vw-3000.app.github.dev";

const char* DEVICE_ID = "frontend-1";
const char* API_KEY = "FRONTEND_1_SECRET";

bool wifiReady = false;

// ---------------- LCD ----------------
TFT_eSPI tft;

// ---------------- RFID ----------------
#define RFID_ADDR 0x28
MFRC522 mfrc522(RFID_ADDR);
MFRC522::MIFARE_Key rfidKey;
bool rfidReady = false;

// These must match backend-full/wio-rfid-writer.
// Do not use sector trailer blocks 7 or 11.
const byte DATA_BLOCKS[] = {4, 5, 6, 8, 9, 10};
const int DATA_BLOCK_COUNT = sizeof(DATA_BLOCKS) / sizeof(DATA_BLOCKS[0]);
const int MAX_PAYLOAD_LEN = DATA_BLOCK_COUNT * 16;

// ---------------- SERVO ----------------
#define SERVO_NUM 4
SMS_STS st;
byte ID[SERVO_NUM] = {1, 2, 3, 4};

// ---------------- POSITIONS ----------------
const s16 HOME_POS = 1024;
const s16 TRIGGER_POS = 0;

// ---------------- SETTINGS ----------------
const int PRODUCT_COUNT = 4;
const int MAX_PRODUCT_QUANTITY = 10;

// ---------------- STATE MACHINE ----------------
enum State {
  WELCOME,
  AUTH,
  DISPENSE
};

State state = WELCOME;

// ---------------- SESSION ----------------
String currentCardUID = "";
String currentPayload = "";
String currentUserName = "";
String currentOrderNumber = "";
String currentProductName = "";
int approvedQuantities[PRODUCT_COUNT] = {0, 0, 0, 0};

// ---------------- DISPLAY ----------------
void displayMachineScreen(
  const char* title,
  const char* line1,
  const char* line2,
  const char* line3,
  uint16_t color
) {
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(color, TFT_BLACK);
  tft.setTextSize(2);
  tft.setCursor(20, 30);
  tft.println(title);

  tft.drawFastHLine(0, 70, 320, TFT_DARKGREY);

  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setCursor(20, 100);
  tft.println(line1);

  tft.setCursor(20, 140);
  tft.println(line2);

  tft.setCursor(20, 180);
  tft.println(line3);
}

void showWelcome() {
  if (!rfidReady) {
    displayMachineScreen(
      "Welcome to",
      "XIAO Vending Machine!",
      "Press A to init RFID,",
      "then scan for XIAO!",
      TFT_YELLOW
    );
  } else {
    displayMachineScreen(
      "Welcome to",
      "XIAO Vending Machine!",
      "Scan your order card",
      "to collect XIAO!",
      TFT_CYAN
    );
  }
}

void showWiFiConnecting() { displayMachineScreen("Connecting WiFi", "Please wait...", "", "", TFT_CYAN); }
void showWiFiFailed() { displayMachineScreen("WIFI ERROR", "Cannot connect WiFi", "See booth staff", "", TFT_RED); }
void showBackendChecking() { displayMachineScreen("Checking Backend", "Card detected", "Please wait...", "", TFT_CYAN); }
void showBackendError(const char* reason) { displayMachineScreen("BACKEND ERROR", reason, "See booth staff", "", TFT_RED); }
void showCardReadError() { displayMachineScreen("CARD ERROR", "No valid order", "See booth staff", "", TFT_RED); }

void showPrepaidOrderScreen() {
  char line1[32];
  snprintf(line1, sizeof(line1), "Order ready");
  displayMachineScreen("ORDER APPROVED", line1, "Dispensing now...", "", TFT_GREEN);
}

void showDispensing() { displayMachineScreen("Dispensing...", "", "", "", TFT_GREEN); }
void showDone() { displayMachineScreen("DISPENSE COMPLETE", "Enjoy your XIAO!", "", "", TFT_GREEN); }

// ---------------- HELPERS ----------------
void clearApprovedQuantities() {
  for (int i = 0; i < PRODUCT_COUNT; i++) {
    approvedQuantities[i] = 0;
  }
}

void returnToWelcome() {
  clearApprovedQuantities();
  currentCardUID = "";
  currentPayload = "";
  currentUserName = "";
  currentOrderNumber = "";
  currentProductName = "";
  state = AUTH;
  showWelcome();
}

String jsonValue(const String& json, const String& key) {
  String needle = "\"" + key + "\":";
  int p = json.indexOf(needle);
  if (p < 0) return "";

  p += needle.length();

  while (p < json.length() && (json[p] == ' ' || json[p] == '\t')) {
    p++;
  }

  if (p >= json.length()) return "";

  if (json[p] == '"') {
    int start = p + 1;
    int end = json.indexOf('"', start);
    if (end < 0) return "";
    return json.substring(start, end);
  }

  int end = json.indexOf(',', p);
  if (end < 0) end = json.indexOf('}', p);
  if (end < 0) end = json.length();

  String value = json.substring(p, end);
  value.trim();
  return value;
}

String jsonEscape(String value) {
  value.replace("\\", "\\\\");
  value.replace("\"", "\\\"");
  return value;
}

// ---------------- SERVO ----------------
void moveServo(byte id, s16 pos) {
  st.WritePosEx(id, pos, 1500, 50);
}

void dispenseProduct(int productId) {
  Serial.print("[DISPENSE] Product ");
  Serial.println(productId + 1);

  moveServo(productId + 1, TRIGGER_POS);
  delay(1500);
  moveServo(productId + 1, HOME_POS);
  delay(1000);
}

void dispenseApprovedQuantities() {
  showDispensing();
  delay(1000);

  for (int productId = 0; productId < PRODUCT_COUNT; productId++) {
    for (int i = 0; i < approvedQuantities[productId]; i++) {
      Serial.print("[DISPENSE] Product ");
      Serial.print(productId + 1);
      Serial.print(" cycle ");
      Serial.print(i + 1);
      Serial.print(" of ");
      Serial.println(approvedQuantities[productId]);

      dispenseProduct(productId);
      delay(500);
    }
  }

  showDone();
  delay(1500);
}

// ---------------- RFID ----------------
void stopRFID() {
  mfrc522.PICC_HaltA();
  mfrc522.PCD_StopCrypto1();
}

String getScannedUIDString() {
  String uidText = "";

  for (byte i = 0; i < mfrc522.uid.size; i++) {
    if (mfrc522.uid.uidByte[i] < 0x10) {
      uidText += "0";
    }

    uidText += String(mfrc522.uid.uidByte[i], HEX);

    if (i < mfrc522.uid.size - 1) {
      uidText += " ";
    }
  }

  uidText.toUpperCase();
  return uidText;
}

void printScannedUID() {
  Serial.print("[RFID] Scanned UID: ");
  Serial.println(getScannedUIDString());
  Serial.print("[RFID] UID length: ");
  Serial.println(mfrc522.uid.size);
}

bool authenticateBlock(byte blockAddr) {
  MFRC522::StatusCode status;
  status = (MFRC522::StatusCode)mfrc522.PCD_Authenticate(
    MFRC522::PICC_CMD_MF_AUTH_KEY_A,
    blockAddr,
    &rfidKey,
    &(mfrc522.uid)
  );

  if (status != MFRC522::STATUS_OK) {
    Serial.print("[RFID] Auth failed block ");
    Serial.print(blockAddr);
    Serial.print(": ");
    Serial.println(mfrc522.GetStatusCodeName(status));
    return false;
  }

  return true;
}

bool readPayloadFromCard(String &payload) {
  payload = "";

  for (int b = 0; b < DATA_BLOCK_COUNT; b++) {
    byte blockAddr = DATA_BLOCKS[b];

    if (!authenticateBlock(blockAddr)) {
      return false;
    }

    byte buffer[18];
    byte size = sizeof(buffer);
    MFRC522::StatusCode status = (MFRC522::StatusCode)mfrc522.MIFARE_Read(blockAddr, buffer, &size);

    if (status != MFRC522::STATUS_OK) {
      Serial.print("[RFID] Read failed block ");
      Serial.print(blockAddr);
      Serial.print(": ");
      Serial.println(mfrc522.GetStatusCodeName(status));
      return false;
    }

    for (int i = 0; i < 16; i++) {
      if (buffer[i] == 0) {
        continue;
      }
      payload += (char)buffer[i];
    }
  }

  payload.trim();

  Serial.print("[RFID] Payload from card: ");
  Serial.println(payload);

  return payload.length() > 0;
}

// ---------------- WIFI / HTTP ----------------
void connectWiFi() {
  if (WiFi.status() == WL_CONNECTED) {
    wifiReady = true;
    return;
  }

  showWiFiConnecting();

  Serial.print("[WIFI] Connecting to: ");
  Serial.println(WIFI_SSID);

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  int attempts = 0;

  while (WiFi.status() != WL_CONNECTED && attempts < 40) {
    delay(500);

    Serial.print("[WIFI] Attempt ");
    Serial.print(attempts + 1);
    Serial.print(" status=");
    Serial.println(WiFi.status());

    attempts++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    wifiReady = true;

    Serial.println("[WIFI] Connected");
    Serial.print("[WIFI] IP: ");
    Serial.println(WiFi.localIP());
  } else {
    wifiReady = false;

    Serial.println("[WIFI] Failed to connect");
    Serial.print("[WIFI] Final status=");
    Serial.println(WiFi.status());

    showWiFiFailed();
    delay(1500);
  }
}

bool ensureWiFi() {
  if (WiFi.status() == WL_CONNECTED) {
    wifiReady = true;
    return true;
  }

  connectWiFi();

  return WiFi.status() == WL_CONNECTED;
}

String postJsonToBackend(const char* path, String body, int &httpCode) {
  httpCode = -999;

  if (!ensureWiFi()) {
    return "";
  }

  HTTPClient http;
  String url = String(BACKEND_BASE_URL) + String(path);

  Serial.print("[HTTP] POST ");
  Serial.println(url);
  Serial.print("[HTTP] Body: ");
  Serial.println(body);

  http.begin(url);
  http.setTimeout(8000);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("x-device-id", DEVICE_ID);
  http.addHeader("x-api-key", API_KEY);

  httpCode = http.POST(body);

  String response = "";

  if (httpCode > 0) {
    response = http.getString();
  }

  http.end();
  response.trim();

  Serial.print("[HTTP] Code: ");
  Serial.println(httpCode);
  Serial.print("[HTTP] Response: ");
  Serial.println(response);

  return response;
}

bool reportDispenseComplete(bool success, const char* notes) {
  int code = 0;

  String body = "{";
  body += "\"order_number\":\"" + jsonEscape(currentOrderNumber) + "\",";
  body += "\"success\":" + String(success ? "true" : "false") + ",";
  body += "\"notes\":\"" + jsonEscape(String(notes)) + "\"";
  body += "}";

  String response = postJsonToBackend("/api/frontend/dispense-complete", body, code);

  return code >= 200 && code < 300 && response.indexOf("\"ok\":true") >= 0;
}

bool verifyCardWithBackend() {
  int code = 0;

  String body = "{";
  body += "\"user_name\":\"" + jsonEscape(currentUserName) + "\",";
  body += "\"order_number\":\"" + jsonEscape(currentOrderNumber) + "\",";
  body += "\"rfid_card_uid\":\"" + jsonEscape(currentCardUID) + "\"";
  body += "}";

  String response = postJsonToBackend("/api/frontend/verify-card", body, code);

  if (!(code >= 200 && code < 300)) {
    if (response.indexOf("already used") >= 0) {
      showBackendError("Already collected");
    } else if (response.indexOf("not found") >= 0) {
      showBackendError("Order not found");
    } else {
      showBackendError("Verify failed");
    }
    delay(1500);
    return false;
  }

  if (response.indexOf("\"allow_dispense\":true") < 0) {
    showBackendError("Not approved");
    delay(1500);
    return false;
  }

  String servoText = jsonValue(response, "servo_id");
  String quantityText = jsonValue(response, "quantity");
  currentProductName = jsonValue(response, "product_name");

  int servoId = servoText.toInt();
  int quantity = quantityText.toInt();

  if (servoId < 1 || servoId > PRODUCT_COUNT) {
    showBackendError("Bad servo id");
    delay(1500);
    return false;
  }

  if (quantity < 1 || quantity > MAX_PRODUCT_QUANTITY) {
    showBackendError("Bad quantity");
    delay(1500);
    return false;
  }

  clearApprovedQuantities();
  approvedQuantities[servoId - 1] = quantity;

  return true;
}

// ---------------- MAIN RFID SCAN ----------------
void scanRFIDCard() {
  if (!rfidReady || state != AUTH) return;

  if (!mfrc522.PICC_IsNewCardPresent()) return;
  if (!mfrc522.PICC_ReadCardSerial()) return;

  currentCardUID = getScannedUIDString();
  printScannedUID();
  showBackendChecking();

  String payload = "";
  bool readOk = readPayloadFromCard(payload);

  stopRFID();
  delay(300);

  if (!readOk || payload.length() == 0) {
    Serial.println("[RFID] No valid backend-full payload found on card");
    showCardReadError();
    delay(1500);
    returnToWelcome();
    return;
  }

  currentPayload = payload;
  currentUserName = jsonValue(payload, "user_name");
  currentOrderNumber = jsonValue(payload, "order_number");

  Serial.print("[RFID] user_name: ");
  Serial.println(currentUserName);
  Serial.print("[RFID] order_number: ");
  Serial.println(currentOrderNumber);

  if (currentUserName.length() == 0 || currentOrderNumber.length() == 0) {
    showCardReadError();
    delay(1500);
    returnToWelcome();
    return;
  }

  bool approved = verifyCardWithBackend();

  if (!approved) {
    returnToWelcome();
    return;
  }

  showPrepaidOrderScreen();
  delay(1000);

  dispenseApprovedQuantities();

  bool reportOk = reportDispenseComplete(true, "Wio dispense completed");

  if (!reportOk) {
    showBackendError("Report failed");
    delay(1500);
  }

  returnToWelcome();
}

void handleAuthInput() {
  if (!rfidReady) return;
  scanRFIDCard();
}

// ---------------- SETUP ----------------
void setup() {
  Serial.begin(115200);

  tft.init();
  tft.setRotation(3);
  tft.fillScreen(TFT_BLACK);

  Serial1.begin(1000000);
  st.pSerial = &Serial1;

  for (int i = 0; i < SERVO_NUM; i++) {
    moveServo(ID[i], HOME_POS);
  }

  pinMode(WIO_KEY_A, INPUT_PULLUP);
  pinMode(WIO_KEY_B, INPUT_PULLUP);
  pinMode(WIO_KEY_C, INPUT_PULLUP);

  for (byte i = 0; i < 6; i++) {
    rfidKey.keyByte[i] = 0xFF;
  }

  connectWiFi();
  showWelcome();
}

void initRFID() {
  Wire1.begin();
  mfrc522.PCD_Init();

  byte v = mfrc522.PCD_ReadRegister(mfrc522.VersionReg);

  Serial.print("[RFID] Version register: 0x");
  if (v < 0x10) Serial.print("0");
  Serial.println(v, HEX);

  if (v == 0x00 || v == 0xFF) {
    Serial.println("[RFID] Reader not responding");
    rfidReady = false;
    showWelcome();
  } else {
    Serial.println("[RFID] Reader detected and ready");
    mfrc522.PCD_AntennaOn();
    rfidReady = true;
    state = AUTH;
    showWelcome();
  }
}

void loop() {
  if (!rfidReady) {
    if (digitalRead(WIO_KEY_A) == LOW) {
      initRFID();
      delay(500);
    }
    return;
  }

  if (state == AUTH) {
    handleAuthInput();
    return;
  }
}
