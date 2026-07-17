#include <Arduino.h>
#include <Wire.h>
#include <SPI.h>
#include <SD.h>
#include <RTClib.h>
#include <Adafruit_BME680.h>

// ── Pin definitions ──────────────────────────────────────
#define SD_CS       5
#define SDA_PIN     21
#define SCL_PIN     22
#define TARGET_HZ   1          // 1 Hz per spec
#define INTERVAL_MS (1000 / TARGET_HZ)

// ── Globals ──────────────────────────────────────────────
Adafruit_BME680 bme;
RTC_DS3231 rtc;
File dataFile;

const char* NIGHT = "n00";  // n00 = bench validation run
const char* UNIT  = "01";

// ── Helpers ──────────────────────────────────────────────
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

void buildFilename(DateTime dt, char* buf, size_t len) {
  snprintf(buf, len,
    "/PILLOW_%s_%04d%02d%02d_ENV_%s.csv",
    UNIT,
    dt.year(), dt.month(), dt.day(),
    NIGHT
  );
}

void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial.println("=============================");
  Serial.println("BME688 ENV LOGGER (raw mode)");
  Serial.println("=============================");

  Wire.begin(SDA_PIN, SCL_PIN);

  // ── RTC init ────────────────────────────────────────────
  if (!rtc.begin()) {
    Serial.println("FAIL — DS3231 not found. Check wiring.");
    while (true);
  }
  DateTime now = rtc.now();
  Serial.print("RTC time: ");
  char isoBuf[30];
  buildISO(now, 0, isoBuf, sizeof(isoBuf));
  Serial.println(isoBuf);

  // ── BME688 init ─────────────────────────────────────────
  if (!bme.begin(0x77)) {
    Serial.println("FAIL — BME688 not found at 0x77. Trying 0x76...");
    if (!bme.begin(0x76)) {
      Serial.println("FAIL — BME688 not found at 0x76 either. Check wiring.");
      while (true);
    }
  }
  Serial.println("BME688 init OK");

  // Oversampling + filter settings (standard recommended config)
  bme.setTemperatureOversampling(BME680_OS_8X);
  bme.setHumidityOversampling(BME680_OS_2X);
  bme.setPressureOversampling(BME680_OS_4X);
  bme.setIIRFilterSize(BME680_FILTER_SIZE_3);
  bme.setGasHeater(320, 150); // 320°C for 150ms — standard gas read

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

  // Header per spec — raw mode leaves iaq/iaq_accuracy blank
  dataFile.println("ts_iso,ts_epoch_ms,temp_C,humidity_pct,pressure_hPa,gas_resistance_ohm,iaq,iaq_accuracy");
  dataFile.flush();
  Serial.println("Header written. Logging started (1 Hz).");
  Serial.println("-----------------------------");
  Serial.println("ts_iso                     temp_C   hum%    press_hPa  gas_ohm");
  Serial.println("-----------------------------");
}

void loop() {
  static unsigned long lastSample = 0;
  static unsigned long rowCount   = 0;

  unsigned long now_ms = millis();

  if (now_ms - lastSample >= INTERVAL_MS) {
    lastSample = now_ms;

    if (!bme.performReading()) {
      Serial.println("WARN — BME688 read failed, skipping this sample.");
      return;
    }

    DateTime dt = rtc.now();
    uint32_t ms_off = now_ms % 1000;

    float temp     = bme.temperature;
    float humidity = bme.humidity;
    float pressure = bme.pressure / 100.0; // Pa -> hPa
    float gasOhm   = bme.gas_resistance;   // ohms

    char iso[30];
    buildISO(dt, ms_off, iso, sizeof(iso));
    uint64_t epochMs = buildEpochMs(dt, ms_off);

    // iaq and iaq_accuracy left blank — raw mode per spec fallback
    char row[140];
    snprintf(row, sizeof(row),
      "%s,%llu,%.2f,%.2f,%.2f,%.0f,,",
      iso, epochMs, temp, humidity, pressure, gasOhm
    );
    dataFile.println(row);
    rowCount++;

    if (rowCount % 10 == 0) {
      dataFile.flush();
    }

    Serial.print(iso);
    Serial.print("   ");
    Serial.print(temp, 2); Serial.print("C   ");
    Serial.print(humidity, 2); Serial.print("%   ");
    Serial.print(pressure, 2); Serial.print("hPa   ");
    Serial.print(gasOhm, 0); Serial.println("ohm");
    Serial.print("Rows logged: "); Serial.println(rowCount);
  }
}