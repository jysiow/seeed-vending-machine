#include <Wire.h>
#include <TFT_eSPI.h>
#include "Emakefun_RFID.h"
#include "SCServo.h"
#include <rpcWiFi.h>
#include <HTTPClient.h>

#if defined(__SAMD51__)
#include <sam.h>
#endif

// ---------------- WIFI / BACKEND ----------------
const char* WIFI_SSID = "SEEED-MKT";
const char* WIFI_PASSWORD = "edgemaker2023";

// Use your laptop IP on the same WiFi as the Wio Terminal.
// Do not add a trailing slash.
const char* BACKEND_BASE_URL = "http://192.168.7.176:3001";

bool wifiReady = false;

// ---------------- LCD ----------------
TFT_eSPI tft;

// ---------------- CART SETTINGS ----------------
const int PRODUCT_COUNT = 4;
const int MAX_PRODUCT_QUANTITY = 10;

// ---------------- RFID ----------------
#define RFID_ADDR 0x28
const byte DATA_BLOCKS[] = {4, 5, 6, 8, 9, 10};
const int DATA_BLOCK_COUNT = 6;
const int MAX_PAYLOAD_LEN = 96;
MFRC522 mfrc522(RFID_ADDR);
MFRC522::MIFARE_Key rfidKey;
bool rfidReady = false;

// ---------------- SESSION ----------------
String hardwareUid = "";
String currentCardUID = "";
String cardUserName = "";
String currentOrderNumber = "";
String collectionStatus = "";
float cardBalance = 0.0;
float currentBalance = 0.0;
float remainingBalance = 0.0;
bool cardPayloadValid = false;
bool forceDirectCheck = false;
int prepaidDispenseQty[PRODUCT_COUNT] = {0, 0, 0, 0};

// ---------------- CARD SCAN (non-blocking) ----------------
volatile bool cardScanInProgress = false;
unsigned long cardScanDeadlineMs = 0;
unsigned long cardScanLastAttemptMs = 0;
bool cardScanSawUnknown = false;
String cardScanLastResponse = "ERROR|HTTP";
const unsigned long CARD_SCAN_RETRY_MS = 500;
const unsigned long CARD_SCAN_TOTAL_MS = 10000;
volatile bool cardScanTimedOutByWatchdog = false;

bool initRFID();
bool reinitRFIDQuiet();
void reconnectWiFiQuiet();
void recoverAfterFailedCardScan();

#if defined(__SAMD51__)
volatile bool cardScanWatchdogActive = false;
volatile uint16_t cardScanWatchdogTicks = 0;
volatile uint8_t cardScanPostTimeoutPhase = 0;
volatile bool cardScanNeedsRecovery = false;
static bool cardScanWatchdogTimerReady = false;

void cardScanIsrShowUnreachable() {
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_RED, TFT_BLACK);
  tft.setTextSize(2);
  tft.setCursor(20, 30);
  tft.println("Backend Unreachable");
  tft.drawFastHLine(0, 70, 320, TFT_DARKGREY);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setCursor(20, 100);
  tft.println("Cannot reach server");
  tft.setCursor(20, 140);
  tft.println("Check WiFi/backend");
}

void cardScanIsrShowWelcome() {
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.setTextSize(2);
  tft.setCursor(20, 30);
  tft.println("Welcome to");
  tft.drawFastHLine(0, 70, 320, TFT_DARKGREY);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setCursor(20, 100);
  tft.println("XIAO Vending Machine!");
  tft.setCursor(20, 140);
  tft.println("Scan your card and");
  tft.setCursor(20, 180);
  tft.println("get your XIAO!");
}

void initCardScanWatchdogTimer() {
  if (cardScanWatchdogTimerReady) {
    return;
  }

  GCLK->PCHCTRL[TC3_GCLK_ID].reg = GCLK_PCHCTRL_CHEN | GCLK_PCHCTRL_GEN_GCLK0;
  while ((GCLK->PCHCTRL[TC3_GCLK_ID].reg & GCLK_PCHCTRL_CHEN) == 0) { }

  TC3->COUNT16.CTRLA.bit.ENABLE = 0;
  while (TC3->COUNT16.SYNCBUSY.bit.ENABLE);

  TC3->COUNT16.CTRLA.reg = TC_CTRLA_MODE_COUNT16 | TC_CTRLA_PRESCALER_DIV1024;
  TC3->COUNT16.WAVE.reg = TC_WAVE_WAVEGEN_MFRQ;
  TC3->COUNT16.CC[0].reg = 4688;
  TC3->COUNT16.INTFLAG.reg = TC_INTFLAG_MC0;
  TC3->COUNT16.INTENSET.reg = TC_INTENSET_MC0;
  NVIC_SetPriority(TC3_IRQn, 1);
  NVIC_EnableIRQ(TC3_IRQn);

  cardScanWatchdogTimerReady = true;
}

void startCardScanWatchdog() {
  initCardScanWatchdogTimer();
  cardScanWatchdogTicks = 0;
  cardScanPostTimeoutPhase = 0;
  cardScanWatchdogActive = true;

  TC3->COUNT16.CTRLA.bit.ENABLE = 0;
  while (TC3->COUNT16.SYNCBUSY.bit.ENABLE);
  TC3->COUNT16.COUNT.reg = 0;
  while (TC3->COUNT16.SYNCBUSY.bit.COUNT);
  TC3->COUNT16.INTFLAG.reg = TC_INTFLAG_MC0;
  TC3->COUNT16.CTRLA.bit.ENABLE = 1;
  while (TC3->COUNT16.SYNCBUSY.bit.ENABLE);
}

void stopCardScanWatchdog() {
  cardScanWatchdogActive = false;

  if (!cardScanWatchdogTimerReady) {
    return;
  }

  TC3->COUNT16.CTRLA.bit.ENABLE = 0;
  while (TC3->COUNT16.SYNCBUSY.bit.ENABLE);
}

void TC3_Handler() {
  if (!(TC3->COUNT16.INTFLAG.bit.MC0)) {
    return;
  }

  TC3->COUNT16.INTFLAG.reg = TC_INTFLAG_MC0;

  if (!cardScanWatchdogActive) {
    return;
  }

  cardScanWatchdogTicks++;

  if (cardScanWatchdogTicks == 100) {
    cardScanInProgress = false;
    cardScanTimedOutByWatchdog = true;
    cardScanPostTimeoutPhase = 1;
    cardScanIsrShowUnreachable();
  } else if (cardScanWatchdogTicks >= 112) {
    cardScanWatchdogActive = false;
    cardScanPostTimeoutPhase = 0;
    cardScanNeedsRecovery = true;
    cardScanIsrShowWelcome();
    TC3->COUNT16.CTRLA.bit.ENABLE = 0;
    while (TC3->COUNT16.SYNCBUSY.bit.ENABLE);
  }
}
#else
void startCardScanWatchdog() {
  cardScanDeadlineMs = millis() + CARD_SCAN_TOTAL_MS;
}

