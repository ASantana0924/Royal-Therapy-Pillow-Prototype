#include <Arduino.h>
#include <Wire.h>
#include <SPI.h>
#include <SD.h>
#include <RTClib.h>
#include <Adafruit_ADXL345_U.h>
#include <Adafruit_BME680.h>
#include <driver/i2s.h>
#include <sys/time.h>
#include "../../src/logging/session.h"

// ============================================================
// Pillow Prototype Data Logger
// Ground-up replacement for dry_run.cpp
//
// Design goals:
//   - One immutable session configuration
//   - One shared monotonic session timestamp
//   - No continuously-open log file
//   - ACC / ENV / MIC are the only acquisition files kept open
//   - Metadata is written only after acquisition files close
//   - Explicit startup validation and shutdown
//   - ACC is the timing-sensitive I2C stream
//   - BME688 uses asynchronous beginReading/endReading so the I2C mutex
//     is NOT held during the gas-heater/measurement interval
//   - Audio is written as raw PCM (no header) — every byte is immediately valid
//   - PCM segments rotate on elapsed segment time (1 hour), not file size
//   - Convert PCM to WAV after collection using audit/pcm_to_wav.py
// ============================================================

// -------------------- Pin definitions -----------------------
#define SD_CS     4
#define SD_SCK    18
#define SD_MISO   19
#define SD_MOSI   23
#define SDA_PIN   21
#define SCL_PIN   22
#define PIN_BCLK  26
#define PIN_WS    25
#define PIN_DOUT  33
#define LED_PIN   2

// -------------------- Session configuration -----------------
// Update these before each planned night.
static const char* NIGHT        = "n01";
static const char* PILLOW_MODEL = "3-layer memory foam";
static const float ROOM_TEMP    = 24.0f;
static const char* CONDITIONS   = "no partner present, window closed, fan off";
static const char* PLACEMENT    = "Environment sensor in between middle and top layer near the side of the pillow ~10 cm from head";
static const char* NOTES        = "";

// 5-minute validation run. Change ONLY this value to perform the overnight test.
// #define RUN_DURATION_MS (60UL * 60UL * 1000UL)
#define RUN_DURATION_MS (7UL * 60UL * 60UL * 1000UL)

// -------------------- Sensor configuration ------------------
#define ACC_INTERVAL_MS 10UL
#define SAMPLE_RATE 16000
#define BITS_PER_SAMPLE 16
#define DMA_BUF_COUNT 16
#define DMA_BUF_LEN 1024
#define WRITE_BUF_SIZE 2048
#define GAIN_FACTOR 2
#define HPF_ALPHA 0.98f
#define NOISE_GATE_THRESHOLD 200

// -------------------- Audio buffering -----------------------
#define STAGE_BUF_SAMPLES 8192
static int16_t stageBuf[STAGE_BUF_SAMPLES];
static size_t stageBufIdx = 0;

#define SEGMENT_DURATION_S 3600UL
#define BYTES_PER_SAMPLE 2UL
#define WAV_PATCH_INTERVAL_MS (30UL * 60UL * 1000UL)
#define MIN_SEGMENT_REMAINING_MS (10UL * 60UL * 1000UL)

// -------------------- SD / startup ---------------------------
#define SD_SPI_HZ 4000000
#define ACC_OPEN_RETRIES 5
#define ACC_OPEN_DELAY_MS 500

// -------------------- Globals -------------------------------
RTC_DS3231 rtc;
Adafruit_ADXL345_Unified accel = Adafruit_ADXL345_Unified(12345);
Adafruit_BME680 bme;

File accFile;
File envFile;
File wavFile;
File indexFile;

SemaphoreHandle_t i2cMutex = nullptr;
SemaphoreHandle_t envMutex = nullptr;
TaskHandle_t envTaskHandle = nullptr;

struct EnvReading {
  float temp_c;
  float humidity_pct;
  float pressure_hpa;
  float gas_resistance_ohm;
  uint64_t epoch_ms;
  bool valid;
};

static EnvReading latestEnv = {0, 0, 0, 0, 0, false};

// BME688 asynchronous measurement uses beginReading()/endReading().
// The I2C mutex is held only for the short START/FINISH transactions;
// the ~300 ms gas-heater/measurement interval does NOT hold the bus.

// One immutable session time anchor.
static uint64_t sessionStartEpochMs = 0;
static uint32_t sessionStartMillis = 0;
static int sessionYear = 0;
static int sessionMonth = 0;
static int sessionDay = 0;

static uint32_t runStartMillis = 0;
static bool sessionRunning = false;
static bool shutdownComplete = false;
static bool i2sInstalled = false;

static uint8_t wavSegmentIndex = 0;
static uint8_t wavSegmentsCompleted = 0;
static uint32_t segmentBytesWritten = 0;
static uint32_t segmentOpenMs = 0;
static uint64_t segmentStartEpochMs = 0;
static uint32_t segmentStartMillis = 0;
static uint32_t segmentStartSampleCount = 0;
static uint32_t totalMicSamples = 0;

static uint32_t accRowCount = 0;
static uint32_t envRowCount = 0;
static uint32_t accWriteFailures = 0;
static uint32_t accMutexFailures = 0;
static uint32_t envReadFailures = 0;
static uint32_t envMutexFailures = 0;
static uint32_t envWriteFailures = 0;
static uint32_t micWriteFailures = 0;
static uint32_t wavOpenFailures = 0;
static uint32_t audioReadErrors = 0;

static uint32_t lastAccMillis = 0;
static uint32_t lastEnvAttemptMillis = 0;
static uint32_t lastStatusMillis = 0;
static uint32_t lastWavPatchMillis = 0;
static uint32_t lastBlinkMillis = 0;
static bool ledState = false;

static float hpf_prev_in = 0.0f;
static float hpf_prev_out = 0.0f;
static int32_t rawBuf[WRITE_BUF_SIZE];

