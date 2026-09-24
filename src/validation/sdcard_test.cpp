#include <Arduino.h>
#include <SPI.h>
#include <SD.h>

#define SD_CS   4
#define SD_SCK  18
#define SD_MOSI 23
#define SD_MISO 19

// Expected storage per night at configured rates
#define NIGHTS_TARGET     10
#define EST_GB_PER_NIGHT  1.0

void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial.println("=============================");
  Serial.println("MICROSD CAPACITY CHECK");
  Serial.println("=============================");

  // ── Force SPI pins to known idle state ───────────────────
  // Clears any residual state from previous sketches
  // or the boot process before SD library takes over
  pinMode(SD_CS,   OUTPUT); digitalWrite(SD_CS,   HIGH);
  pinMode(SD_SCK,  OUTPUT); digitalWrite(SD_SCK,  HIGH);
  pinMode(SD_MOSI, OUTPUT); digitalWrite(SD_MOSI, HIGH);
  pinMode(SD_MISO, INPUT);
  delay(1000);

  // ── Mount with retry ─────────────────────────────────────
  SPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
  bool sdMounted = false;
  for (int attempt = 1; attempt <= 5; attempt++) {
    Serial.printf("SD mount attempt %d/5...\n", attempt);
    if (SD.begin(SD_CS, SPI, 4000000)) {
      sdMounted = true;
      break;
    }
    Serial.println("Mount failed — retrying in 2 seconds.");
    SD.end();
    delay(2000);
    SPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
  }
  if (!sdMounted) {
    Serial.println("FAIL — microSD not mounted after 5 attempts.");
    Serial.println("Check wiring and FAT32 format.");
    while (true);
  }

  // ── Card info ────────────────────────────────────────────
  uint8_t cardType = SD.cardType();
  Serial.print("Card type:   ");
  switch (cardType) {
    case CARD_MMC:  Serial.println("MMC");  break;
    case CARD_SD:   Serial.println("SD");   break;
    case CARD_SDHC: Serial.println("SDHC"); break;
    default:        Serial.println("Unknown"); break;
  }

  uint64_t cardSizeBytes = SD.cardSize();
  float    cardSizeGB    = cardSizeBytes / 1073741824.0;
  uint64_t usedBytes     = SD.usedBytes();
  uint64_t freeBytes     = cardSizeBytes - usedBytes;
  float    usedGB        = usedBytes  / 1073741824.0;
  float    freeGB        = freeBytes  / 1073741824.0;

  Serial.println();
  Serial.println("--- Storage ---");
  Serial.print("Total size:  "); Serial.print(cardSizeGB, 2);
  Serial.println(" GB");
  Serial.print("Used:        "); Serial.print(usedGB, 2);
  Serial.println(" GB");
  Serial.print("Free:        "); Serial.print(freeGB, 2);
  Serial.println(" GB");

  // ── Capacity check ───────────────────────────────────────
  float requiredGB = NIGHTS_TARGET * EST_GB_PER_NIGHT;
  Serial.println();
  Serial.println("--- Capacity check ---");
  Serial.print("Required for "); Serial.print(NIGHTS_TARGET);
  Serial.print(" nights (~"); Serial.print(EST_GB_PER_NIGHT, 1);
  Serial.print(" GB/night): "); Serial.print(requiredGB, 1);
  Serial.println(" GB");
  Serial.print("Free space available:          ");
  Serial.print(freeGB, 2); Serial.println(" GB");

  if (freeGB >= requiredGB) {
    Serial.println("✓ Sufficient space for full run — PASS");
  } else {
    Serial.print("✗ FAIL — insufficient space. Need ");
    Serial.print(requiredGB - freeGB, 2);
    Serial.println(" GB more.");
  }

  // ── Write / read integrity test ──────────────────────────
  Serial.println();
  Serial.println("--- Write / read integrity test ---");

  const char* testPath = "/sd_test.txt";
  const char* testData = "PILLOW SD INTEGRITY CHECK OK";

  File wf = SD.open(testPath, FILE_WRITE);
  if (!wf) {
    Serial.println("FAIL — could not open test file for write.");
    while (true);
  }
  wf.println(testData);
  wf.flush();
  wf.close();

  File rf = SD.open(testPath);
  if (!rf) {
    Serial.println("FAIL — could not open test file for read.");
    while (true);
  }
  char readBuf[64] = {0};
  int i = 0;
  while (rf.available() && i < 63) {
    readBuf[i++] = rf.read();
  }
  rf.close();

  for (int j = 0; j < 64; j++) {
    if (readBuf[j] == '\n' || readBuf[j] == '\r') {
      readBuf[j] = '\0';
      break;
    }
  }

  if (strcmp(readBuf, testData) == 0) {
    Serial.println("Written: \"" + String(testData) + "\"");
    Serial.println("Read:    \"" + String(readBuf)  + "\"");
    Serial.println("✓ Write / read integrity — PASS");
  } else {
    Serial.println("✗ FAIL — data mismatch on read back.");
    Serial.print("Expected: "); Serial.println(testData);
    Serial.print("Got:      "); Serial.println(readBuf);
  }

  SD.remove(testPath);

  Serial.println();
  Serial.println("=============================");
  Serial.println("SD check complete.");
  Serial.println("=============================");

  // Clean teardown so next flash gets clean SPI state
  SD.end();
  SPI.end();
}

void loop() {}