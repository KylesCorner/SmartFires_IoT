#include <Arduino.h>

void setup() {
  Serial.begin(115200);
  while (!Serial && millis() < 3000) {
  }

  Serial1.begin(115200);
  Serial.println("SmartFires base station firmware entrypoint active");
  Serial.println("Base station class-architecture port is pending");
}

void loop() {
  static uint32_t lastStatusMs = 0;
  const uint32_t now = millis();

  if (now - lastStatusMs >= 5000) {
    Serial.println("[base] waiting for base station port implementation");
    lastStatusMs = now;
  }
}
