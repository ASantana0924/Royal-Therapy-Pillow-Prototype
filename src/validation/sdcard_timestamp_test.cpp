#include <Arduino.h>
#include <Wire.h>
#include <SPI.h>
#include <SD.h>
#include <RTClib.h>
#include "../../src/logging/session.h"

#define SD_CS   5
#define SDA_PIN 21
#define SCL_PIN 22

RTC_DS3231 rtc;

void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial.println("=============================");
  Serial.println("SD TIMESTAMP TEST");
  Serial.println("=============================");

  Wire.begin(SDA_PIN, SCL_PIN);

  // ── RTC ─────────────────────────────────────────────────
  if (!rtc.begin()) {
    Serial.println("FAIL — DS3231 not found.");
    while (true);
  }

  // ── Wait for time sync from set_rtc.py ──────────────────
  waitForTimeSync(rtc, 10000);

  // ── microSD ──────────────────────────────────────────────
  SPI.begin(18, 19, 23, SD_CS);
  if (!SD.begin(SD_CS)) {
    Serial.println("FAIL — microSD not mounted.");
    while (true);
  }
  Serial.println("microSD init OK");

  // ── Write test file ──────────────────────────────────────
  Serial.println();
  Serial.println("--- Writing test file ---");

  DateTime now = rtc.now();
  char filename[40];
  snprintf(filename, sizeof(filename),
    "/TSTEST_%04d%02d%02d_%02d%02d%02d.txt",
    now.year(), now.month(), now.day(),
    now.hour(), now.minute(), now.second()
  );

  Serial.print("Filename: "); Serial.println(filename);

  File wf = SD.open(filename, FILE_WRITE);
  if (!wf) {
    Serial.println("FAIL — could not open file for write.");
    while (true);
  }

  // Write a few lines with timestamp info
  char line1[60], line2[60], line3[60];
  snprintf(line1, sizeof(line1),
    "Written at: %04d-%02d-%02d %02d:%02d:%02d",
    now.year(), now.month(), now.day(),
    now.hour(), now.minute(), now.second()
  );
  snprintf(line2, sizeof(line2),
    "Unix epoch: %lu", (unsigned long)now.unixtime()
  );
  snprintf(line3, sizeof(line3),
    "Unit: %s | FW: %s", UNIT_ID, FW_VERSION
  );

  wf.println(line1);
  wf.println(line2);
  wf.println(line3);
  wf.flush();
  wf.close();

  Serial.println("File written successfully.");

  // ── Read back and verify ──────────────────────────────────
  Serial.println();
  Serial.println("--- Reading back ---");

  File rf = SD.open(filename);
  if (!rf) {
    Serial.println("FAIL — could not open file for read.");
    while (true);
  }

  int lineCount = 0;
  while (rf.available()) {
    String line = rf.readStringUntil('\n');
    line.trim();
    if (line.length() > 0) {
      Serial.print("  Line "); Serial.print(++lineCount);
      Serial.print(": "); Serial.println(line);
    }
  }
  rf.close();

  if (lineCount == 3) {
    Serial.println("✓ All 3 lines read back correctly — PASS");
  } else {
    Serial.print("✗ Expected 3 lines, got: ");
    Serial.println(lineCount);
  }

  // ── Verify file timestamp ─────────────────────────────────
  Serial.println();
  Serial.println("--- File timestamp check ---");
  Serial.println("Pull the microSD and check the file in Windows Explorer.");
  Serial.println("The file modification date should show:");
  Serial.printf("  %04d-%02d-%02d %02d:%02d\n",
    now.year(), now.month(), now.day(),
    now.hour(), now.minute()
  );
  Serial.println("If it shows 1980-01-01 the callback did not register.");
  Serial.println("If it shows the correct date, both fixes are working.");

  Serial.println();
  Serial.println("=============================");
  Serial.println("Test complete.");
  Serial.println("=============================");
}

void loop() {}