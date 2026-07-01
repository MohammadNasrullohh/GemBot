#include <Arduino.h>

// Pin 2 adalah pin LED bawaan (built-in LED) pada kebanyakan ESP32 DevKit V1
#define LED_PIN 2

void setup() {
  Serial.begin(115200);
  pinMode(LED_PIN, OUTPUT);
  Serial.println("\n\n=== Halo dari ESP32 DevKit Biasa! ===");
}

void loop() {
  digitalWrite(LED_PIN, HIGH);
  Serial.println("LED ON (Nyala)");
  delay(1000);
  digitalWrite(LED_PIN, LOW);
  Serial.println("LED OFF (Mati)");
  delay(1000);
}