// ============================================================
// Timestamp helpers
// ============================================================

static uint64_t getSessionEpochMs() {
  return sessionStartEpochMs + (uint32_t)(millis() - sessionStartMillis);
}

static void epochMsToDateTime(uint64_t epochMs, DateTime& dt) {
  uint32_t seconds = (uint32_t)(epochMs / 1000ULL);
  // Apply EDT offset (UTC-4) for ts_iso local time display.
  // ts_epoch_ms remains UTC. Change to -5*3600 after Nov DST end.
  const int32_t TZ_OFFSET_S = -4 * 3600;
  dt = DateTime((uint32_t)((int32_t)seconds + TZ_OFFSET_S));
}

static void formatTimestamp(uint64_t epochMs, char* iso, size_t len) {
  DateTime dt;
  epochMsToDateTime(epochMs, dt);
  uint16_t ms = (uint16_t)(epochMs % 1000ULL);
  snprintf(iso, len, "%04d-%02d-%02dT%02d:%02d:%02d.%03u",
           dt.year(), dt.month(), dt.day(),
           dt.hour(), dt.minute(), dt.second(), ms);
}

// ============================================================
// Filename helpers
// ============================================================

static void buildAccFilename(char* buf, size_t len) {
  snprintf(buf, len, "/PILLOW_%s_%04d%02d%02d_ACC_%s.csv",
           UNIT_ID, sessionYear, sessionMonth, sessionDay, NIGHT);
}

static void buildEnvFilename(char* buf, size_t len) {
  snprintf(buf, len, "/PILLOW_%s_%04d%02d%02d_ENV_%s.csv",
           UNIT_ID, sessionYear, sessionMonth, sessionDay, NIGHT);
}

static void buildIndexFilename(char* buf, size_t len) {
  snprintf(buf, len, "/PILLOW_%s_%04d%02d%02d_MIC_%s_index.csv",
           UNIT_ID, sessionYear, sessionMonth, sessionDay, NIGHT);
}

static void buildWavFilename(uint8_t segment, char* buf, size_t len) {
  // PCM format — no header, every byte written is immediately valid audio.
  // Convert to WAV after collection using audit/pcm_to_wav.py
  snprintf(buf, len, "/PILLOW_%s_%04d%02d%02d_MIC_%s_h%02u.pcm",
           UNIT_ID, sessionYear, sessionMonth, sessionDay, NIGHT, segment);
}

static void buildMetaFilename(char* buf, size_t len) {
  snprintf(buf, len, "/PILLOW_%s_%04d%02d%02d_meta.json",
           UNIT_ID, sessionYear, sessionMonth, sessionDay);
}

// ============================================================
// Serial logging only. No log file is held open.
// ============================================================

static void serialLog(const char* level, const char* msg) {
  char ts[32];
  formatTimestamp(getSessionEpochMs(), ts, sizeof(ts));
  Serial.printf("[%s] %s: %s\n", ts, level, msg);
}

static void logInfo(const char* msg)  { serialLog("INFO", msg); }
static void logWarn(const char* msg)  { serialLog("WARN", msg); }
static void logError(const char* msg) { serialLog("ERROR", msg); }

// ============================================================
// SD helpers
// ============================================================

static bool mountSD() {
  pinMode(SD_CS, OUTPUT);
  digitalWrite(SD_CS, HIGH);
  pinMode(SD_SCK, OUTPUT);
  digitalWrite(SD_SCK, HIGH);
  pinMode(SD_MOSI, OUTPUT);
  digitalWrite(SD_MOSI, HIGH);
  pinMode(SD_MISO, INPUT);

  delay(250);
  SPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);

  for (int attempt = 1; attempt <= 5; ++attempt) {
    Serial.printf("SD mount attempt %d/5...\n", attempt);
    if (SD.begin(SD_CS, SPI, SD_SPI_HZ)) {
      Serial.printf("microSD OK, size=%llu MB\n", (unsigned long long)(SD.cardSize() / (1024ULL * 1024ULL)));
      return true;
    }
    SD.end();
    delay(1000);
    SPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
  }
  return false;
}

static bool verifyFileSize(File& f, size_t minimumBytes) {
  if (!f) return false;
  f.flush();
  return f.size() >= minimumBytes;
}

// We deliberately do NOT delete old session files. Existing files are evidence.
// If the requested name already exists, fail rather than destroying it.
static bool ensureFreshSessionNames() {
  char fname[96];
  buildAccFilename(fname, sizeof(fname));
  if (SD.exists(fname)) {
    Serial.printf("FAIL — existing session file found: %s\n", fname);
    return false;
  }
  buildEnvFilename(fname, sizeof(fname));
  if (SD.exists(fname)) {
    Serial.printf("FAIL — existing session file found: %s\n", fname);
    return false;
  }
  buildIndexFilename(fname, sizeof(fname));
  if (SD.exists(fname)) {
    Serial.printf("FAIL — existing session file found: %s\n", fname);
    return false;
  }
  buildMetaFilename(fname, sizeof(fname));
  if (SD.exists(fname)) {
    Serial.printf("FAIL — existing metadata file found: %s\n", fname);
    return false;
  }
  for (uint8_t i = 0; i < 10; ++i) {
    buildWavFilename(i, fname, sizeof(fname));
    if (SD.exists(fname)) {
      Serial.printf("FAIL — existing WAV found: %s\n", fname);
      return false;
    }
  }
  return true;
}

// ============================================================
// WAV helpers
// ============================================================