void stopCardScanWatchdog() {
}
#endif

// ---------------- SERVO ----------------
#define SERVO_NUM 4
SMS_STS st;

byte ID[SERVO_NUM] = {1, 2, 3, 4};
u16 Speed[SERVO_NUM] = {1500, 1500, 1500, 1500};
byte ACC[SERVO_NUM] = {50, 50, 50, 50};

// ---------------- POSITIONS ----------------
const s16 HOME_POS = 1024;
const s16 TRIGGER_POS = 0;

// ---------------- CART SETTINGS ----------------
int approvedQuantities[PRODUCT_COUNT] = {0, 0, 0, 0};

// ---------------- STATE MACHINE ----------------
enum State {
  WELCOME,
  AUTH,
  PRODUCT,
  CONFIRM,
  QUANTITY,
  QUANTITY_CONFIRM,
  MORE_PRODUCTS,
  CART_CONFIRM,
  CART_MODIFY,
  CART_EDIT_QUANTITY,
  DISPENSE
};

State state = WELCOME;

// ---------------- PRODUCT ----------------
int selectedProduct = 0;

// ---------------- QUANTITY ----------------
int selectedQuantity = 1;

// ---------------- CART ----------------
int cartQuantities[PRODUCT_COUNT] = {0, 0, 0, 0};
int selectedCartProduct = 0;
int editQuantity = 0;

// ---------------- CONFIRM OPTION ----------------
int confirmOption = 0; // 0 = YES, 1 = NO

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

// ---------------- WIFI ----------------
void showWiFiConnecting() {
  displayMachineScreen(
    "Connecting WiFi",
    WIFI_SSID,
    "Please wait...",
    "",
    TFT_CYAN
  );
}

void showWiFiFailed() {
  displayMachineScreen(
    "WIFI ERROR",
    "Cannot connect WiFi",
    "Check SSID/password",
    "",
    TFT_RED
  );
}

void showWiFiConnected() {
  char ipLine[24];
  snprintf(ipLine, sizeof(ipLine), "IP: %s", WiFi.localIP().toString().c_str());

  displayMachineScreen(
    "WiFi Connected",
    WIFI_SSID,
    ipLine,
    "",
    TFT_GREEN
  );
}

void showRFIDInitializing() {
  displayMachineScreen(
    "Initializing RFID",
    "Please wait...",
    "",
    "",
    TFT_CYAN
  );
}

void showRFIDFailed() {
  displayMachineScreen(
    "RFID ERROR",
    "Reader not responding",
    "Check Grove wiring",
    "",
    TFT_RED
  );
}

void showRFIDReady() {
  displayMachineScreen(
    "RFID Ready",
    "Reader initiated",
    "Sensor is ready",
    "",
    TFT_GREEN
  );
}

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

// ---------------- WELCOME ----------------
void showWelcome() {
  displayMachineScreen(
    "Welcome to",
    "XIAO Vending Machine!",
    "Scan your card and",
    "get your XIAO!",
    TFT_CYAN
  );
}

bool initSystem() {
  connectWiFi();
  if (!wifiReady) {
    return false;
  }

  showWiFiConnected();
  delay(2000);

  if (!initRFID()) {
    return false;
  }

  showRFIDReady();
  delay(2000);

  state = AUTH;
  showWelcome();
  return true;
}

// ---------------- AUTH ----------------
void showBackendChecking() {
  displayMachineScreen(
    "Checking Backend",
    "Card detected",
    "Please wait...",
    "",
    TFT_CYAN
  );
}

void showBackendError(const char* reason) {
  displayMachineScreen(
    "BACKEND ERROR",
    reason,
    "See booth staff",
    "",
    TFT_RED
  );
}

void showBackendUnreachable() {
  displayMachineScreen(
    "Backend Unreachable",
    "Cannot reach server",
    "Check WiFi/backend",
    "",
    TFT_RED
  );
}

void showPurchaseChecking() {
  displayMachineScreen(
    "Checking Purchase",
    "Please wait...",
    "",
    "",
    TFT_CYAN
  );
}

void showInsufficientBalance() {
  displayMachineScreen(
    "INSUFFICIENT",
    "Not enough balance",
    "See booth staff",
    "",
    TFT_RED
  );
}

void showOutOfStock() {
  displayMachineScreen(
    "OUT OF STOCK",
    "Cannot complete",
    "See booth staff",
    "",
    TFT_RED
  );
}

void showAuthSuccess(float balance) {
  char balanceLine[24];
  snprintf(balanceLine, sizeof(balanceLine), "Balance: $%.2f", balance);

  displayMachineScreen(
    "AUTHORISED CARD",
    "Authorised card detected",
    balanceLine,
    "Choose your XIAO",
    TFT_GREEN
  );
}

void showAuthFail() {
  displayMachineScreen(
    "UNAUTHORISED CARD",
    "No access",
    "",
    "",
    TFT_RED
  );
}

// ---------------- PRODUCT SCREEN ----------------
void showProductScreen() {
  displayMachineScreen(
    (selectedProduct == 0 ? "> [1] XIAO RP2040" : "[1] XIAO RP2040"),
    (selectedProduct == 1 ? "> [2] XIAO RP2350" : "[2] XIAO RP2350"),
    (selectedProduct == 2 ? "> [3] XIAO ESP326C" : "[3] XIAO ESP326C"),
    (selectedProduct == 3 ? "> [4] XIAO ESP32C3" : "[4] XIAO ESP32C3"),
    TFT_WHITE
  );
}

// ---------------- CONFIRM SCREEN ----------------
void showConfirmScreen() {
  const char* names[4] = {
    "XIAO RP2040",
    "XIAO RP2350",
    "XIAO ESP326C",
    "XIAO ESP32C3"
  };

  displayMachineScreen(
    names[selectedProduct],
    "Confirm?",
    "",
    (confirmOption == 0 ? "> YES    NO" : "  YES   > NO"),
    TFT_GREEN
  );
}

// ---------------- QUANTITY SCREEN ----------------
void showQuantityScreen() {
  const char* names[4] = {
    "XIAO RP2040",
    "XIAO RP2350",
    "XIAO ESP326C",
    "XIAO ESP32C3"
  };

  char line1[32];
  char line2[32];

  snprintf(line1, sizeof(line1), "Product: %s", names[selectedProduct]);
  snprintf(line2, sizeof(line2), "Quantity: %d", selectedQuantity);

  displayMachineScreen(
    "Choose Quantity",
    line1,
    line2,
    "UP/DOWN PRESS",
    TFT_CYAN
  );
}

