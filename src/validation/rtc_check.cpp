#include <Arduino.h>
#include <Wire.h>
#include <RTClib.h>

RTC_DS3231 rtc;

void buildISO(DateTime dt, uint32_t ms_offset, char* buf, size_t len) {
  snprintf(buf, len,
    "%04d-%02d-%02dT%02d:%02d:%02d.%03lu",
    dt.year(), dt.month(),  dt.day(),
    dt.hour(), dt.minute(), dt.second(),
    (unsigned long)(ms_offset % 1000)
  );
}

uint64_t buildEpochMs(DateTime dt, uint32_t ms_offset) {
  return ((uint64_t)dt.unixtime() * 1000ULL) + (ms_offset % 1000);
}

// Parse time string from PC: T2026,7,10,22,30,0
bool parseAndSetTime(String cmd) {
  if (!cmd.startsWith("T")) return false;
  cmd = cmd.substring(1); // strip T

  int vals[6];
  int idx = 0;
  char buf[40];
  cmd.toCharArray(buf, sizeof(buf));
  char* token = strtok(buf, ",");
  while (token != NULL && idx < 6) {
    vals[idx++] = atoi(token);
    token = strtok(NULL, ",");
  }
  if (idx < 6) return false;

  rtc.adjust(DateTime(vals[0], vals[1], vals[2],
                      vals[3], vals[4], vals[5]));
  return true;
}

void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial.println("=============================");
  Serial.println("DS3231 RTC CHECK");
  Serial.println("=============================");

  Wire.begin(21, 22);

  if (!rtc.begin()) {
    Serial.println("FAIL — DS3231 not found.");
    while (true);
  }
  Serial.println("DS3231 detected on I2C bus (0x68).");

  // ── Wait for time sync from PC ────────────────────────
  Serial.println("Waiting for time sync from PC...");
  Serial.println("Run: python set_rtc.py");
  Serial.println("(or send: T2026,7,10,22,30,0)");

  unsigned long waitStart = millis();
  bool synced = false;

  while (millis() - waitStart < 10000) { // wait up to 10 seconds
    if (Serial.available()) {
      String cmd = Serial.readStringUntil('\n');
      cmd.trim();
      if (parseAndSetTime(cmd)) {
        synced = true;
        break;
      }
    }
  }

  if (synced) {
    DateTime now = rtc.now();
    char isoBuf[30];
    buildISO(now, 0, isoBuf, sizeof(isoBuf));
    Serial.print("✓ RTC synced to: ");
    Serial.println(isoBuf);
    Serial.println("Compare to wall clock — should match within 1-2 seconds.");
  } else {
    Serial.println("No sync received — using existing RTC time.");
    DateTime now = rtc.now();
    char isoBuf[30];
    buildISO(now, 0, isoBuf, sizeof(isoBuf));
    Serial.print("Current RTC time: ");
    Serial.println(isoBuf);
  }

  Serial.println();

  // ── Timestamp advance check ───────────────────────────
  Serial.println("=============================");
  Serial.println("TIMESTAMP ADVANCE CHECK");
  Serial.println("Printing one row every 5 seconds for 30 seconds.");
  Serial.println("=============================");
  Serial.println();
  Serial.println("ts_iso                       ts_epoch_ms");
  Serial.println("------------------------------------------");

  DateTime prev;
  bool first     = true;
  bool monotonic = true;

  for (int i = 0; i < 6; i++) {
    DateTime current = rtc.now();
    uint32_t ms_off  = millis() % 1000;

    char iso[30];
    buildISO(current, ms_off, iso, sizeof(iso));
    uint64_t epochMs = buildEpochMs(current, ms_off);

    Serial.print(iso);
    Serial.print("    ");
    Serial.println((unsigned long)epochMs);

    if (!first && current.unixtime() <= prev.unixtime()) {
      monotonic = false;
    }
    prev  = current;
    first = false;

    delay(5000);
  }

  // ── Results ───────────────────────────────────────────
  Serial.println();
  Serial.println("=============================");
  Serial.println("RESULTS");
  Serial.println("=============================");

  if (monotonic) {
    Serial.println("✓ Timestamps monotonically increasing — PASS");
  } else {
    Serial.println("✗ Timestamps NOT monotonically increasing — FAIL");
  }

  DateTime final_dt = rtc.now();
  char finalBuf[30];
  buildISO(final_dt, 0, finalBuf, sizeof(finalBuf));
  Serial.print("Final RTC time: ");
  Serial.println(finalBuf);
  Serial.println("Scan complete.");
}

void loop() {}