static bool writeWavHeader(File& f, uint32_t dataBytes) {
  uint32_t sampleRate = SAMPLE_RATE;
  uint16_t channels = 1;
  uint16_t bits = BITS_PER_SAMPLE;
  uint32_t byteRate = sampleRate * channels * (bits / 8);
  uint16_t blockAlign = channels * (bits / 8);
  uint32_t chunkSize = 36 + dataBytes;
  uint32_t fmtSize = 16;
  uint16_t audioFormat = 1;

  if (f.write((const uint8_t*)"RIFF", 4) != 4) return false;
  if (f.write((const uint8_t*)&chunkSize, 4) != 4) return false;
  if (f.write((const uint8_t*)"WAVE", 4) != 4) return false;
  if (f.write((const uint8_t*)"fmt ", 4) != 4) return false;
  if (f.write((const uint8_t*)&fmtSize, 4) != 4) return false;
  if (f.write((const uint8_t*)&audioFormat, 2) != 2) return false;
  if (f.write((const uint8_t*)&channels, 2) != 2) return false;
  if (f.write((const uint8_t*)&sampleRate, 4) != 4) return false;
  if (f.write((const uint8_t*)&byteRate, 4) != 4) return false;
  if (f.write((const uint8_t*)&blockAlign, 2) != 2) return false;
  if (f.write((const uint8_t*)&bits, 2) != 2) return false;
  if (f.write((const uint8_t*)"data", 4) != 4) return false;
  if (f.write((const uint8_t*)&dataBytes, 4) != 4) return false;
  return true;
}

static bool patchWavHeader(File& f, uint32_t dataBytes) {
  if (!f) return false;
  uint32_t chunkSize = 36 + dataBytes;
  if (!f.seek(4)) return false;
  if (f.write((const uint8_t*)&chunkSize, 4) != 4) return false;
  if (!f.seek(40)) return false;
  if (f.write((const uint8_t*)&dataBytes, 4) != 4) return false;
  f.seek(f.size());
  return true;
}

static bool flushStageBuffer() {
  if (stageBufIdx == 0) return true;
  if (!wavFile) return false;

  const size_t bytes = stageBufIdx * BYTES_PER_SAMPLE;
  const size_t written = wavFile.write((const uint8_t*)stageBuf, bytes);
  if (written != bytes) {
    micWriteFailures++;
    Serial.printf("ERROR: WAV buffer write failed (%u/%u bytes)\n",
                  (unsigned)written, (unsigned)bytes);
    return false;
  }

  segmentBytesWritten += (uint32_t)bytes;
  stageBufIdx = 0;
  return true;
}

static bool closeCurrentWav(bool writeIndexRow) {
  if (!wavFile) return true;

  flushStageBuffer();

  const uint32_t finalBytes = segmentBytesWritten;
  const uint32_t finalSamples = finalBytes / BYTES_PER_SAMPLE;
  const uint64_t endEpochMs = getSessionEpochMs();
  const uint64_t durationMs = (endEpochMs >= segmentStartEpochMs)
                            ? (endEpochMs - segmentStartEpochMs) : 0;

  // PCM — no header to patch. Just flush and close.
  wavFile.flush();
  wavFile.close();
  bool headerOk = true;

  if (writeIndexRow && indexFile) {
    char wavName[96];
    char iso[32];
    char startIso[32];
    buildWavFilename(wavSegmentIndex, wavName, sizeof(wavName));
    formatTimestamp(segmentStartEpochMs, startIso, sizeof(startIso));

    // duration_s is based on actual samples written, not the planned 3600 s.
    const double durationSeconds = (double)finalSamples / (double)SAMPLE_RATE;
    char row[220];
    snprintf(row, sizeof(row), "%s,%s,%llu,%.3f,%d",
             wavName + 1,
             startIso,
             (unsigned long long)segmentStartEpochMs,
             durationSeconds,
             SAMPLE_RATE);
    if (indexFile.println(row) == 0) {
      logWarn("Failed to write MIC index row.");
    }
    indexFile.flush();
    (void)iso;
  }

  Serial.printf("PCM h%02u closed: %.2f MB, %lu samples\n",
                wavSegmentIndex,
                finalBytes / 1048576.0,
                (unsigned long)finalSamples);

  wavSegmentsCompleted++;
  return headerOk;
}

static bool openWavSegment() {
  if (wavFile) {
    closeCurrentWav(true);
  }

  if (wavSegmentIndex >= 10) {
    logError("WAV segment limit reached.");
    return false;
  }

  char wavName[96];
  buildWavFilename(wavSegmentIndex, wavName, sizeof(wavName));

  wavFile = SD.open(wavName, FILE_WRITE);
  if (!wavFile) {
    wavOpenFailures++;
    Serial.printf("ERROR: Could not open WAV: %s\n", wavName);
    return false;
  }

  segmentBytesWritten = 0;
  stageBufIdx = 0;
  hpf_prev_in = 0.0f;
  hpf_prev_out = 0.0f;
  segmentStartEpochMs = getSessionEpochMs();
  segmentStartMillis = millis();
  segmentStartSampleCount = totalMicSamples;

  // PCM — no header written. First byte is audio data.
  // File is valid from the moment it opens — no close dependency.
  Serial.printf("PCM h%02u opened: %s\n", wavSegmentIndex, wavName);
  lastWavPatchMillis = millis();
  segmentOpenMs = millis();
  return true;
}

// ============================================================
// File open helpers
// ============================================================

static bool openAccFile() {
  char fname[96];
  buildAccFilename(fname, sizeof(fname));

  for (int attempt = 1; attempt <= ACC_OPEN_RETRIES; ++attempt) {
    if (accFile) accFile.close();

    accFile = SD.open(fname, FILE_WRITE);
    if (!accFile) {
      Serial.printf("ACC open attempt %d/%d failed.\n", attempt, ACC_OPEN_RETRIES);
      delay(ACC_OPEN_DELAY_MS);
      continue;
    }

    if (accFile.println("ts_iso,ts_epoch_ms,accel_x_g,accel_y_g,accel_z_g") == 0) {
      accFile.close();
      delay(ACC_OPEN_DELAY_MS);
      continue;
    }
    accFile.flush();

    if (verifyFileSize(accFile, 10)) {
      Serial.printf("ACC file opened: %s (attempt %d)\n", fname, attempt);
      return true;
    }

    accFile.close();
    delay(ACC_OPEN_DELAY_MS);
  }

  logError("ACC file failed to open after all retries.");
  return false;
}