// ---------------- QUANTITY CONFIRM SCREEN ----------------
void showQuantityConfirmScreen() {
  const char* names[4] = {
    "XIAO RP2040",
    "XIAO RP2350",
    "XIAO ESP326C",
    "XIAO ESP32C3"
  };

  char line1[32];
  char line2[32];

  snprintf(line1, sizeof(line1), "Product: %s", names[selectedProduct]);
  snprintf(line2, sizeof(line2), "Quantity: %d", selectedQuantity);

  displayMachineScreen(
    "Confirm Quantity",
    line1,
    line2,
    (confirmOption == 0 ? "> YES    NO" : "  YES   > NO"),
    TFT_GREEN
  );
}

// ---------------- CHOOSE MORE PRODUCTS SCREEN ----------------
void showMoreProductsScreen() {
  displayMachineScreen(
    "Do you want to",
    "add another XIAO?",
    "",
    (confirmOption == 0 ? "> YES    NO" : "  YES   > NO"),
    TFT_GREEN
  );
}

// ---------------- CONFIRM CART SCREEN ----------------
void showCartConfirmScreen() {
  const char* cartNames[4] = {
    "RP2040",
    "RP2350",
    "ESP326",
    "ESP32C3"
  };

  char line1[32] = "";
  char line2[32] = "";

  char items[4][16];
  int itemCount = 0;

  for (int i = 0; i < PRODUCT_COUNT; i++) {
    if (cartQuantities[i] > 0) {
      snprintf(items[itemCount], sizeof(items[itemCount]), "%s:%d", cartNames[i], cartQuantities[i]);
      itemCount++;
    }
  }

  if (itemCount == 0) {
    snprintf(line1, sizeof(line1), "Cart is empty");
    snprintf(line2, sizeof(line2), "");
  } else {
    if (itemCount >= 1) {
      snprintf(line1, sizeof(line1), "%s", items[0]);
    }

    if (itemCount >= 2) {
      strncat(line1, "  ", sizeof(line1) - strlen(line1) - 1);
      strncat(line1, items[1], sizeof(line1) - strlen(line1) - 1);
    }

    if (itemCount >= 3) {
      snprintf(line2, sizeof(line2), "%s", items[2]);
    }

    if (itemCount >= 4) {
      strncat(line2, "  ", sizeof(line2) - strlen(line2) - 1);
      strncat(line2, items[3], sizeof(line2) - strlen(line2) - 1);
    }
  }

  displayMachineScreen(
    "Confirm Cart",
    line1,
    line2,
    (confirmOption == 0 ? "> YES    NO" : "  YES   > NO"),
    TFT_GREEN
  );
}

// ---------------- MODIFY CART SCREEN ----------------
void showCartModifyScreen() {
  const char* shortNames[4] = {
    "RP2040",
    "RP2350",
    "ESP326C",
    "ESP32C3"
  };

  char line1[32] = "";
  char line2[32] = "";

  snprintf(line1, sizeof(line1), "> %s x%d", shortNames[selectedCartProduct], cartQuantities[selectedCartProduct]);
  snprintf(line2, sizeof(line2), "PRESS to edit");

  displayMachineScreen(
    "Modify Cart",
    line1,
    line2,
    "UP/DOWN select",
    TFT_CYAN
  );
}

// ---------------- EDIT QUANTITY SCREEN ----------------
void showCartEditQuantityScreen() {
  const char* shortNames[4] = {
    "RP2040",
    "RP2350",
    "ESP326C",
    "ESP32C3"
  };

  char line1[32];
  char line2[32];

  snprintf(line1, sizeof(line1), "%s", shortNames[selectedCartProduct]);
  snprintf(line2, sizeof(line2), "Quantity: %d", editQuantity);

  displayMachineScreen(
    "Edit Quantity",
    line1,
    line2,
    "UP/DOWN PRESS",
    TFT_CYAN
  );
}

// ---------------- DISPENSE ----------------
void showDispensing() {
  displayMachineScreen(
    "Dispensing...",
    "",
    "",
    "",
    TFT_GREEN
  );
}

void showDone() {
  displayMachineScreen(
    "DISPENSE COMPLETE",
    "Enjoy your XIAO!",
    "",
    "",
    TFT_GREEN
  );
}

// ---------------- CART HELPERS ----------------
bool isCartEmpty() {
  for (int i = 0; i < PRODUCT_COUNT; i++) {
    if (cartQuantities[i] > 0) {
      return false;
    }
  }

  return true;
}

void clearCart() {
  for (int i = 0; i < PRODUCT_COUNT; i++) {
    cartQuantities[i] = 0;
  }
}

void addSelectedProductToCart() {
  if (selectedQuantity <= 0) {
    return;
  }

  cartQuantities[selectedProduct] += selectedQuantity;

  if (cartQuantities[selectedProduct] > MAX_PRODUCT_QUANTITY) {
    cartQuantities[selectedProduct] = MAX_PRODUCT_QUANTITY;
  }
}

void selectFirstCartProduct() {
  for (int i = 0; i < PRODUCT_COUNT; i++) {
    if (cartQuantities[i] > 0) {
      selectedCartProduct = i;
      return;
    }
  }

  selectedCartProduct = 0;
}

void selectPreviousCartProduct() {
  for (int step = 1; step <= PRODUCT_COUNT; step++) {
    int index = selectedCartProduct - step;

    if (index < 0) {
      index += PRODUCT_COUNT;
    }

    if (cartQuantities[index] > 0) {
      selectedCartProduct = index;
      return;
    }
  }
}

