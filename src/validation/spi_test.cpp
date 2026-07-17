#include <Arduino.h>
#include <SPI.h>
#include <SD.h>

#define SD_CS 5

void setup() {
  Serial.begin(115200);
  delay(2000);

  Serial.println("=============================");
  Serial.println("MINIMAL SPI / SD TEST");
  Serial.println("=============================");
  Serial.printf("Free heap: %u bytes\n", ESP.getFreeHeap());

  // Explicitly deselect card
  pinMode(SD_CS, OUTPUT);
  digitalWrite(SD_CS, HIGH);
  delay(1000);

  Serial.println("Starting SPI...");
  SPI.begin(18, 19, 23, SD_CS);

  // Try mounting at multiple frequencies
  uint32_t freqs[] = {400000, 1000000, 2000000, 4000000, 20000000};
  const char* freqNames[] = {"400 KHz", "1 MHz", "2 MHz", "4 MHz", "20 MHz"};

  for (int i = 0; i < 5; i++) {
    Serial.print("Trying ");
    Serial.print(freqNames[i]);
    Serial.print("... ");

    SD.end();
    delay(500);

    if (SD.begin(SD_CS, SPI, freqs[i])) {
      Serial.println("✓ MOUNTED");
      uint8_t cardType = SD.cardType();
      Serial.print("Card type: ");
      switch (cardType) {
        case CARD_MMC:  Serial.println("MMC");  break;
        case CARD_SD:   Serial.println("SD");   break;
        case CARD_SDHC: Serial.println("SDHC"); break;
        default:        Serial.println("Unknown"); break;
      }
      float cardGB = SD.cardSize() / 1073741824.0;
      Serial.printf("Card size: %.1f GB\n", cardGB);
      SD.end();
    } else {
      Serial.println("✗ FAILED");
    }
    delay(500);
  }

  Serial.println();
  Serial.println("Test complete.");
}

void loop() {}