static bool openEnvFile() {
  char fname[96];
  buildEnvFilename(fname, sizeof(fname));
  envFile = SD.open(fname, FILE_WRITE);
  if (!envFile) return false;

  if (envFile.println("ts_iso,ts_epoch_ms,temp_C,humidity_pct,pressure_hPa,gas_resistance_ohm,iaq,iaq_accuracy") == 0) {
    envFile.close();
    return false;
  }
  envFile.flush();
  return verifyFileSize(envFile, 20);
}

static bool openIndexFile() {
  char fname[96];
  buildIndexFilename(fname, sizeof(fname));
  indexFile = SD.open(fname, FILE_WRITE);
  if (!indexFile) return false;

  if (indexFile.println("file_name,ts_iso_start,ts_epoch_ms_start,duration_s,sample_rate_hz") == 0) {
    indexFile.close();
    return false;
  }
  indexFile.flush();
  return verifyFileSize(indexFile, 20);
}

// ============================================================
// Sensor initialization
// ============================================================

static bool initRTC() {
  if (xSemaphoreTake(i2cMutex, pdMS_TO_TICKS(250)) != pdTRUE) return false;
  bool ok = rtc.begin();
  xSemaphoreGive(i2cMutex);
  if (!ok) return false;

  if (rtc.lostPower()) {
    Serial.println("WARN: DS3231 reports lost power. Use set_rtc.py before testing.");
  }

  waitForTimeSync(rtc, 10000);
  return true;
}

static bool initAccelerometer() {
  if (xSemaphoreTake(i2cMutex, pdMS_TO_TICKS(250)) != pdTRUE) return false;
  bool ok = accel.begin();
  if (ok) {
    accel.setRange(ADXL345_RANGE_16_G);
    accel.setDataRate(ADXL345_DATARATE_100_HZ);
  }
  xSemaphoreGive(i2cMutex);
  return ok;
}

static bool initEnvironmentSensor() {
  if (xSemaphoreTake(i2cMutex, pdMS_TO_TICKS(250)) != pdTRUE) return false;
  bool ok = bme.begin(0x77);
  if (!ok) ok = bme.begin(0x76);
  if (ok) {
    bme.setTemperatureOversampling(BME680_OS_8X);
    bme.setHumidityOversampling(BME680_OS_2X);
    bme.setPressureOversampling(BME680_OS_4X);
    bme.setIIRFilterSize(BME680_FILTER_SIZE_3);
    bme.setGasHeater(400, 300);
  }
  xSemaphoreGive(i2cMutex);
  return ok;
}

static bool initI2S() {
  i2s_config_t cfg = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
    .sample_rate = SAMPLE_RATE,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT,
    .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,
    .communication_format = I2S_COMM_FORMAT_STAND_I2S,
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count = DMA_BUF_COUNT,
    .dma_buf_len = DMA_BUF_LEN,
    .use_apll = false,
    .tx_desc_auto_clear = false,
    .fixed_mclk = 0
  };

  i2s_pin_config_t pins = {
    .bck_io_num = PIN_BCLK,
    .ws_io_num = PIN_WS,
    .data_out_num = I2S_PIN_NO_CHANGE,
    .data_in_num = PIN_DOUT
  };

  esp_err_t err = i2s_driver_install(I2S_NUM_0, &cfg, 0, nullptr);
  if (err != ESP_OK) {
    Serial.printf("FAIL: i2s_driver_install error %d\n", (int)err);
    return false;
  }

  err = i2s_set_pin(I2S_NUM_0, &pins);
  if (err != ESP_OK) {
    Serial.printf("FAIL: i2s_set_pin error %d\n", (int)err);
    i2s_driver_uninstall(I2S_NUM_0);
    return false;
  }

  i2s_zero_dma_buffer(I2S_NUM_0);
  i2sInstalled = true;
  return true;
}

// ============================================================
// Session clock initialization
// ============================================================

static bool captureSessionClock() {
  if (xSemaphoreTake(i2cMutex, pdMS_TO_TICKS(250)) != pdTRUE) return false;

  DateTime now = rtc.now();
  uint32_t ms = millis();

  sessionStartEpochMs = (uint64_t)now.unixtime() * 1000ULL;
  sessionStartMillis = ms;
  sessionYear = now.year();
  sessionMonth = now.month();
  sessionDay = now.day();

  xSemaphoreGive(i2cMutex);

  // Keep the ESP32 system clock synchronized with the same RTC anchor.
  struct timeval tv;
  tv.tv_sec = (time_t)(sessionStartEpochMs / 1000ULL);
  tv.tv_usec = 0;
  settimeofday(&tv, nullptr);

  char ts[32];
  formatTimestamp(sessionStartEpochMs, ts, sizeof(ts));
  Serial.printf("Session clock anchored: %s\n", ts);
  return true;
}

// ============================================================
// Environment task
// ============================================================