void selectNextCartProduct() {
  for (int step = 1; step <= PRODUCT_COUNT; step++) {
    int index = (selectedCartProduct + step) % PRODUCT_COUNT;

    if (cartQuantities[index] > 0) {
      selectedCartProduct = index;
      return;
    }
  }
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

void dispenseCart() {
  for (int productId = 0; productId < PRODUCT_COUNT; productId++) {
    for (int i = 0; i < cartQuantities[productId]; i++) {
      Serial.print("[DISPENSE] Product ");
      Serial.print(productId + 1);
      Serial.print(" cycle ");
      Serial.print(i + 1);
      Serial.print(" of ");
      Serial.println(cartQuantities[productId]);

      dispenseProduct(productId);
      delay(500);
    }
  }
}

// ---------------- BACKEND / HTTP ----------------
String buildCardCheckBody();
void processCardCheckResponse(String response);

String urlEncode(String value) {
  String encoded = "";

  for (int i = 0; i < value.length(); i++) {
    char c = value.charAt(i);

    if (
      (c >= 'a' && c <= 'z') ||
      (c >= 'A' && c <= 'Z') ||
      (c >= '0' && c <= '9') ||
      c == '-' || c == '_' || c == '.' || c == '~'
    ) {
      encoded += c;
    } else if (c == ' ') {
      encoded += "%20";
    } else {
      char hex[4];
      snprintf(hex, sizeof(hex), "%%%02X", (unsigned char)c);
      encoded += hex;
    }
  }

  return encoded;
}

bool isWiFiConnectedQuick() {
  if (WiFi.status() == WL_CONNECTED) {
    wifiReady = true;
    return true;
  }

  wifiReady = false;
  return false;
}

bool parseBackendHost(IPAddress& ip, uint16_t& port) {
  String url = BACKEND_BASE_URL;

  if (!url.startsWith("http://")) {
    return false;
  }

  url.remove(0, 7);

  int slash = url.indexOf('/');
  if (slash >= 0) {
    url = url.substring(0, slash);
  }

  int colon = url.indexOf(':');
  String hostPart = colon >= 0 ? url.substring(0, colon) : url;
  port = colon >= 0 ? (uint16_t)url.substring(colon + 1).toInt() : 80;

  return ip.fromString(hostPart);
}

String extractApiLine(const String& raw) {
  int idx = raw.indexOf("DIRECT|");

  if (idx < 0) {
    idx = raw.indexOf("PREPAID_PARTIAL|");
  }

  if (idx < 0) {
    idx = raw.indexOf("PREPAID|");
  }

  if (idx < 0) {
    idx = raw.indexOf("ERROR|");
  }

  if (idx < 0) {
    return "ERROR|HTTP";
  }

  String line = raw.substring(idx);
  int end = line.indexOf('\r');

  if (end < 0) {
    end = line.indexOf('\n');
  }

  if (end > 0) {
    line = line.substring(0, end);
  }

  line.trim();
  return line;
}

String postCardCheckBounded(unsigned long deadlineMs) {
#if defined(__SAMD51__)
  if (cardScanWatchdogTicks >= 100 || cardScanPostTimeoutPhase > 0) {
    return "ERROR|HTTP";
  }
#endif

  if ((long)(millis() - deadlineMs) >= 0) {
    return "ERROR|HTTP";
  }

  if (!isWiFiConnectedQuick()) {
    return "ERROR|WIFI";
  }

  IPAddress ip;
  uint16_t port = 3000;

  if (!parseBackendHost(ip, port)) {
    return "ERROR|HTTP";
  }

  unsigned long remaining = deadlineMs - millis();

  if (remaining < 200) {
    return "ERROR|HTTP";
  }

  int32_t connectMs = (int32_t)min(500UL, remaining - 100);

  if (connectMs < 100) {
    return "ERROR|HTTP";
  }

  WiFiClient client;

  Serial.print("[HTTP] Connect ");
  Serial.print(ip);
  Serial.print(":");
  Serial.print(port);
  Serial.print(" timeout ");
  Serial.println(connectMs);

  if (!client.connect(ip, port, connectMs)) {
    client.stop();
    Serial.println("[HTTP] Connect failed");
    return "ERROR|HTTP";
  }

  String body = buildCardCheckBody();
  String hostHeader = ip.toString();

  client.print(
    String("POST /machine/card-check HTTP/1.1\r\n") +
    "Host: " + hostHeader + "\r\n" +
    "Content-Type: application/x-www-form-urlencoded\r\n" +
    "Connection: close\r\n" +
    "Content-Length: " + String(body.length()) + "\r\n\r\n" +
    body
  );

  String raw = "";
  unsigned long readUntil = min(deadlineMs, millis() + 1000);

  while ((long)(millis() - readUntil) < 0) {
    while (client.available()) {
      raw += (char)client.read();

      if (raw.indexOf("DIRECT|") >= 0 || raw.indexOf("PREPAID|") >= 0 || raw.indexOf("PREPAID_PARTIAL|") >= 0) {
        break;
      }

      if (raw.indexOf("ERROR|") >= 0) {
        break;
      }
    }

    if (raw.indexOf("DIRECT|") >= 0 || raw.indexOf("PREPAID|") >= 0 || raw.indexOf("PREPAID_PARTIAL|") >= 0) {
      break;
    }

    if (raw.indexOf("ERROR|") >= 0) {
      break;
    }

    if (!client.connected() && client.available() == 0) {
      break;
    }

    delay(10);
  }

  client.stop();

  if (raw.length() == 0) {
    Serial.println("[HTTP] No response body");
    return "ERROR|HTTP";
  }

  String apiLine = extractApiLine(raw);
  Serial.print("[HTTP] Response: ");
  Serial.println(apiLine);
  return apiLine;
}

String postToBackend(const char* path, String body, unsigned long deadlineMs = 0) {
  const bool hasDeadline = deadlineMs > 0;

  if (hasDeadline && millis() >= deadlineMs) {
    Serial.println("[HTTP] Deadline reached before request");
    return "ERROR|HTTP";
  }

  if (!isWiFiConnectedQuick()) {
    if (!hasDeadline) {
      if (!ensureWiFi()) {
        return "ERROR|WIFI";
      }
    } else {
      return "ERROR|WIFI";
    }
  }

  unsigned long remaining = hasDeadline ? (deadlineMs - millis()) : 8000UL;

  if (hasDeadline && remaining < 300) {
    Serial.println("[HTTP] Deadline too close to start request");
    return "ERROR|HTTP";
  }

  uint16_t connectTimeout = hasDeadline
    ? (uint16_t)min(1500UL, remaining)
    : 3000;
  uint16_t tcpTimeout = hasDeadline
    ? (uint16_t)min(1500UL, remaining)
    : 8000;

  HTTPClient http;
  String url = String(BACKEND_BASE_URL) + String(path);

  Serial.print("[HTTP] POST ");
  Serial.println(url);
  Serial.print("[HTTP] Body: ");
  Serial.println(body);
  Serial.print("[HTTP] Connect timeout ms: ");
  Serial.println(connectTimeout);

  http.begin(url);
  http.setConnectTimeout(connectTimeout);
  http.setTimeout(tcpTimeout);
  http.addHeader("Content-Type", "application/x-www-form-urlencoded");

  int code = http.POST(body);
  String response = "";

  if (code > 0) {
    response = http.getString();
  } else {
    response = "ERROR|HTTP";
  }

  http.end();
  response.trim();

  Serial.print("[HTTP] Code: ");
  Serial.println(code);
  Serial.print("[HTTP] Response: ");
  Serial.println(response);

  return response;
}

void clearApprovedQuantities() {
  for (int i = 0; i < PRODUCT_COUNT; i++) {
    approvedQuantities[i] = 0;
  }
}

bool parseQuantities(String text, int output[PRODUCT_COUNT]) {
  text.trim();
  int temp[PRODUCT_COUNT] = {0, 0, 0, 0};
  int index = 0;
  int start = 0;

  while (index < PRODUCT_COUNT) {
    int comma = text.indexOf(',', start);
    String part = comma == -1 ? text.substring(start) : text.substring(start, comma);
    part.trim();

    if (part.length() == 0) {
      return false;
    }

    temp[index] = part.toInt();

    if (temp[index] < 0 || temp[index] > MAX_PRODUCT_QUANTITY) {
      return false;
    }

    index++;

    if (comma == -1) {
      break;
    }

    start = comma + 1;
  }

  if (index != PRODUCT_COUNT) {
    return false;
  }

  for (int i = 0; i < PRODUCT_COUNT; i++) {
    output[i] = temp[i];
  }

  return true;
}

void copyApprovedToCart() {
  for (int i = 0; i < PRODUCT_COUNT; i++) {
    cartQuantities[i] = approvedQuantities[i];
  }
}

// ---------------- RFID STOP ----------------
void stopRFID() {
  mfrc522.PICC_HaltA();
  mfrc522.PCD_StopCrypto1();
}

void reconnectWiFiQuiet() {
  if (WiFi.status() == WL_CONNECTED) {
    wifiReady = true;
    return;
  }

  Serial.println("[WIFI] Reconnecting after scan abort...");
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 8000) {
    delay(250);
  }

  wifiReady = WiFi.status() == WL_CONNECTED;
  Serial.print("[WIFI] Reconnect ");
  Serial.println(wifiReady ? "OK" : "failed");
}

