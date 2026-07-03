/*
  XIAO ESP32C6 + Emakefun MFRC522 (I2C) functional test

  Wiring (PH2.0 / Grove-style I2C module):
    RFID 5V  -> XIAO 5V
    RFID GND -> XIAO GND
    RFID SDA -> XIAO D4 (GPIO22)
    RFID SCL -> XIAO D5 (GPIO23)

  Default I2C address: 0x28
*/

#include <Arduino.h>
#include <Wire.h>
#include "Emakefun_RFID.h"

static const int I2C_SDA = 22;
static const int I2C_SCL = 23;
static const byte RFID_ADDR = 0x28;

MFRC522 mfrc522(RFID_ADDR);

void scanI2C() {
  Serial.println("Scanning I2C bus...");
  int found = 0;
  for (byte addr = 1; addr < 127; addr++) {
    Wire.beginTransmission(addr);
    if (Wire.endTransmission() == 0) {
      Serial.printf("  Found device at 0x%02X\n", addr);
      found++;
    }
  }
  if (found == 0) {
    Serial.println("  No I2C devices found. Check wiring (5V/GND/SDA/SCL).");
  }
}

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

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println();
  Serial.println("=== XIAO ESP32C6 RFID Test ===");

  Wire.begin(I2C_SDA, I2C_SCL);
  Wire.setClock(100000);

  scanI2C();

  mfrc522.PCD_Init();
  byte version = mfrc522.PCD_ReadRegister(MFRC522::VersionReg);
  Serial.printf("MFRC522 firmware version: 0x%02X", version);
  if (version == 0x91 || version == 0x92) {
    Serial.println(" (OK)");
  } else if (version == 0x00 || version == 0xFF) {
    Serial.println(" (FAIL - module not responding)");
    Serial.println("Check power, I2C wiring, and address 0x28.");
  } else {
    Serial.println(" (unexpected, but module may still work)");
  }

  Serial.println("Place an RFID card on the reader...");
}

void loop() {
  if (!mfrc522.PICC_IsNewCardPresent() || !mfrc522.PICC_ReadCardSerial()) {
    delay(100);
    return;
  }

  String uid = readUidString();
  byte piccType = mfrc522.PICC_GetType(mfrc522.uid.sak);
  Serial.print("Card detected! UID: ");
  Serial.print(uid);
  Serial.print("  Type: ");
  Serial.println((const __FlashStringHelper*)mfrc522.PICC_GetTypeName(piccType));

  mfrc522.PICC_HaltA();
  mfrc522.PCD_StopCrypto1();
  delay(1000);
}