static void envTask(void* param) {
  (void)param;

  // Keep ENV on Core 0 at a low priority. ACC runs in the Arduino loop on
  // Core 1, and the shared I2C mutex is held only for short transactions.
  TickType_t lastWake = xTaskGetTickCount();

  while (sessionRunning) {
    // ----------------------------------------------------------
    // Phase 1: start a forced BME measurement.
    // ----------------------------------------------------------
    // beginReading() starts the measurement and returns the time at which
    // the measurement should be complete. It does NOT require us to hold
    // the I2C mutex during the heater/measurement interval.
    uint32_t measurementEnd = 0;

    if (xSemaphoreTake(i2cMutex, pdMS_TO_TICKS(2)) == pdTRUE) {
      measurementEnd = bme.beginReading();
      xSemaphoreGive(i2cMutex);
    } else {
      envMutexFailures++;
    }

    if (measurementEnd == 0) {
      envReadFailures++;
    } else {
      // --------------------------------------------------------
      // Phase 2: wait WITHOUT holding the I2C mutex.
      // --------------------------------------------------------
      // This is the key scheduling change. The BME gas heater can run
      // for ~300 ms, but ACC remains free to use I2C during this time.
      const uint32_t now = millis();
      if ((int32_t)(measurementEnd - now) > 0) {
        vTaskDelay(pdMS_TO_TICKS(measurementEnd - now));
      }

      // Do not call endReading() until the measurement is actually ready.
      // Adafruit's endReading() can block if called early, and we never
      // want that wait to happen while holding the shared I2C mutex.
      int remainingMs = bme.remainingReadingMillis();
      while (remainingMs > 0 && sessionRunning) {
        vTaskDelay(pdMS_TO_TICKS(remainingMs));
        remainingMs = bme.remainingReadingMillis();
      }

      // --------------------------------------------------------
      // Phase 3: finish/read the measurement.
      // --------------------------------------------------------
      bool ok = false;
      if (sessionRunning &&
          xSemaphoreTake(i2cMutex, pdMS_TO_TICKS(2)) == pdTRUE) {
        ok = bme.endReading();
        if (ok) {
          const uint64_t measurementEpoch = getSessionEpochMs();

          if (xSemaphoreTake(envMutex, pdMS_TO_TICKS(2)) == pdTRUE) {
            latestEnv.temp_c = bme.temperature;
            latestEnv.humidity_pct = bme.humidity;
            latestEnv.pressure_hpa = bme.pressure / 100.0f;
            latestEnv.gas_resistance_ohm = bme.gas_resistance;
            latestEnv.epoch_ms = measurementEpoch;
            latestEnv.valid = true;
            xSemaphoreGive(envMutex);
          } else {
            envMutexFailures++;
          }
        }
        xSemaphoreGive(i2cMutex);
      } else {
        envMutexFailures++;
      }

      if (!ok) {
        envReadFailures++;
      }

    }

    // Exactly one ENV acquisition cycle per second.
    vTaskDelayUntil(&lastWake, pdMS_TO_TICKS(1000));
  }

  envTaskHandle = nullptr;
  vTaskDelete(nullptr);
}

// ============================================================
// ACC acquisition
// ============================================================

static void acquireAccelerometer(uint32_t nowMs) {
  if (!accFile) return;

  if ((uint32_t)(nowMs - lastAccMillis) < ACC_INTERVAL_MS) return;
  lastAccMillis += ACC_INTERVAL_MS;

  // If loop execution was delayed, don't generate a burst of synthetic rows.
  if ((uint32_t)(nowMs - lastAccMillis) > 100UL) {
    lastAccMillis = nowMs;
  }

  sensors_event_t event;

  // ACC is the timing-sensitive stream. The ENV task only holds this mutex
  // for short BME register transactions, so ACC should never wait tens of
  // milliseconds for the bus. A short timeout prevents a transient I2C
  // conflict from stalling the 100 Hz loop.
  if (xSemaphoreTake(i2cMutex, pdMS_TO_TICKS(2)) != pdTRUE) {
    accMutexFailures++;
    return;
  }
  accel.getEvent(&event);
  xSemaphoreGive(i2cMutex);

  const uint64_t epochMs = getSessionEpochMs();
  char iso[32];
  formatTimestamp(epochMs, iso, sizeof(iso));

  const float ax = event.acceleration.x / 9.80665f;
  const float ay = event.acceleration.y / 9.80665f;
  const float az = event.acceleration.z / 9.80665f;

  char row[128];
  const int n = snprintf(row, sizeof(row), "%s,%llu,%.4f,%.4f,%.4f",
                         iso,
                         (unsigned long long)epochMs,
                         ax, ay, az);
  if (n <= 0 || n >= (int)sizeof(row)) {
    accWriteFailures++;
    return;
  }

  if (accFile.println(row) == 0) {
    accWriteFailures++;
    return;
  }

  accRowCount++;
  if ((accRowCount % 250) == 0) {
    accFile.flush();
  }
}

// ============================================================
// ENV acquisition/write
// ============================================================

static void writeEnvironmentReading() {
  if (!envFile) return;

  EnvReading reading;
  bool haveReading = false;

  if (xSemaphoreTake(envMutex, 0) == pdTRUE) {
    if (latestEnv.valid) {
      reading = latestEnv;
      latestEnv.valid = false;
      haveReading = true;
    }
    xSemaphoreGive(envMutex);
  }

  if (!haveReading) return;

  char iso[32];
  formatTimestamp(reading.epoch_ms, iso, sizeof(iso));

  char row[180];
  const int n = snprintf(row, sizeof(row), "%s,%llu,%.2f,%.2f,%.2f,%.0f,,",
                         iso,
                         (unsigned long long)reading.epoch_ms,
                         reading.temp_c,
                         reading.humidity_pct,
                         reading.pressure_hpa,
                         reading.gas_resistance_ohm);
  if (n <= 0 || n >= (int)sizeof(row)) {
    envWriteFailures++;
    return;
  }

  if (envFile.println(row) == 0) {
    envWriteFailures++;
    return;
  }

  envRowCount++;
  if ((envRowCount % 30) == 0) {
    envFile.flush();
  }
}

// ============================================================
// Audio acquisition
// ============================================================

