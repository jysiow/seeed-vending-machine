// Basic functional test for Seeed Studio XIAO ESP32C6
// Blinks the user LED (GPIO15) and prints status over USB serial.

void setup() {
  Serial.begin(115200);
  pinMode(LED_BUILTIN, OUTPUT);
  Serial.println("XIAO ESP32C6 functional test started");
}

void loop() {
  digitalWrite(LED_BUILTIN, HIGH);
  Serial.println("LED ON");
  delay(500);
  digitalWrite(LED_BUILTIN, LOW);
  Serial.println("LED OFF");
  delay(500);
}
