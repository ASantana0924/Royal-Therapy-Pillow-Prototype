#include <Arduino.h>
#include <Wire.h>
#include <SPI.h>
#include <SD.h>
#include <RTClib.h>
#include "../../src/logging/session.h"

#define SD_CS    4
#define SDA_PIN  21
#define SCL_PIN  22

RTC_DS3231 rtc;

void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial.println("=============================");
  Serial.println("FILE NAMING + METADATA TEST");
  Serial.println("=============================");

  Wire.begin(SDA_PIN, SCL_PIN);

  // ── RTC ─────────────────────────────────────────────────
  if (!rtc.begin()) {
    Serial.println("FAIL — DS3231 not found.");
    while (true);
  }
  DateTime now = rtc.now();
  char isoBuf[30];
  snprintf(isoBuf, sizeof(isoBuf),
    "%04d-%02d-%02dT%02d:%02d:%02d",
    now.year(), now.month(), now.day(),
    now.hour(), now.minute(), now.second()
  );
  Serial.print("RTC time: "); Serial.println(isoBuf);

  // ── microSD ──────────────────────────────────────────────
  SPI.begin(18, 19, 23, SD_CS);
  if (!SD.begin(SD_CS)) {
    Serial.println("FAIL — microSD not found.");
    while (true);
  }
  Serial.println("microSD init OK");
  Serial.println();

  // ── Test filename generation ──────────────────────────────
  Serial.println("--- Filename generation ---");

  char accFile[60], envFile[60], micFile[60],
       wavFile[60], metaFile[60];

  buildSessionFilename(now, "ACC", "n01", "csv", accFile, sizeof(accFile));
  buildSessionFilename(now, "ENV", "n01", "csv", envFile, sizeof(envFile));
  buildSessionFilename(now, "MIC", "n01", "csv", micFile, sizeof(micFile));
  buildWavFilename(now, "n01", 0, wavFile, sizeof(wavFile));
  snprintf(metaFile, sizeof(metaFile),
    "/PILLOW_%s_%04d%02d%02d_meta.json",
    UNIT_ID, now.year(), now.month(), now.day()
  );

  Serial.print("ACC:   "); Serial.println(accFile);
  Serial.print("ENV:   "); Serial.println(envFile);
  Serial.print("MIC idx: "); Serial.println(micFile);
  Serial.print("WAV h00: "); Serial.println(wavFile);
  Serial.print("META:  "); Serial.println(metaFile);
  Serial.println();

  // ── Validate naming convention ────────────────────────────
  Serial.println("--- Naming convention checks ---");
  bool pass = true;

  // Check ACC filename contains expected components
  if (strstr(accFile, "PILLOW_") == NULL) {
    Serial.println("FAIL — ACC missing PILLOW_ prefix"); pass = false;
  }
  if (strstr(accFile, "_ACC_") == NULL) {
    Serial.println("FAIL — ACC missing _ACC_ stream tag"); pass = false;
  }
  if (strstr(accFile, "_n01") == NULL) {
    Serial.println("FAIL — ACC missing night number"); pass = false;
  }
  if (strstr(accFile, ".csv") == NULL) {
    Serial.println("FAIL — ACC missing .csv extension"); pass = false;
  }
  if (strstr(wavFile, "_h00.wav") == NULL) {
    Serial.println("FAIL — WAV missing hour segment suffix"); pass = false;
  }
  if (strstr(metaFile, "_meta.json") == NULL) {
    Serial.println("FAIL — meta missing _meta.json suffix"); pass = false;
  }

  if (pass) {
    Serial.println("✓ All filenames match convention — PASS");
  }
  Serial.println();

  // ── Write and verify meta.json ────────────────────────────
  Serial.println("--- Writing meta.json ---");
  writeMetaJSON(
    now,
    "n01",
    "3-layer memory foam",
    23.0,
    "bench test; no partner",
    "sensor at pillow top edge ~10 cm from head",
    "naming and metadata validation test"
  );

  // Read it back and print to Serial
  Serial.println();
  Serial.println("--- meta.json contents ---");
  File f = SD.open(metaFile);
  if (f) {
    while (f.available()) {
      Serial.write(f.read());
    }
    f.close();
    Serial.println();
    Serial.println("✓ meta.json read back successfully — PASS");
  } else {
    Serial.println("FAIL — could not read back meta.json");
  }

  Serial.println();
  Serial.println("=============================");
  Serial.println("Test complete.");
  Serial.println("=============================");
}

void loop() {}