bool reinitRFIDQuiet() {
  stopRFID();
  mfrc522.PCD_AntennaOff();
  delay(100);

  Wire1.end();
  delay(50);
  Wire1.begin();
  delay(50);

  mfrc522.PCD_Init();

  byte v = mfrc522.PCD_ReadRegister(mfrc522.VersionReg);

  Serial.print("[RFID] Re-init version: 0x");
  if (v < 0x10) {
    Serial.print("0");
  }
  Serial.println(v, HEX);

  if (v == 0x00 || v == 0xFF) {
    Serial.println("[RFID] Re-init failed");
    rfidReady = false;
    return false;
  }

  mfrc522.PCD_AntennaOn();
  rfidReady = true;
  Serial.println("[RFID] Re-initialized, ready for next scan");
  return true;
}

void recoverAfterFailedCardScan() {
  stopCardScanWatchdog();
  cardScanInProgress = false;
  cardScanTimedOutByWatchdog = false;
  cardScanPostTimeoutPhase = 0;
  cardScanNeedsRecovery = false;
  cardScanSawUnknown = false;
  cardScanLastAttemptMs = 0;

  WiFi.disconnect();
  delay(100);

  reconnectWiFiQuiet();
  reinitRFIDQuiet();
  state = AUTH;
}

void printScannedUID() {
  Serial.print("[RFID] Scanned UID: ");
  Serial.println(getScannedUIDString());
  Serial.print("[RFID] UID length: ");
  Serial.println(mfrc522.uid.size);
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

String jsonFieldFromCard(const String& json, const String& key) {
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

String jsonFieldOrNullOut(const String& value) {
  if (value.length() == 0) return "null";
  return "\"" + value + "\"";
}

String buildCardPayloadV2(
  const String& cardUid,
  const String& userName,
  const String& orderNumber,
  const String& status,
  float balance
) {
  char balBuf[16];
  snprintf(balBuf, sizeof(balBuf), "%.2f", balance);

  String payload = "{";
  payload += "\"v\":2,";
  payload += "\"uid\":" + jsonFieldOrNullOut(cardUid) + ",";
  payload += "\"usr\":" + jsonFieldOrNullOut(userName) + ",";
  payload += "\"ord\":" + jsonFieldOrNullOut(orderNumber) + ",";
  payload += "\"st\":" + jsonFieldOrNullOut(status) + ",";
  payload += "\"bal\":" + String(balBuf);
  payload += "}";
  return payload;
}

bool authenticateDataBlock(byte blockAddr) {
  MFRC522::StatusCode status = (MFRC522::StatusCode)mfrc522.PCD_Authenticate(
    MFRC522::PICC_CMD_MF_AUTH_KEY_A,
    blockAddr,
    &rfidKey,
    &(mfrc522.uid)
  );
  return status == MFRC522::STATUS_OK;
}

bool readCardPayloadFromChip(String& payloadOut) {
  payloadOut = "";

  for (int i = 0; i < DATA_BLOCK_COUNT; i++) {
    byte buffer[18];
    byte size = sizeof(buffer);
    byte block = DATA_BLOCKS[i];

    if (!authenticateDataBlock(block)) {
      return false;
    }

    MFRC522::StatusCode status = (MFRC522::StatusCode)mfrc522.MIFARE_Read(block, buffer, &size);
    if (status != MFRC522::STATUS_OK) {
      return false;
    }

    for (int j = 0; j < 16; j++) {
      if (buffer[j] != 0) {
        payloadOut += (char)buffer[j];
      }
    }
  }

  payloadOut.trim();
  return payloadOut.length() > 0 && payloadOut.startsWith("{");
}

bool writeCardPayloadToChip(const String& payload) {
  if (payload.length() > MAX_PAYLOAD_LEN) {
    return false;
  }

  for (int i = 0; i < DATA_BLOCK_COUNT; i++) {
    byte buffer[16];
    memset(buffer, 0, 16);
    int start = i * 16;

    for (int j = 0; j < 16; j++) {
      int idx = start + j;
      if (idx < (int)payload.length()) {
        buffer[j] = payload[idx];
      }
    }

    byte block = DATA_BLOCKS[i];
    if (!authenticateDataBlock(block)) {
      return false;
    }

    MFRC522::StatusCode status = (MFRC522::StatusCode)mfrc522.MIFARE_Write(block, buffer, 16);
    if (status != MFRC522::STATUS_OK) {
      return false;
    }
  }

  return true;
}

bool parseCardPayload(const String& payload) {
  cardPayloadValid = false;
  cardUserName = "";
  currentOrderNumber = "";
  collectionStatus = "";
  cardBalance = 0.0;

  if (!payload.startsWith("{")) {
    return false;
  }

  String cardUidFromJson = jsonFieldFromCard(payload, "uid");
  cardUserName = jsonFieldFromCard(payload, "usr");
  currentOrderNumber = jsonFieldFromCard(payload, "ord");
  collectionStatus = jsonFieldFromCard(payload, "st");
  String balText = jsonFieldFromCard(payload, "bal");

  if (cardUidFromJson.length() > 0) {
    currentCardUID = cardUidFromJson;
  }

  cardBalance = balText.length() > 0 ? balText.toFloat() : 0.0;
  cardPayloadValid = (currentCardUID.length() > 0 && cardUserName.length() > 0);

  Serial.print("[RFID] Card payload uid=");
  Serial.print(currentCardUID);
  Serial.print(" usr=");
  Serial.print(cardUserName);
  Serial.print(" ord=");
  Serial.print(currentOrderNumber.length() ? currentOrderNumber : "(null)");
  Serial.print(" st=");
  Serial.print(collectionStatus.length() ? collectionStatus : "(null)");
  Serial.print(" bal=");
  Serial.println(cardBalance);

  return cardPayloadValid;
}

bool updateCardAfterRedeem(const String& newStatus) {
  String orderForCard = currentOrderNumber;
  String statusForCard = newStatus;

  if (newStatus == "collected") {
    orderForCard = "";
    statusForCard = "";
  }

  String payload = buildCardPayloadV2(
    currentCardUID,
    cardUserName,
    orderForCard,
    statusForCard,
    cardBalance
  );

  Serial.print("[RFID] Write-back payload: ");
  Serial.println(payload);
  return writeCardPayloadToChip(payload);
}

void dispensePrepaidQuantities(const int qty[PRODUCT_COUNT]) {
  for (int productId = 0; productId < PRODUCT_COUNT; productId++) {
    for (int i = 0; i < qty[productId]; i++) {
      dispenseProduct(productId);
      delay(500);
    }
  }
}

bool redeemPrepaidOrder(const int qty[PRODUCT_COUNT]) {
  String body = "card_uid=" + urlEncode(currentCardUID);
  body += "&user_name=" + urlEncode(cardUserName);
  body += "&order_number=" + urlEncode(currentOrderNumber);
  body += "&q1=" + String(qty[0]);
  body += "&q2=" + String(qty[1]);
  body += "&q3=" + String(qty[2]);
  body += "&q4=" + String(qty[3]);

  String response = postToBackend("/machine/redeem-order", body);
  if (!response.startsWith("APPROVED|")) {
    Serial.print("[HTTP] Redeem failed: ");
    Serial.println(response);
    return false;
  }

  int firstSep = response.indexOf('|');
  int secondSep = response.indexOf('|', firstSep + 1);
  if (secondSep < 0) {
    return false;
  }

  String newStatus = response.substring(firstSep + 1, secondSep);
  newStatus.trim();
  newStatus.toLowerCase();

  if (!updateCardAfterRedeem(newStatus)) {
    Serial.println("[RFID] Card write-back failed after redeem");
  }

  return true;
}

void handlePrepaidDispense(String response) {
  int firstSep = response.indexOf('|');
  int secondSep = response.indexOf('|', firstSep + 1);
  if (firstSep < 0 || secondSep < 0) {
    showBackendError("Bad prepaid response");
    delay(1200);
    state = AUTH;
    showWelcome();
    return;
  }

  currentOrderNumber = response.substring(firstSep + 1, secondSep);
  String qtyText = response.substring(secondSep + 1);

  for (int i = 0; i < PRODUCT_COUNT; i++) {
    prepaidDispenseQty[i] = 0;
  }

  if (!parseQuantities(qtyText, prepaidDispenseQty)) {
    showBackendError("Bad quantity");
    delay(1200);
    state = AUTH;
    showWelcome();
    return;
  }

  showDispensing();
  delay(800);
  dispensePrepaidQuantities(prepaidDispenseQty);

  if (!redeemPrepaidOrder(prepaidDispenseQty)) {
    showBackendError("Redeem failed");
    delay(1200);
    state = AUTH;
    showWelcome();
    return;
  }

  showDone();
  delay(1500);
  currentOrderNumber = "";
  collectionStatus = "";
  forceDirectCheck = false;
  state = AUTH;
  showWelcome();
}

void retryDirectAfterCollected() {
  forceDirectCheck = true;
  currentOrderNumber = "";
  collectionStatus = "";

  cardScanInProgress = true;
  cardScanSawUnknown = false;
  cardScanLastResponse = "ERROR|HTTP";
  cardScanTimedOutByWatchdog = false;
  cardScanDeadlineMs = millis() + CARD_SCAN_TOTAL_MS;
  cardScanLastAttemptMs = 0;

  stopCardScanWatchdog();
  startCardScanWatchdog();
  showBackendChecking();
}

bool writeCardBalanceAfterPurchase(float newBalance) {
  cardBalance = newBalance;
  String payload = buildCardPayloadV2(
    currentCardUID,
    cardUserName,
    currentOrderNumber,
    collectionStatus,
    cardBalance
  );
  return writeCardPayloadToChip(payload);
}

void processCardCheckResponse(String response) {
  if (response.startsWith("DIRECT|")) {
    String balanceText = response.substring(String("DIRECT|").length());
    currentBalance = balanceText.toFloat();
    cardBalance = currentBalance;
    forceDirectCheck = false;

    showAuthSuccess(currentBalance);
    delay(1200);

    clearCart();
    clearApprovedQuantities();

    state = PRODUCT;
    selectedProduct = 0;
    selectedQuantity = 1;
    confirmOption = 0;
    showProductScreen();
    return;
  }

  if (response.startsWith("PREPAID_PARTIAL|") || response.startsWith("PREPAID|")) {
    handlePrepaidDispense(response);
    return;
  }

  if (response.startsWith("ERROR|ALREADY_COLLECTED")) {
    showBackendError("Already collected");
    delay(1200);
    retryDirectAfterCollected();
    return;
  }

  if (response.startsWith("ERROR|UNAUTHORISED") || response.startsWith("ERROR|UNKNOWN_CARD")) {
    showAuthFail();
  } else if (response.startsWith("ERROR|ORDER_NOT_FOUND")) {
    showBackendError("Order not found");
  } else if (response.startsWith("ERROR|")) {
    showBackendError("Card check failed");
  } else if (response.startsWith("ERROR|WIFI") || response.startsWith("ERROR|HTTP")) {
    showBackendUnreachable();
  } else {
    showBackendError("Unknown response");
  }

  delay(1200);
  state = AUTH;
  showWelcome();
}

bool approveDirectPurchaseWithBackend() {
  showPurchaseChecking();

  String body = "card_uid=" + urlEncode(currentCardUID);
  body += "&user_name=" + urlEncode(cardUserName);
  body += "&balance=" + String(cardBalance, 2);
  body += "&q1=" + String(cartQuantities[0]);
  body += "&q2=" + String(cartQuantities[1]);
  body += "&q3=" + String(cartQuantities[2]);
  body += "&q4=" + String(cartQuantities[3]);

  String response = postToBackend("/machine/direct-purchase", body);

  if (response.startsWith("APPROVED|")) {
    int firstSep = response.indexOf('|');
    int secondSep = response.indexOf('|', firstSep + 1);

    if (secondSep == -1) {
      showBackendError("Bad response");
      delay(1200);
      state = CART_CONFIRM;
      showCartConfirmScreen();
      return false;
    }

    remainingBalance = response.substring(firstSep + 1, secondSep).toFloat();
    cardBalance = remainingBalance;
    String qtyText = response.substring(secondSep + 1);

    clearApprovedQuantities();

    if (!parseQuantities(qtyText, approvedQuantities)) {
      showBackendError("Bad quantity");
      delay(1200);
      state = CART_CONFIRM;
      showCartConfirmScreen();
      return false;
    }

    if (!writeCardBalanceAfterPurchase(remainingBalance)) {
      Serial.println("[RFID] Failed to write balance back to card");
    }

    return true;
  }

  if (response.startsWith("DENIED|INSUFFICIENT_BALANCE")) {
    showInsufficientBalance();
  } else if (response.startsWith("DENIED|OUT_OF_STOCK")) {
    showOutOfStock();
  } else if (response.startsWith("DENIED|UNAUTHORISED")) {
    showAuthFail();
  } else {
    showBackendError("Purchase denied");
  }

  delay(1200);
  state = CART_CONFIRM;
  showCartConfirmScreen();
  return false;
}

// ---------------- RFID CARD SCAN ----------------
String buildCardCheckBody() {
  String body = "hardware_uid=" + urlEncode(hardwareUid);
  body += "&card_uid=" + urlEncode(currentCardUID);
  body += "&user_name=" + urlEncode(cardUserName);
  body += "&balance=" + String(cardBalance, 2);

  if (!forceDirectCheck && currentOrderNumber.length() > 0) {
    body += "&order_number=" + urlEncode(currentOrderNumber);
  }

  if (!forceDirectCheck && collectionStatus.length() > 0) {
    body += "&collection_status=" + urlEncode(collectionStatus);
  }

  return body;
}

bool isBackendUnreachable(const String& response) {
  return response.startsWith("ERROR|WIFI") || response.startsWith("ERROR|HTTP");
}

void finishCardScanTimeout() {
  cardScanInProgress = false;
  cardScanTimedOutByWatchdog = false;

  if (cardScanSawUnknown && !isBackendUnreachable(cardScanLastResponse)) {
    Serial.println("[RFID] Unauthorised card after scan");
    showAuthFail();
    delay(1200);
  } else {
    Serial.println("[HTTP] Backend unreachable after 10 seconds");
    showBackendUnreachable();
    delay(1200);
  }

  recoverAfterFailedCardScan();
  showWelcome();
}

void pollCardScanBackend() {
  if (cardScanPostTimeoutPhase == 1) {
    return;
  }

  if (!cardScanInProgress) {
    return;
  }

#if defined(__SAMD51__)
  if (cardScanWatchdogTicks >= 100) {
    return;
  }
#endif

  if ((long)(millis() - cardScanDeadlineMs) >= 0) {
    finishCardScanTimeout();
    return;
  }

  if (millis() - cardScanLastAttemptMs < CARD_SCAN_RETRY_MS) {
    return;
  }

  cardScanLastAttemptMs = millis();
  cardScanLastResponse = postCardCheckBounded(cardScanDeadlineMs);

  if (cardScanTimedOutByWatchdog || !cardScanInProgress) {
    return;
  }

  if (cardScanPostTimeoutPhase == 1) {
    return;
  }

  if (
    cardScanLastResponse.startsWith("DIRECT|") ||
    cardScanLastResponse.startsWith("PREPAID|") ||
    cardScanLastResponse.startsWith("PREPAID_PARTIAL|")
  ) {
    stopCardScanWatchdog();
    cardScanInProgress = false;
    stopRFID();
    delay(300);
    processCardCheckResponse(cardScanLastResponse);
    return;
  }

  if (cardScanLastResponse.startsWith("ERROR|UNAUTHORISED")) {
    stopCardScanWatchdog();
    cardScanInProgress = false;
    stopRFID();
    delay(300);
    showAuthFail();
    delay(1200);
    recoverAfterFailedCardScan();
    showWelcome();
    return;
  }

  if (cardScanLastResponse.startsWith("ERROR|UNKNOWN_CARD")) {
    cardScanSawUnknown = true;
    return;
  }

  if (isBackendUnreachable(cardScanLastResponse)) {
    return;
  }

  stopCardScanWatchdog();
  cardScanInProgress = false;
  stopRFID();
  delay(300);
  processCardCheckResponse(cardScanLastResponse);
}

void scanRFIDCard() {
  if (!rfidReady) return;
  if (state != AUTH) return;

  if (cardScanPostTimeoutPhase == 1) {
    return;
  }

#if defined(__SAMD51__)
  if (cardScanNeedsRecovery) {
    recoverAfterFailedCardScan();
    showWelcome();
    return;
  }
#endif

  if (cardScanInProgress) {
    pollCardScanBackend();
    return;
  }

  if (!mfrc522.PICC_IsNewCardPresent()) {
    return;
  }

  if (!mfrc522.PICC_ReadCardSerial()) {
    return;
  }

  hardwareUid = getScannedUIDString();
  printScannedUID();

  String payload = "";
  if (!readCardPayloadFromChip(payload) || !parseCardPayload(payload)) {
    Serial.println("[RFID] Invalid or missing card JSON");
    showAuthFail();
    delay(1200);
    stopRFID();
    return;
  }

  forceDirectCheck = false;

  cardScanInProgress = true;
  cardScanSawUnknown = false;
  cardScanLastResponse = "ERROR|HTTP";
  cardScanTimedOutByWatchdog = false;
  cardScanDeadlineMs = millis() + CARD_SCAN_TOTAL_MS;
  cardScanLastAttemptMs = 0;

  stopCardScanWatchdog();
  startCardScanWatchdog();
  showBackendChecking();
}

// ---------------- AUTH INPUT ----------------
void handleAuthInput() {
  if (!rfidReady) return;

  scanRFIDCard();
}

// ---------------- JOYSTICK ----------------
void handleJoystick() {
  if (
    state != PRODUCT &&
    state != CONFIRM &&
    state != QUANTITY &&
    state != QUANTITY_CONFIRM &&
    state != MORE_PRODUCTS &&
    state != CART_CONFIRM &&
    state != CART_MODIFY &&
    state != CART_EDIT_QUANTITY
  ) return;

  if (state == PRODUCT) {

    if (digitalRead(WIO_5S_UP) == LOW) {
      selectedProduct = (selectedProduct + 3) % 4;
      showProductScreen();
      delay(200);
    }

    if (digitalRead(WIO_5S_DOWN) == LOW) {
      selectedProduct = (selectedProduct + 1) % 4;
      showProductScreen();
      delay(200);
    }

    if (digitalRead(WIO_5S_PRESS) == LOW) {
      delay(200);
      state = CONFIRM;
      confirmOption = 0;
      showConfirmScreen();
    }
  }

  if (state == QUANTITY) {

    if (digitalRead(WIO_5S_UP) == LOW) {
      selectedQuantity = min(selectedQuantity + 1, MAX_PRODUCT_QUANTITY);
      showQuantityScreen();
      delay(200);
    }

    if (digitalRead(WIO_5S_DOWN) == LOW) {
      selectedQuantity = max(selectedQuantity - 1, 0);
      showQuantityScreen();
      delay(200);
    }

    if (digitalRead(WIO_5S_PRESS) == LOW) {
      delay(200);
      state = QUANTITY_CONFIRM;
      confirmOption = 0;
      showQuantityConfirmScreen();
    }
  }

  if (state == QUANTITY_CONFIRM) {

    if (digitalRead(WIO_5S_LEFT) == LOW) {
      confirmOption = 0;
      showQuantityConfirmScreen();
      delay(200);
    }

    if (digitalRead(WIO_5S_RIGHT) == LOW) {
      confirmOption = 1;
      showQuantityConfirmScreen();
      delay(200);
    }

    if (digitalRead(WIO_5S_PRESS) == LOW) {
      delay(200);

      if (confirmOption == 0) {
        if (selectedQuantity == 0) {
          if (isCartEmpty()) {
            state = PRODUCT;
            selectedProduct = 0;
            selectedQuantity = 1;
            showProductScreen();
          } else {
            state = MORE_PRODUCTS;
            confirmOption = 0;
            showMoreProductsScreen();
          }
        } else {
          addSelectedProductToCart();

          state = MORE_PRODUCTS;
          confirmOption = 0;
          showMoreProductsScreen();
        }
      } else {
        state = QUANTITY;
        showQuantityScreen();
      }

      confirmOption = 0;
    }
  }

  if (state == MORE_PRODUCTS) {

    if (digitalRead(WIO_5S_LEFT) == LOW) {
      confirmOption = 0;
      showMoreProductsScreen();
      delay(200);
    }

    if (digitalRead(WIO_5S_RIGHT) == LOW) {
      confirmOption = 1;
      showMoreProductsScreen();
      delay(200);
    }

    if (digitalRead(WIO_5S_PRESS) == LOW) {
      delay(200);

      if (confirmOption == 0) {
        state = PRODUCT;
        selectedProduct = 0;
        selectedQuantity = 1;
        showProductScreen();
      } else {
        if (isCartEmpty()) {
          state = PRODUCT;
          selectedProduct = 0;
          selectedQuantity = 1;
          showProductScreen();
        } else {
          state = CART_CONFIRM;
          confirmOption = 0;
          showCartConfirmScreen();
        }
      }

      confirmOption = 0;
    }
  }

  if (state == CART_CONFIRM) {

    if (digitalRead(WIO_5S_LEFT) == LOW) {
      confirmOption = 0;
      showCartConfirmScreen();
      delay(200);
    }

    if (digitalRead(WIO_5S_RIGHT) == LOW) {
      confirmOption = 1;
      showCartConfirmScreen();
      delay(200);
    }

    if (digitalRead(WIO_5S_PRESS) == LOW) {
      delay(200);

      if (confirmOption == 0) {
        bool approved = approveDirectPurchaseWithBackend();

        if (approved) {
          copyApprovedToCart();
          state = DISPENSE;

          showDispensing();
          delay(1000);

          dispenseCart();

          showDone();
          delay(1500);

          clearCart();
          currentCardUID = "";
          currentOrderNumber = "";

          state = AUTH;
          showWelcome();
        }
      } else {
        if (isCartEmpty()) {
          state = PRODUCT;
          selectedProduct = 0;
          selectedQuantity = 1;
          showProductScreen();
        } else {
          selectFirstCartProduct();
          state = CART_MODIFY;
          showCartModifyScreen();
        }
      }

      confirmOption = 0;
    }
  }

  if (state == CART_MODIFY) {

    if (digitalRead(WIO_5S_UP) == LOW) {
      selectPreviousCartProduct();
      showCartModifyScreen();
      delay(200);
    }

    if (digitalRead(WIO_5S_DOWN) == LOW) {
      selectNextCartProduct();
      showCartModifyScreen();
      delay(200);
    }

    if (digitalRead(WIO_5S_PRESS) == LOW) {
      delay(200);
      editQuantity = cartQuantities[selectedCartProduct];
      state = CART_EDIT_QUANTITY;
      showCartEditQuantityScreen();
    }
  }

  if (state == CART_EDIT_QUANTITY) {

    if (digitalRead(WIO_5S_UP) == LOW) {
      editQuantity = min(editQuantity + 1, MAX_PRODUCT_QUANTITY);
      showCartEditQuantityScreen();
      delay(200);
    }

    if (digitalRead(WIO_5S_DOWN) == LOW) {
      editQuantity = max(editQuantity - 1, 0);
      showCartEditQuantityScreen();
      delay(200);
    }

    if (digitalRead(WIO_5S_PRESS) == LOW) {
      delay(200);

      cartQuantities[selectedCartProduct] = editQuantity;

      if (isCartEmpty()) {
        state = PRODUCT;
        selectedProduct = 0;
        selectedQuantity = 1;
        showProductScreen();
      } else {
        state = CART_CONFIRM;
        confirmOption = 0;
        showCartConfirmScreen();
      }
    }
  }
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

  pinMode(WIO_5S_UP, INPUT_PULLUP);
  pinMode(WIO_5S_DOWN, INPUT_PULLUP);
  pinMode(WIO_5S_LEFT, INPUT_PULLUP);
  pinMode(WIO_5S_RIGHT, INPUT_PULLUP);
  pinMode(WIO_5S_PRESS, INPUT_PULLUP);

  for (byte i = 0; i < 6; i++) {
    rfidKey.keyByte[i] = 0xFF;
  }

  initSystem();
}

// ---------------- RFID INIT ----------------
bool initRFID() {
  showRFIDInitializing();

  Wire1.begin();
  mfrc522.PCD_Init();

  byte v = mfrc522.PCD_ReadRegister(mfrc522.VersionReg);

  Serial.print("[RFID] Version register: 0x");
  if (v < 0x10) {
    Serial.print("0");
  }
  Serial.println(v, HEX);

  if (v == 0x00 || v == 0xFF) {
    Serial.println("[RFID] Reader not responding");

    rfidReady = false;
    showRFIDFailed();
    delay(1500);
    return false;
  }

  Serial.println("[RFID] Reader detected and ready");

  mfrc522.PCD_AntennaOn();

  rfidReady = true;
  return true;
}

// ---------------- LOOP ----------------
void loop() {
#if defined(__SAMD51__)
  if (cardScanTimedOutByWatchdog && cardScanWatchdogTicks >= 112 && !cardScanNeedsRecovery) {
    cardScanNeedsRecovery = true;
  }

  if (cardScanNeedsRecovery) {
    recoverAfterFailedCardScan();
    showWelcome();
  }
#endif

  if (WiFi.status() != WL_CONNECTED) {
    wifiReady = false;
  }

  if (!wifiReady || !rfidReady) {
    initSystem();
    delay(1000);
    return;
  }

  if (state == AUTH) {
    handleAuthInput();
    return;
  }

  handleJoystick();

  // ---------------- CONFIRM LOGIC ----------------
  if (state == CONFIRM) {

    if (digitalRead(WIO_5S_LEFT) == LOW) {
      confirmOption = 0;
      showConfirmScreen();
      delay(200);
    }

    if (digitalRead(WIO_5S_RIGHT) == LOW) {
      confirmOption = 1;
      showConfirmScreen();
      delay(200);
    }

    if (digitalRead(WIO_5S_PRESS) == LOW) {
      delay(200);

      if (confirmOption == 0) {
        state = QUANTITY;
        selectedQuantity = 1;
        confirmOption = 0;
        showQuantityScreen();
      }
      else {
        state = PRODUCT;
        showProductScreen();
      }

      confirmOption = 0;
    }
  }
}
