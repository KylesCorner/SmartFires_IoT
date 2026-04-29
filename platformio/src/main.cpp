#include <Arduino.h>

void setup() {
  Serial.begin(115200);
  while (!Serial && millis() < 3000) {}

  Serial.println("SmartFires Feather M0 build stub");
}

void loop() {
  delay(1000);
}
