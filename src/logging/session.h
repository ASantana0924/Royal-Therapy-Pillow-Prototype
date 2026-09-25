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
#define FW_VERSION        "1.1.0"
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

// ── Session config from SD card ───────────────────────────
// Reads /session.cfg from the SD card root.
// Format — one key=value per line, no quotes:
//   NIGHT=n23
//   PILLOW_MODEL=hotel gel microfiber
//   ROOM_TEMP=23.5
//   CONDITIONS=no partner present, window closed, fan on
//   PLACEMENT=sensor at pillow top edge ~10 cm from head
//   NOTES=
//
// Missing keys fall back to the defaults defined below.
// Edit on the SD card in Notepad before each night.
// No reflashing needed.

struct SessionConfig {
  char night[16];
  char pillow_model[64];
  float room_temp_c;
  char conditions[128];
  char placement[128];
  char notes[128];
};

static void trimStr(char* s) {
  int start = 0;
  while (s[start] == ' ' || s[start] == '\t') start++;
  if (start > 0) memmove(s, s + start, strlen(s) - start + 1);
  int len = strlen(s);
  while (len > 0 && (s[len-1] == ' ' || s[len-1] == '\t' ||
                     s[len-1] == '\r' || s[len-1] == '\n')) {
    s[--len] = '\0';
  }
}

static void writeDefaultConfig() {
  if (SD.exists("/session.cfg")) return;
  File f = SD.open("/session.cfg", FILE_WRITE);
  if (!f) {
    Serial.println("WARN: could not create default session.cfg");
    return;
  }
  f.println("# Pillow Sensor Session Configuration");
  f.println("# Edit this file on the SD card before each night.");
  f.println("# Lines starting with # are comments.");
  f.println("#");
  f.println("NIGHT=n01");
  f.println("PILLOW_MODEL=hotel gel microfiber");
  f.println("ROOM_TEMP=23.0");
  f.println("CONDITIONS=no partner present, window closed, fan off");
  f.println("PLACEMENT=environment sensor at pillow top edge under pillow cover ~10 cm from head");
  f.println("NOTES=");
  f.flush();
  f.close();
  Serial.println("Created default session.cfg on SD card.");
}

static SessionConfig readSessionConfig() {
  SessionConfig cfg;
  // Defaults — used if session.cfg missing or a key is absent
  strncpy(cfg.night,        "n01",                                sizeof(cfg.night)        - 1);
  strncpy(cfg.pillow_model, "hotel gel microfiber",               sizeof(cfg.pillow_model) - 1);
  cfg.room_temp_c = 23.0f;
  strncpy(cfg.conditions,   "no partner present",                 sizeof(cfg.conditions)   - 1);
  strncpy(cfg.placement,    "sensor at pillow top edge ~10cm",    sizeof(cfg.placement)    - 1);
  strncpy(cfg.notes,        "",                                   sizeof(cfg.notes)        - 1);

  writeDefaultConfig();

  File f = SD.open("/session.cfg", FILE_READ);
  if (!f) {
    Serial.println("WARN: session.cfg not found, using defaults.");
    return cfg;
  }

  Serial.println("Reading session.cfg...");
  while (f.available()) {
    String line = f.readStringUntil('\n');
    char buf[256];
    line.toCharArray(buf, sizeof(buf));
    trimStr(buf);

    if (buf[0] == '#' || buf[0] == '\0') continue;

    char* eq = strchr(buf, '=');
    if (!eq) continue;
    *eq = '\0';
    char* key = buf;
    char* val = eq + 1;
    trimStr(key);
    trimStr(val);

    if      (strcmp(key, "NIGHT")        == 0) strncpy(cfg.night,        val, sizeof(cfg.night)        - 1);
    else if (strcmp(key, "PILLOW_MODEL") == 0) strncpy(cfg.pillow_model, val, sizeof(cfg.pillow_model) - 1);
    else if (strcmp(key, "ROOM_TEMP")    == 0) cfg.room_temp_c = atof(val);
    else if (strcmp(key, "CONDITIONS")   == 0) strncpy(cfg.conditions,   val, sizeof(cfg.conditions)   - 1);
    else if (strcmp(key, "PLACEMENT")    == 0) strncpy(cfg.placement,    val, sizeof(cfg.placement)    - 1);
    else if (strcmp(key, "NOTES")        == 0) strncpy(cfg.notes,        val, sizeof(cfg.notes)        - 1);
  }
  f.close();

  Serial.printf("  Night:   %s\n",     cfg.night);
  Serial.printf("  Pillow:  %s\n",     cfg.pillow_model);
  Serial.printf("  Temp:    %.1f C\n", cfg.room_temp_c);
  Serial.printf("  Cond:    %s\n",     cfg.conditions);
  return cfg;
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