static void acquireMicrophone() {
  if (!i2sInstalled || !wavFile) return;

  size_t bytesRead = 0;
  const esp_err_t err = i2s_read(I2S_NUM_0,
                                 rawBuf,
                                 sizeof(rawBuf),
                                 &bytesRead,
                                 0);
  if (err != ESP_OK) {
    audioReadErrors++;
    return;
  }
  if (bytesRead == 0) return;

  const int samplesRead = bytesRead / sizeof(int32_t);
  const int monoSamples = samplesRead / 2;

  for (int i = 0; i < monoSamples; ++i) {
    const int32_t raw = rawBuf[i * 2] >> 8;
    const float in = (float)raw;
    const float out = HPF_ALPHA * (hpf_prev_out + in - hpf_prev_in);
    hpf_prev_in = in;
    hpf_prev_out = out;

    int32_t sample = (int32_t)(out * GAIN_FACTOR);
    if (sample > 32767) sample = 32767;
    if (sample < -32768) sample = -32768;
    if (abs(sample) < NOISE_GATE_THRESHOLD) sample = 0;

    stageBuf[stageBufIdx++] = (int16_t)sample;
    totalMicSamples++;

    if (stageBufIdx >= STAGE_BUF_SAMPLES) {
      if (!flushStageBuffer()) {
        // Do not pretend the data was successfully written.
        // The counter records the failed write.
      }
    }
  }
}

// ============================================================
// WAV segment management
// ============================================================

static bool maybeRotateWav(uint32_t nowMs) {
  if (!wavFile) return false;

  // Time-based rotation — immune to I2S clock drift
  const uint32_t segmentElapsed = nowMs - segmentOpenMs;
  if (segmentElapsed < SEGMENT_DURATION_S * 1000UL) return false;

  const uint32_t sessionRemaining = (nowMs - runStartMillis < RUN_DURATION_MS)
    ? RUN_DURATION_MS - (nowMs - runStartMillis) : 0;
  if (sessionRemaining <= MIN_SEGMENT_REMAINING_MS) return false;

  if (wavSegmentIndex >= 9) {
    logError("Cannot create another PCM segment: segment limit reached.");
    return false;
  }

  closeCurrentWav(true);
  ++wavSegmentIndex;
  return openWavSegment();
}

// periodicWavPatch removed — not needed with PCM format.
// PCM has no header dependency; every byte written is immediately valid.

// ============================================================
// Startup verification
// ============================================================

static bool writeFirstAccRow() {
  if (!accFile) return false;

  sensors_event_t event;
  if (xSemaphoreTake(i2cMutex, pdMS_TO_TICKS(100)) != pdTRUE) {
    accMutexFailures++;
    return false;
  }
  accel.getEvent(&event);
  xSemaphoreGive(i2cMutex);

  const uint64_t epochMs = getSessionEpochMs();
  char iso[32];
  formatTimestamp(epochMs, iso, sizeof(iso));

  char row[128];
  const int n = snprintf(row, sizeof(row), "%s,%llu,%.4f,%.4f,%.4f",
                         iso,
                         (unsigned long long)epochMs,
                         event.acceleration.x / 9.80665f,
                         event.acceleration.y / 9.80665f,
                         event.acceleration.z / 9.80665f);
  if (n <= 0 || n >= (int)sizeof(row)) return false;

  if (accFile.println(row) == 0) return false;
  accFile.flush();
  if (accFile.size() <= 10) return false;

  accRowCount++;
  return true;
}

// ============================================================
// Metadata
// ============================================================

static bool writeMetadataJson(uint64_t endEpochMs) {
  // At this point ALL acquisition files must already be closed.
  char filename[96];
  buildMetaFilename(filename, sizeof(filename));

  File f = SD.open(filename, FILE_WRITE);
  if (!f) {
    Serial.printf("FAIL — could not create meta.json: %s\n", filename);
    return false;
  }

  char startIso[32];
  char endIso[32];
  formatTimestamp(sessionStartEpochMs, startIso, sizeof(startIso));
  formatTimestamp(endEpochMs, endIso, sizeof(endIso));

  const uint64_t durationMs = (endEpochMs >= sessionStartEpochMs)
                            ? endEpochMs - sessionStartEpochMs : 0;

  f.println("{");
  f.printf("  \"prototype\": \"PILLOW\",\n");
  f.printf("  \"unit_id\": \"%s\",\n", UNIT_ID);
  f.printf("  \"session_date\": \"%04d-%02d-%02d\",\n", sessionYear, sessionMonth, sessionDay);
  f.printf("  \"night\": \"%s\",\n", NIGHT);
  f.printf("  \"firmware_version\": \"%s\",\n", FW_VERSION);
  f.printf("  \"start_ts_iso\": \"%s\",\n", startIso);
  f.printf("  \"end_ts_iso\": \"%s\",\n", endIso);
  f.printf("  \"start_ts_epoch_ms\": %llu,\n", (unsigned long long)sessionStartEpochMs);
  f.printf("  \"end_ts_epoch_ms\": %llu,\n", (unsigned long long)endEpochMs);
  f.printf("  \"duration_s\": %.3f,\n", durationMs / 1000.0);

  f.println("  \"sensors\": {");
  f.printf("    \"ADXL345\": {\"stream\": \"ACC\", \"rate_hz\": %d, \"range_g\": 16},\n", SENSOR_RATE_ACC);
  f.printf("    \"INMP441\": {\"stream\": \"MIC\", \"rate_hz\": %d, \"bits\": 16, \"channels\": 1, \"format\": \"pcm_s16le\"},\n", SENSOR_RATE_MIC);
  f.printf("    \"BME688\": {\"stream\": \"ENV\", \"rate_hz\": %d, \"bsec\": false, \"async_measurement\": true}\n", SENSOR_RATE_ENV);
  f.println("  },");

  f.printf("  \"placement\": \"%s\",\n", PLACEMENT);
  f.printf("  \"pillow_model\": \"%s\",\n", PILLOW_MODEL);
  f.printf("  \"room_temp_c\": %.1f,\n", ROOM_TEMP);
  f.printf("  \"conditions\": \"%s\",\n", CONDITIONS);
  f.printf("  \"notes\": \"%s\",\n", NOTES);

  f.println("  \"quality\": {");
  f.printf("    \"acc_rows\": %lu,\n", (unsigned long)accRowCount);
  f.printf("    \"env_rows\": %lu,\n", (unsigned long)envRowCount);
  f.printf("    \"mic_samples\": %lu,\n", (unsigned long)totalMicSamples);
  f.printf("    \"acc_write_failures\": %lu,\n", (unsigned long)accWriteFailures);
  f.printf("    \"acc_mutex_failures\": %lu,\n", (unsigned long)accMutexFailures);
  f.printf("    \"env_read_failures\": %lu,\n", (unsigned long)envReadFailures);
  f.printf("    \"env_mutex_failures\": %lu,\n", (unsigned long)envMutexFailures);
  f.printf("    \"env_write_failures\": %lu,\n", (unsigned long)envWriteFailures);
  f.printf("    \"mic_write_failures\": %lu,\n", (unsigned long)micWriteFailures);
  f.printf("    \"audio_read_errors\": %lu,\n", (unsigned long)audioReadErrors);
  f.printf("    \"wav_open_failures\": %lu\n", (unsigned long)wavOpenFailures);
  f.println("  },");

  f.printf("  \"wav_segments\": %u\n", (unsigned)wavSegmentsCompleted);
  f.println("}");
  f.flush();

  const size_t finalSize = f.size();
  f.close();

  if (finalSize < 100) {
    Serial.printf("FAIL — meta.json appears incomplete (%u bytes).\n", (unsigned)finalSize);
    return false;
  }

  Serial.printf("meta.json written: %s (%u bytes)\n", filename, (unsigned)finalSize);
  return true;
}

