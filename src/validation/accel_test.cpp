#include <Arduino.h>
#include <Wire.h>
#include <SPI.h>
#include <SD.h>
#include <RTClib.h>
#include <Adafruit_ADXL345_U.h>

// ── Pin definitions ──────────────────────────────────────
#define SD_CS       4
#define SDA_PIN     21
#define SCL_PIN     22
#define TARGET_HZ   100        // 50–100 Hz per spec
#define INTERVAL_MS (1000 / TARGET_HZ)

// ── Globals ──────────────────────────────────────────────
Adafruit_ADXL345_Unified accel = Adafruit_ADXL345_Unified(12345);
RTC_DS3231 rtc;
File dataFile;

// Night number — increment manually before each session
// e.g. n01 for first night, n02 for second, etc.
const char* NIGHT = "n00";  // n00 = bench validation run
const char* UNIT  = "01";

// ── Helpers ──────────────────────────────────────────────
// Build ts_iso string: 2026-06-26T22:05:04.020
void buildISO(DateTime dt, uint32_t ms_offset, char* buf, size_t len) {
  snprintf(buf, len,
    "%04d-%02d-%02dT%02d:%02d:%02d.%03lu",
    dt.year(), dt.month(),  dt.day(),
    dt.hour(), dt.minute(), dt.second(),
    (unsigned long)(ms_offset % 1000)
  );
}

// Build ts_epoch_ms (Unix ms, UTC)
uint64_t buildEpochMs(DateTime dt, uint32_t ms_offset) {
  return ((uint64_t)dt.unixtime() * 1000ULL) + (ms_offset % 1000);
}

// Build filename per spec: PILLOW_01_YYYYMMDD_ACC_n01.csv
void buildFilename(DateTime dt, char* buf, size_t len) {
  snprintf(buf, len,
    "/PILLOW_%s_%04d%02d%02d_ACC_%s.csv",
    UNIT,
    dt.year(), dt.month(), dt.day(),
    NIGHT
  );
}

void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial.println("=============================");
  Serial.println("ADXL345 ACC LOGGER");
  Serial.println("=============================");

  Wire.begin(SDA_PIN, SCL_PIN);

  // ── RTC init ────────────────────────────────────────────
  if (!rtc.begin()) {
    Serial.println("FAIL — DS3231 not found. Check wiring.");
    while (true);
  }
  if (rtc.lostPower()) {
    Serial.println("WARN — RTC lost power, setting compile time.");
    rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
  }
  DateTime now = rtc.now();
  Serial.print("RTC time: ");
  char isoBuf[30];
  buildISO(now, 0, isoBuf, sizeof(isoBuf));
  Serial.println(isoBuf);

  // ── ADXL345 init ────────────────────────────────────────
  if (!accel.begin()) {
    Serial.println("FAIL — ADXL345 not found. Check wiring.");
    while (true);
  }
  // Set range to ±16g per spec
  accel.setRange(ADXL345_RANGE_16_G);
  // Set data rate to 100 Hz
  accel.setDataRate(ADXL345_DATARATE_100_HZ);
  Serial.println("ADXL345 init OK — range ±16g, rate 100 Hz");

  // ── microSD init ────────────────────────────────────────
  SPI.begin(18, 19, 23, SD_CS);
  if (!SD.begin(SD_CS)) {
    Serial.println("FAIL — microSD not found. Check wiring and format.");
    while (true);
  }
  Serial.println("microSD init OK");

  // ── Open file ───────────────────────────────────────────
  char filename[50];
  buildFilename(now, filename, sizeof(filename));
  Serial.print("Opening file: ");
  Serial.println(filename);

  dataFile = SD.open(filename, FILE_WRITE);
  if (!dataFile) {
    Serial.println("FAIL — could not open file for writing.");
    while (true);
  }

  // Write header row per spec
  dataFile.println("ts_iso,ts_epoch_ms,accel_x_g,accel_y_g,accel_z_g");
  dataFile.flush();
  Serial.println("Header written. Logging started.");
  Serial.println("-----------------------------");
  Serial.println("ts_iso                     accel_x    accel_y    accel_z");
  Serial.println("-----------------------------");
}

void loop() {
  static unsigned long lastSample = 0;
  static unsigned long rowCount   = 0;
  static unsigned long lastReport = 0;

  unsigned long now_ms = millis();

  if (now_ms - lastSample >= INTERVAL_MS) {
    lastSample = now_ms;

    // Get RTC time + millis offset for sub-second precision
    DateTime dt = rtc.now();
    uint32_t ms_off = now_ms % 1000;

    // Read accelerometer
    sensors_event_t event;
    accel.getEvent(&event);

    float ax = event.acceleration.x / 9.81;  // m/s² → g
    float ay = event.acceleration.y / 9.81;
    float az = event.acceleration.z / 9.81;

    // Build timestamp strings
    char iso[30];
    buildISO(dt, ms_off, iso, sizeof(iso));
    uint64_t epochMs = buildEpochMs(dt, ms_off);

    // Write CSV row
    char row[100];
    snprintf(row, sizeof(row),
      "%s,%llu,%.4f,%.4f,%.4f",
      iso, epochMs, ax, ay, az
    );
    dataFile.println(row);
    rowCount++;

    // Flush every 50 rows (~0.5 sec at 100 Hz) to protect data
    if (rowCount % 50 == 0) {
      dataFile.flush();
    }

    // Print to Serial every second for monitoring
    if (now_ms - lastReport >= 1000) {
      lastReport = now_ms;
      Serial.print(iso);
      Serial.print("   ");
      Serial.print(ax, 4); Serial.print("g  ");
      Serial.print(ay, 4); Serial.print("g  ");
      Serial.print(az, 4); Serial.println("g");
      Serial.print("Rows logged: "); Serial.println(rowCount);
    }
  }
}