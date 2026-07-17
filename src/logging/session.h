#ifndef SESSION_H
#define SESSION_H

#include <Arduino.h>
#include <SD.h>
#include <RTClib.h>
#include <sys/time.h>

// ── Global RTC pointer ────────────────────────────────────
RTC_DS3231* _rtc_ptr = nullptr;

// ── Session config ────────────────────────────────────────
#define UNIT_ID           "01"
#define FW_VERSION        "1.0.0"
#define SENSOR_RATE_ACC   100
#define SENSOR_RATE_ENV   1
#define SENSOR_RATE_MIC   16000

// ── Sync ESP32 system clock from RTC ─────────────────────
// Call once after RTC is set. This sets the ESP32's internal
// system time to match the DS3231. The SD library uses the
// ESP32 system clock for file timestamps automatically.
void syncSystemClock(RTC_DS3231& rtc) {
  _rtc_ptr = &rtc;
  DateTime now = rtc.now();
  struct timeval tv;
  tv.tv_sec  = now.unixtime();
  tv.tv_usec = 0;
  settimeofday(&tv, NULL);
  Serial.println("✓ ESP32 system clock synced from RTC");
}

// ── Filename builders ─────────────────────────────────────
void buildSessionFilename(DateTime dt, const char* stream,
                          const char* night, const char* ext,
                          char* buf, size_t len) {
  snprintf(buf, len,
    "/PILLOW_%s_%04d%02d%02d_%s_%s.%s",
    UNIT_ID,
    dt.year(), dt.month(), dt.day(),
    stream, night, ext
  );
}

void buildWavFilename(DateTime dt, const char* night,
                      uint8_t hour, char* buf, size_t len) {
  snprintf(buf, len,
    "/PILLOW_%s_%04d%02d%02d_MIC_%s_h%02d.wav",
    UNIT_ID,
    dt.year(), dt.month(), dt.day(),
    night, hour
  );
}

// ── Metadata writer ───────────────────────────────────────
void writeMetaJSON(DateTime dt, const char* night,
                   const char* pillow_model,
                   float room_temp_c,
                   const char* conditions,
                   const char* placement,
                   const char* notes) {

  char filename[50];
  snprintf(filename, sizeof(filename),
    "/PILLOW_%s_%04d%02d%02d_meta.json",
    UNIT_ID,
    dt.year(), dt.month(), dt.day()
  );

  File f = SD.open(filename, FILE_WRITE);
  if (!f) {
    Serial.print("FAIL — could not write meta.json: ");
    Serial.println(filename);
    return;
  }

  char sessionDate[12];
  snprintf(sessionDate, sizeof(sessionDate),
    "%04d-%02d-%02d",
    dt.year(), dt.month(), dt.day()
  );

  f.println("{");
  f.printf("  \"prototype\": \"PILLOW\",\n");
  f.printf("  \"unit_id\": \"%s\",\n",          UNIT_ID);
  f.printf("  \"session_date\": \"%s\",\n",     sessionDate);
  f.printf("  \"night\": \"%s\",\n",            night);
  f.printf("  \"firmware_version\": \"%s\",\n", FW_VERSION);
  f.println("  \"sensors\": {");
  f.printf("    \"ADXL345\": {\"stream\": \"ACC\", \"rate_hz\": %d, \"range_g\": 16},\n",
           SENSOR_RATE_ACC);
  f.printf("    \"INMP441\": {\"stream\": \"MIC\", \"rate_hz\": %d, \"bits\": 16, \"channels\": 1},\n",
           SENSOR_RATE_MIC);
  f.printf("    \"BME688\":  {\"stream\": \"ENV\", \"rate_hz\": %d, \"bsec\": false}\n",
           SENSOR_RATE_ENV);
  f.println("  },");
  f.printf("  \"placement\": \"%s\",\n",        placement);
  f.printf("  \"pillow_model\": \"%s\",\n",     pillow_model);
  f.printf("  \"room_temp_c\": %.1f,\n",        room_temp_c);
  f.printf("  \"conditions\": \"%s\",\n",       conditions);
  f.printf("  \"notes\": \"%s\"\n",             notes);
  f.println("}");

  f.flush();
  f.close();

  Serial.print("meta.json written: ");
  Serial.println(filename);
}

// ── Serial time sync ──────────────────────────────────────
bool parseAndSetTime(RTC_DS3231& rtc, String cmd) {
  cmd.trim();
  if (!cmd.startsWith("T")) return false;
  cmd = cmd.substring(1);

  int vals[6] = {0};
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

void waitForTimeSync(RTC_DS3231& rtc, unsigned long timeoutMs = 10000) {
  Serial.println("Waiting for time sync — run: py set_rtc.py");
  unsigned long start = millis();
  bool synced = false;

  while (millis() - start < timeoutMs) {
    if (Serial.available()) {
      String cmd = Serial.readStringUntil('\n');
      if (parseAndSetTime(rtc, cmd)) {
        synced = true;
        break;
      }
    }
  }

  DateTime now = rtc.now();
  char isoBuf[30];
  snprintf(isoBuf, sizeof(isoBuf),
    "%04d-%02d-%02dT%02d:%02d:%02d",
    now.year(), now.month(), now.day(),
    now.hour(), now.minute(), now.second()
  );

  if (synced) {
    Serial.print("✓ RTC synced to: ");
  } else {
    Serial.print("No sync received — using existing RTC time: ");
  }
  Serial.println(isoBuf);

  // Always sync system clock after time sync attempt
  syncSystemClock(rtc);
}

#endif