// ============================================================
// Shutdown
// ============================================================

static void stopEnvironmentTask() {
  sessionRunning = false;
  if (envTaskHandle != nullptr) {
    // Give the task a moment to leave performReading cleanly.
    for (int i = 0; i < 20 && envTaskHandle != nullptr; ++i) {
      delay(10);
    }
    if (envTaskHandle != nullptr) {
      vTaskDelete(envTaskHandle);
      envTaskHandle = nullptr;
    }
  }
}

static void closeAllAcquisitionFiles() {
  // Audio first because it has RAM buffered samples and a mutable WAV header.
  if (wavFile) {
    closeCurrentWav(true);
  }

  if (accFile) {
    accFile.flush();
    accFile.close();
  }

  if (envFile) {
    envFile.flush();
    envFile.close();
  }

  if (indexFile) {
    indexFile.flush();
    indexFile.close();
  }
}

static void shutdownSession() {
  if (shutdownComplete) return;
  shutdownComplete = true;

  Serial.println("\n===================================");
  Serial.println("SESSION SHUTDOWN");
  Serial.println("===================================");

  sessionRunning = false;
  stopEnvironmentTask();

  const uint64_t endEpochMs = getSessionEpochMs();

  // Stop I2S before closing the WAV so no more audio is being acquired.
  if (i2sInstalled) {
    i2s_stop(I2S_NUM_0);
  }

  closeAllAcquisitionFiles();

  // Metadata is deliberately written AFTER all acquisition files close.
  const bool metaOk = writeMetadataJson(endEpochMs);

  if (i2sInstalled) {
    i2s_driver_uninstall(I2S_NUM_0);
    i2sInstalled = false;
  }

  if (envMutex) {
    vSemaphoreDelete(envMutex);
    envMutex = nullptr;
  }
  if (i2cMutex) {
    vSemaphoreDelete(i2cMutex);
    i2cMutex = nullptr;
  }

  SD.end();
  SPI.end();

  digitalWrite(LED_PIN, LOW);

  Serial.printf("ACC rows: %lu\n", (unsigned long)accRowCount);
  Serial.printf("ENV rows: %lu\n", (unsigned long)envRowCount);
  Serial.printf("MIC samples: %lu\n", (unsigned long)totalMicSamples);
  Serial.printf("ACC write failures: %lu\n", (unsigned long)accWriteFailures);
  Serial.printf("ACC mutex failures: %lu\n", (unsigned long)accMutexFailures);
  Serial.printf("ENV read failures: %lu\n", (unsigned long)envReadFailures);
  Serial.printf("ENV write failures: %lu\n", (unsigned long)envWriteFailures);
  Serial.printf("MIC write failures: %lu\n", (unsigned long)micWriteFailures);
  Serial.printf("Audio read errors: %lu\n", (unsigned long)audioReadErrors);
  Serial.printf("WAV open failures: %lu\n", (unsigned long)wavOpenFailures);
  Serial.printf("Metadata: %s\n", metaOk ? "OK" : "FAILED");
  Serial.println("All acquisition files closed.");
  Serial.println("Session complete.");

  while (true) {
    delay(1000);
  }
}

// ============================================================
// Setup
// ============================================================

