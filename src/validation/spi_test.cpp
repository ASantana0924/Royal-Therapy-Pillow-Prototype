#include <Arduino.h>
#include <SPI.h>
#include <SD.h>

#define SD_CS    4
#define SD_SCK   18
#define SD_MOSI  23
#define SD_MISO  19

void setup() {
  Serial.begin(115200);
  delay(2000);

  Serial.println("=============================");
  Serial.println("SPI ONLY TEST — no I2C, no I2S");
  Serial.println("=============================");
  Serial.printf("Free heap: %u\n", ESP.getFreeHeap());

  // Drive all SPI pins to known state
  Serial.println("Setting SPI pins to idle state...");
  pinMode(SD_CS,   OUTPUT); digitalWrite(SD_CS,   HIGH);
  pinMode(SD_SCK,  OUTPUT); digitalWrite(SD_SCK,  HIGH);
  pinMode(SD_MOSI, OUTPUT); digitalWrite(SD_MOSI, HIGH);
  pinMode(SD_MISO, INPUT);
  delay(1000);

  Serial.printf("CS   (GPIO%d): %s\n", SD_CS,
    digitalRead(SD_CS)   ? "HIGH" : "LOW");
  Serial.printf("SCK  (GPIO%d): %s\n", SD_SCK,
    digitalRead(SD_SCK)  ? "HIGH" : "LOW");
  Serial.printf("MOSI (GPIO%d): %s\n", SD_MOSI,
    digitalRead(SD_MOSI) ? "HIGH" : "LOW");
  Serial.printf("MISO (GPIO%d): %s\n", SD_MISO,
    digitalRead(SD_MISO) ? "HIGH" : "LOW");

  Serial.println();
  Serial.println("Starting SPI...");
  SPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
  delay(500);

  Serial.println("Attempting SD mount at 400 kHz...");
  if (SD.begin(SD_CS, SPI, 400000)) {
    Serial.println("✓ MOUNTED at 400 kHz");
    Serial.printf("Card type: %d\n", SD.cardType());
    Serial.printf("Card size: %.1f GB\n",
      SD.cardSize() / 1073741824.0);
    SD.end();
  } else {
    Serial.println("✗ FAILED at 400 kHz");
  }

  delay(500);
  SPI.end();
  delay(500);
  SPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);

  Serial.println("Attempting SD mount at 4 MHz...");
  if (SD.begin(SD_CS, SPI, 4000000)) {
    Serial.println("✓ MOUNTED at 4 MHz");
    SD.end();
  } else {
    Serial.println("✗ FAILED at 4 MHz");
  }

  Serial.println();
  Serial.println("Test complete.");
}

void loop() {}