void setup() {
  Serial.begin(115200);
  delay(1000);

  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, HIGH);

  Serial.println("===============================================");
  Serial.printf("Pillow Prototype Logger | Unit %s | %s\n", UNIT_ID, FW_VERSION);
  Serial.printf("Night: %s | Pillow: %s\n", NIGHT, PILLOW_MODEL);
  Serial.println("Ground-up deterministic logger");
  Serial.println("===============================================");

  i2cMutex = xSemaphoreCreateMutex();
  envMutex = xSemaphoreCreateMutex();
  if (!i2cMutex || !envMutex) {
    Serial.println("FAIL — mutex creation failed.");
    while (true) delay(1000);
  }

  Wire.begin(SDA_PIN, SCL_PIN);
  // Both the ADXL345 and BME688 support Fast-mode I2C. Keep bus
  // transactions short so the 100 Hz ACC stream gets priority.
  Wire.setClock(400000);

  // RTC must be valid before session naming/timestamps.
  if (!initRTC()) {
    Serial.println("FAIL — DS3231 initialization.");
    while (true) delay(1000);
  }

  if (!captureSessionClock()) {
    Serial.println("FAIL — could not capture session clock.");
    while (true) delay(1000);
  }

  setenv("TZ", "EST5EDT,M3.2.0,M11.1.0", 1);
  tzset();

  // Hardware validation before creating data files.
  if (!initAccelerometer()) {
    Serial.println("FAIL — ADXL345 not detected.");
    while (true) delay(1000);
  }
  Serial.println("ADXL345 OK");

  if (!initEnvironmentSensor()) {
    Serial.println("FAIL — BME688 not detected.");
    while (true) delay(1000);
  }
  Serial.println("BME688 OK");

  if (!mountSD()) {
    Serial.println("FAIL — microSD mount failed.");
    while (true) delay(1000);
  }

  // Never destroy existing raw data automatically.
  if (!ensureFreshSessionNames()) {
    Serial.println("FAIL — session filenames already exist.");
    Serial.println("Change NIGHT/session configuration or use a clean SD card.");
    SD.end();
    while (true) delay(1000);
  }

  if (!initI2S()) {
    Serial.println("FAIL — I2S initialization.");
    SD.end();
    while (true) delay(1000);
  }
  Serial.println("I2S OK");

  // Open only the acquisition files. No session log file is held open.
  if (!openAccFile()) {
    Serial.println("FAIL — ACC file could not be created.");
    shutdownSession();
  }
  Serial.println("ACC file ready");

  if (!openEnvFile()) {
    Serial.println("FAIL — ENV file could not be created.");
    shutdownSession();
  }
  Serial.println("ENV file ready");

  if (!openIndexFile()) {
    Serial.println("FAIL — MIC index file could not be created.");
    shutdownSession();
  }
  Serial.println("MIC index ready");

  // Critical startup gate: ACC must produce an actual data row.
  if (!writeFirstAccRow()) {
    Serial.println("FAIL — first ACC row could not be written.");
    shutdownSession();
  }
  Serial.println("First ACC row confirmed");

  // Start the BME task only after the SD files and first ACC row are valid.
  sessionRunning = true;
  BaseType_t taskResult = xTaskCreatePinnedToCore(
    envTask,
    "envTask",
    4096,
    nullptr,
    1,  // Low-priority ENV task; ACC remains timing-sensitive.
    &envTaskHandle,
    0
  );

  if (taskResult != pdPASS) {
    Serial.println("FAIL — could not start ENV task.");
    shutdownSession();
  }
  Serial.println("ENV task running on Core 0");

  // Open first WAV only after all other acquisition streams are ready.
  if (!openWavSegment()) {
    Serial.println("FAIL — first WAV could not be opened.");
    shutdownSession();
  }

  runStartMillis = millis();
  lastAccMillis = runStartMillis;
  lastEnvAttemptMillis = runStartMillis;
  lastStatusMillis = runStartMillis;
  lastWavPatchMillis = runStartMillis;
  lastBlinkMillis = runStartMillis;

  Serial.printf("ALL STREAMS LOGGING — %lu minute run\n",
                RUN_DURATION_MS / 60000UL);
  Serial.println("Metadata will be written after acquisition files close.");
  Serial.println("===============================================\n");
}

// ============================================================
// Main loop
// ============================================================

void loop() {
  if (shutdownComplete) {
    delay(1000);
    return;
  }

  const uint32_t nowMs = millis();

  // Heartbeat
  if ((uint32_t)(nowMs - lastBlinkMillis) >= 1000UL) {
    lastBlinkMillis = nowMs;
    ledState = !ledState;
    digitalWrite(LED_PIN, ledState ? HIGH : LOW);
  }

  if ((uint32_t)(nowMs - runStartMillis) >= RUN_DURATION_MS) {
    shutdownSession();
    return;
  }

  // ACC is time-driven at 100 Hz.
  acquireAccelerometer(nowMs);

  // ENV result is produced by Core 0 and consumed here.
  writeEnvironmentReading();

  // MIC acquisition runs continuously from I2S.
  acquireMicrophone();

  // Rotate based on one hour of elapsed segment time and only when
  // enough session time remains for the next segment.
  maybeRotateWav(nowMs);

  // Periodically patch the WAV header without closing the file.
  //periodicWavPatch(nowMs);

  // Status every 30 seconds for the 5-minute test, 30 minutes overnight.
  const uint32_t statusInterval = (RUN_DURATION_MS <= 600000UL)
                                 ? 30000UL
                                 : 1800000UL;
  if ((uint32_t)(nowMs - lastStatusMillis) >= statusInterval) {
    lastStatusMillis = nowMs;
    const uint32_t elapsed = (nowMs - runStartMillis) / 1000UL;
    const uint32_t remaining = (elapsed < RUN_DURATION_MS / 1000UL)
                             ? (RUN_DURATION_MS / 1000UL - elapsed) : 0;
    Serial.printf("STATUS | elapsed=%lus remaining=%lus ACC=%lu ENV=%lu MIC=%lu WAV=%.2fMB heap=%u\n",
                  (unsigned long)elapsed,
                  (unsigned long)remaining,
                  (unsigned long)accRowCount,
                  (unsigned long)envRowCount,
                  (unsigned long)totalMicSamples,
                  segmentBytesWritten / 1048576.0,
                  ESP.getFreeHeap());
  }

  // Prevent the loop from monopolizing the scheduler while still allowing
  // I2S to be drained aggressively.
  taskYIELD();
}
