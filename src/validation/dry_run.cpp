#include <Arduino.h>
#include <Wire.h>
#include <SPI.h>
#include <SD.h>
#include <RTClib.h>
#include <Adafruit_ADXL345_U.h>
#include <Adafruit_BME680.h>
#include <driver/i2s.h>
#include "../../src/logging/session.h"

// ── Pin definitions ──────────────────────────────────────
#define SD_CS     5
#define SDA_PIN   21
#define SCL_PIN   22
#define PIN_BCLK  26
#define PIN_WS    25
#define PIN_DOUT  33
#define LED_PIN   2   // onboard LED

// ── Session config ───────────────────────────────────────
// ↓↓ UPDATE THESE BEFORE EACH NIGHT ↓↓
const char* NIGHT        = "n04";
const char* PILLOW_MODEL = "shredded memory foam queen";
const float ROOM_TEMP    = 23.0;
const char* CONDITIONS   = "fan on, no partner present, windows closed";
const char* PLACEMENT    = "sensor at pillow top edge ~10 cm from head";

// ── Run duration ─────────────────────────────────────────
#define RUN_DURATION_MS  (7UL * 60UL * 60UL * 1000UL)  // 7 hours

// ── ACC config ───────────────────────────────────────────
#define ACC_INTERVAL_MS  10  // 100 Hz

// ── Audio config ─────────────────────────────────────────
#define SAMPLE_RATE           16000
#define BITS_PER_SAMPLE       16
#define DMA_BUF_COUNT         16
#define DMA_BUF_LEN           1024
#define WRITE_BUF_SIZE        2048
#define GAIN_FACTOR           3
#define HPF_ALPHA             0.98f
#define NOISE_GATE_THRESHOLD  200

// ── Double buffer config ─────────────────────────────────
#define STAGE_BUF_SAMPLES  8192
#define NUM_BUFS           2

// ── Segment config ───────────────────────────────────────
#define SEGMENT_DURATION_S  3600
#define BYTES_PER_SAMPLE    2
#define BYTES_PER_SECOND    (SAMPLE_RATE * BYTES_PER_SAMPLE)
#define BYTES_PER_SEGMENT   ((uint32_t)BYTES_PER_SECOND * SEGMENT_DURATION_S)

// ── Globals ──────────────────────────────────────────────
RTC_DS3231               rtc;
Adafruit_ADXL345_Unified accel = Adafruit_ADXL345_Unified(12345);
Adafruit_BME680          bme;

File accFile;
File envFile;
File wavFile;
File indexFile;

uint32_t segmentBytesWritten = 0;
uint8_t  hourIndex           = 0;
uint32_t sessionStartEpoch   = 0;
unsigned long runStartMs     = 0;

int32_t rawBuf[WRITE_BUF_SIZE];

// ── Double buffer ─────────────────────────────────────────
int16_t   audioBufs[NUM_BUFS][STAGE_BUF_SAMPLES];
int       audioBufLens[NUM_BUFS]  = {0, 0};
volatile int  fillBuf             = 0;
volatile bool bufReady[NUM_BUFS]  = {false, false};

// ── Audio task + mutex ────────────────────────────────────
TaskHandle_t      audioTaskHandle = NULL;
SemaphoreHandle_t wavMutex        = NULL;

// ── HPF state ────────────────────────────────────────────
float hpf_prev_in  = 0.0f;
float hpf_prev_out = 0.0f;

// ── Row counters ─────────────────────────────────────────
uint32_t accRowCount = 0;
uint32_t envRowCount = 0;

// ── I2S installed flag ────────────────────────────────────
bool i2sInstalled = false;

// ── LED blink state ───────────────────────────────────────
bool          ledState  = false;
unsigned long lastBlink = 0;

// ── Helpers ──────────────────────────────────────────────
void buildISO(DateTime dt, uint32_t ms_off, char* buf, size_t len) {
  snprintf(buf, len,
    "%04d-%02d-%02dT%02d:%02d:%02d.%03lu",
    dt.year(), dt.month(),  dt.day(),
    dt.hour(), dt.minute(), dt.second(),
    (unsigned long)(ms_off % 1000)
  );
}

uint64_t toEpochMs(DateTime dt, uint32_t ms_off) {
  return ((uint64_t)dt.unixtime() * 1000ULL) + (ms_off % 1000);
}

// ── Peripheral teardown ───────────────────────────────────
void teardownPeripherals() {
  Serial.println("Tearing down peripherals...");

  if (audioTaskHandle != NULL) {
    vTaskDelete(audioTaskHandle);
    audioTaskHandle = NULL;
  }
  if (wavMutex != NULL) {
    vSemaphoreDelete(wavMutex);
    wavMutex = NULL;
  }
  if (i2sInstalled) {
    i2s_driver_uninstall(I2S_NUM_0);
    i2sInstalled = false;
  }

  SD.end();
  SPI.end();

  // LED off — session ended or failed
  digitalWrite(LED_PIN, LOW);

  Serial.println("Peripherals torn down — safe to power down or reflash.");
}

// ── SD mount with retry ───────────────────────────────────
bool mountSD() {
  pinMode(SD_CS, OUTPUT);
  digitalWrite(SD_CS, HIGH);
  delay(500);
  SPI.begin(18, 19, 23, SD_CS);

  for (int attempt = 1; attempt <= 5; attempt++) {
    Serial.printf("SD mount attempt %d/5...\n", attempt);
    if (SD.begin(SD_CS, SPI, 4000000)) {
      Serial.println("microSD OK");
      return true;
    }
    Serial.println("Mount failed — retrying in 2 seconds.");
    SD.end();
    delay(2000);
    SPI.begin(18, 19, 23, SD_CS);
  }
  return false;
}

// ── SD remount after write error ─────────────────────────
bool remountSD() {
  Serial.println("WARN — SD error, attempting remount...");
  SD.end();
  delay(1000);
  SPI.begin(18, 19, 23, SD_CS);
  if (!SD.begin(SD_CS, SPI, 4000000)) {
    Serial.println("FAIL — SD remount failed.");
    return false;
  }
  Serial.println("SD remounted successfully.");
  return true;
}

// ── WAV header ───────────────────────────────────────────
void writeWavHeader(File& f, uint32_t dataBytes) {
  uint32_t sampleRate    = SAMPLE_RATE;
  uint16_t numChannels   = 1;
  uint16_t bitsPerSample = BITS_PER_SAMPLE;
  uint32_t byteRate      = sampleRate * numChannels * (bitsPerSample / 8);
  uint16_t blockAlign    = numChannels * (bitsPerSample / 8);
  uint32_t chunkSize     = 36 + dataBytes;

  f.write((uint8_t*)"RIFF", 4);
  f.write((uint8_t*)&chunkSize,     4);
  f.write((uint8_t*)"WAVE",         4);
  f.write((uint8_t*)"fmt ",         4);
  uint32_t fmtSize = 16;
  f.write((uint8_t*)&fmtSize,       4);
  uint16_t audioFmt = 1;
  f.write((uint8_t*)&audioFmt,      2);
  f.write((uint8_t*)&numChannels,   2);
  f.write((uint8_t*)&sampleRate,    4);
  f.write((uint8_t*)&byteRate,      4);
  f.write((uint8_t*)&blockAlign,    2);
  f.write((uint8_t*)&bitsPerSample, 2);
  f.write((uint8_t*)"data",         4);
  f.write((uint8_t*)&dataBytes,     4);
}

void patchWavHeader(File& f, uint32_t dataBytes) {
  uint32_t chunkSize = 36 + dataBytes;
  f.seek(4);
  f.write((uint8_t*)&chunkSize, 4);
  f.seek(40);
  f.write((uint8_t*)&dataBytes, 4);
  f.seek(f.size());
}

// ── Flush staging buffer ──────────────────────────────────
void flushStageBuf() {
  if (wavMutex == NULL) return;
  if (xSemaphoreTake(wavMutex, portMAX_DELAY) == pdTRUE) {
    if (audioBufLens[fillBuf] > 0 && wavFile) {
      uint32_t writeBytes = audioBufLens[fillBuf] * BYTES_PER_SAMPLE;
      size_t written = wavFile.write(
        (uint8_t*)audioBufs[fillBuf], writeBytes
      );
      if (written == writeBytes) {
        segmentBytesWritten += writeBytes;
      }
      audioBufLens[fillBuf] = 0;
      bufReady[fillBuf]     = false;
    }
    xSemaphoreGive(wavMutex);
  }
}

// ── Open ACC file ─────────────────────────────────────────
void openAccFile(DateTime dt) {
  char fname[60];
  buildSessionFilename(dt, "ACC", NIGHT, "csv", fname, sizeof(fname));
  accFile = SD.open(fname, FILE_WRITE);
  if (!accFile) {
    Serial.print("FAIL — ACC: "); Serial.println(fname);
    teardownPeripherals();
    while (true);
  }
  accFile.println("ts_iso,ts_epoch_ms,accel_x_g,accel_y_g,accel_z_g");
  accFile.flush();
  Serial.print("ACC: "); Serial.println(fname);
}

// ── Open ENV file ─────────────────────────────────────────
void openEnvFile(DateTime dt) {
  char fname[60];
  buildSessionFilename(dt, "ENV", NIGHT, "csv", fname, sizeof(fname));
  envFile = SD.open(fname, FILE_WRITE);
  if (!envFile) {
    Serial.print("FAIL — ENV: "); Serial.println(fname);
    teardownPeripherals();
    while (true);
  }
  envFile.println("ts_iso,ts_epoch_ms,temp_C,humidity_pct,pressure_hPa,gas_resistance_ohm,iaq,iaq_accuracy");
  envFile.flush();
  Serial.print("ENV: "); Serial.println(fname);
}

// ── Open index CSV ────────────────────────────────────────
void openIndexFile(DateTime dt) {
  char fname[60];
  buildSessionFilename(dt, "MIC", NIGHT, "csv", fname, sizeof(fname));
  indexFile = SD.open(fname, FILE_WRITE);
  if (!indexFile) {
    Serial.print("FAIL — index: "); Serial.println(fname);
    teardownPeripherals();
    while (true);
  }
  indexFile.println("file_name,ts_iso_start,ts_epoch_ms_start,duration_s,sample_rate_hz");
  indexFile.flush();
  Serial.print("MIC index: "); Serial.println(fname);
}

// ── Open new WAV segment ──────────────────────────────────
void openNewWavSegment(DateTime dt) {
  if (wavFile) {
    flushStageBuf();
    patchWavHeader(wavFile, segmentBytesWritten);
    wavFile.flush();
    wavFile.close();
    Serial.printf("Segment h%02d closed — %.2f MB\n",
      hourIndex - 1, segmentBytesWritten / 1048576.0);
  }

  segmentBytesWritten = 0;
  hpf_prev_in         = 0.0f;
  hpf_prev_out        = 0.0f;

  char wavName[60];
  buildWavFilename(dt, NIGHT, hourIndex, wavName, sizeof(wavName));

  wavFile = SD.open(wavName, FILE_WRITE);
  if (!wavFile) {
    Serial.print("FAIL — WAV: "); Serial.println(wavName);
    teardownPeripherals();
    while (true);
  }
  writeWavHeader(wavFile, 0);

  char iso[30];
  buildISO(dt, 0, iso, sizeof(iso));
  uint64_t epochMs = toEpochMs(dt, 0);
  char row[160];
  snprintf(row, sizeof(row),
    "%s,%s,%llu,%lu,%d",
    wavName + 1, iso, epochMs,
    (unsigned long)SEGMENT_DURATION_S, SAMPLE_RATE
  );
  indexFile.println(row);
  indexFile.flush();

  Serial.printf("WAV h%02d opened: %s\n", hourIndex, wavName);
  hourIndex++;
}

// ── I2S init ─────────────────────────────────────────────
void initI2S() {
  i2s_config_t cfg = {
    .mode                 = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
    .sample_rate          = SAMPLE_RATE,
    .bits_per_sample      = I2S_BITS_PER_SAMPLE_32BIT,
    .channel_format       = I2S_CHANNEL_FMT_RIGHT_LEFT,
    .communication_format = I2S_COMM_FORMAT_STAND_I2S,
    .intr_alloc_flags     = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count        = DMA_BUF_COUNT,
    .dma_buf_len          = DMA_BUF_LEN,
    .use_apll             = false,
    .tx_desc_auto_clear   = false,
    .fixed_mclk           = 0
  };
  i2s_pin_config_t pins = {
    .bck_io_num   = PIN_BCLK,
    .ws_io_num    = PIN_WS,
    .data_out_num = I2S_PIN_NO_CHANGE,
    .data_in_num  = PIN_DOUT
  };
  i2s_driver_install(I2S_NUM_0, &cfg, 0, NULL);
  i2s_set_pin(I2S_NUM_0, &pins);
  i2s_zero_dma_buffer(I2S_NUM_0);
  i2sInstalled = true;
}

// ── Audio write task (Core 0) ─────────────────────────────
void audioTask(void* param) {
  for (;;) {
    for (int b = 0; b < NUM_BUFS; b++) {
      if (bufReady[b] && b != fillBuf) {
        if (xSemaphoreTake(wavMutex, portMAX_DELAY) == pdTRUE) {
          if (wavFile && audioBufLens[b] > 0) {
            uint32_t writeBytes = audioBufLens[b] * BYTES_PER_SAMPLE;
            size_t written = wavFile.write(
              (uint8_t*)audioBufs[b], writeBytes
            );
            if (written != writeBytes) {
              Serial.println("WARN — audio write incomplete.");
              wavFile.flush();
              wavFile.close();
              if (remountSD()) {
                char wavName[60];
                buildWavFilename(rtc.now(), NIGHT,
                                 hourIndex - 1,
                                 wavName, sizeof(wavName));
                wavFile = SD.open(wavName, FILE_APPEND);
                if (wavFile) {
                  Serial.println("WAV reopened — continuing.");
                } else {
                  Serial.println("FAIL — could not reopen WAV.");
                }
              }
            } else {
              segmentBytesWritten += writeBytes;
            }
          }
          audioBufLens[b] = 0;
          bufReady[b]     = false;
          xSemaphoreGive(wavMutex);
        }

        if (segmentBytesWritten >= BYTES_PER_SEGMENT) {
          if (xSemaphoreTake(wavMutex, portMAX_DELAY) == pdTRUE) {
            openNewWavSegment(rtc.now());
            xSemaphoreGive(wavMutex);
          }
        }
      }
    }
    vTaskDelay(1 / portTICK_PERIOD_MS);
  }
}

void setup() {
  Serial.begin(115200);
  delay(1000);

  // LED setup — solid on during init, blink during run,
  // off when done or failed
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, HIGH);  // solid on during setup

  Serial.println("╔══════════════════════════════════╗");
  Serial.printf( "║  Night %-4s  Pillow: %-14s║\n",
    NIGHT, PILLOW_MODEL);
  Serial.println("║  Pillow Prototype — All Streams  ║");
  Serial.println("╚══════════════════════════════════╝");
  Serial.println();

  Wire.begin(SDA_PIN, SCL_PIN);

  // ── RTC ─────────────────────────────────────────────────
  if (!rtc.begin()) {
    Serial.println("FAIL — DS3231 not found.");
    teardownPeripherals();
    while (true);
  }
  waitForTimeSync(rtc, 10000);
  DateTime now = rtc.now();
  sessionStartEpoch = now.unixtime();

  // ── ADXL345 ──────────────────────────────────────────────
  if (!accel.begin()) {
    Serial.println("FAIL — ADXL345 not found.");
    teardownPeripherals();
    while (true);
  }
  accel.setRange(ADXL345_RANGE_16_G);
  accel.setDataRate(ADXL345_DATARATE_100_HZ);
  Serial.println("ADXL345 OK");

  // ── BME688 ───────────────────────────────────────────────
  if (!bme.begin(0x77)) {
    if (!bme.begin(0x76)) {
      Serial.println("FAIL — BME688 not found.");
      teardownPeripherals();
      while (true);
    }
  }
  bme.setTemperatureOversampling(BME680_OS_8X);
  bme.setHumidityOversampling(BME680_OS_2X);
  bme.setPressureOversampling(BME680_OS_4X);
  bme.setIIRFilterSize(BME680_FILTER_SIZE_3);
  bme.setGasHeater(320, 150);
  Serial.println("BME688 OK");

  // ── microSD — init BEFORE I2S ─────────────────────────
  if (!mountSD()) {
    Serial.println("FAIL — microSD not mounted after 5 attempts.");
    teardownPeripherals();
    while (true);
  }

  // ── I2S — init AFTER SD ───────────────────────────────
  initI2S();
  Serial.println("I2S OK");

  // ── Open all files ───────────────────────────────────────
  now = rtc.now();
  openAccFile(now);
  openEnvFile(now);
  openIndexFile(now);
  openNewWavSegment(now);

  // ── Write metadata ───────────────────────────────────────
  writeMetaJSON(
    now, NIGHT, PILLOW_MODEL, ROOM_TEMP,
    CONDITIONS, PLACEMENT, ""
  );

  // ── Start audio write task on Core 0 ─────────────────────
  wavMutex = xSemaphoreCreateMutex();
  xTaskCreatePinnedToCore(
    audioTask,
    "audioTask",
    8192,
    NULL,
    1,
    &audioTaskHandle,
    0  // Core 0
  );
  Serial.println("Audio task running on Core 0.");

  runStartMs = millis();

  Serial.println();
  Serial.println("==============================");
  Serial.printf("All streams logging — %lu hr run\n",
    RUN_DURATION_MS / 3600000UL);
  Serial.println("==============================");

  // Setup complete — LED will now blink in loop
  // to show the session is running
}

void loop() {
  unsigned long now_ms = millis();

  // ── LED heartbeat — blink once per second ────────────────
  if (now_ms - lastBlink >= 1000) {
    lastBlink = now_ms;
    ledState  = !ledState;
    digitalWrite(LED_PIN, ledState);
  }

  // ── End of run ────────────────────────────────────────────
  if (now_ms - runStartMs >= RUN_DURATION_MS) {
    Serial.println();
    Serial.println("==============================");
    Serial.println("Run complete — closing files.");

    if (audioTaskHandle != NULL) {
      vTaskDelete(audioTaskHandle);
      audioTaskHandle = NULL;
    }

    flushStageBuf();

    accFile.flush(); accFile.close();
    envFile.flush(); envFile.close();
    patchWavHeader(wavFile, segmentBytesWritten);
    wavFile.flush(); wavFile.close();
    indexFile.flush(); indexFile.close();

    uint32_t elapsed = rtc.now().unixtime() - sessionStartEpoch;
    float wavMB = segmentBytesWritten / 1048576.0;

    Serial.println("==============================");
    Serial.printf("Duration:     %lu s (%lu hr %lu min)\n",
      elapsed, elapsed / 3600, (elapsed % 3600) / 60);
    Serial.printf("ACC rows:     %lu\n",  accRowCount);
    Serial.printf("ENV rows:     %lu\n",  envRowCount);
    Serial.printf("WAV segments: %d\n",   hourIndex);
    Serial.printf("Last WAV:     %.2f MB\n", wavMB);
    Serial.println("==============================");

    teardownPeripherals();  // LED turns off here
    while (true);
  }

  // ── ACC — 100 Hz ─────────────────────────────────────────
  static unsigned long lastAcc = 0;
  if (now_ms - lastAcc >= ACC_INTERVAL_MS) {
    lastAcc = now_ms;

    DateTime dt     = rtc.now();
    uint32_t ms_off = now_ms % 1000;

    sensors_event_t event;
    accel.getEvent(&event);
    float ax = event.acceleration.x / 9.81;
    float ay = event.acceleration.y / 9.81;
    float az = event.acceleration.z / 9.81;

    char iso[30];
    buildISO(dt, ms_off, iso, sizeof(iso));
    uint64_t epochMs = toEpochMs(dt, ms_off);

    char row[100];
    snprintf(row, sizeof(row),
      "%s,%llu,%.4f,%.4f,%.4f",
      iso, epochMs, ax, ay, az
    );
    accFile.println(row);
    accRowCount++;

    if (accRowCount % 500 == 0) accFile.flush();
  }

  // ── ENV — 1 Hz ───────────────────────────────────────────
  static unsigned long lastEnv = 0;
  if (now_ms - lastEnv >= 1000) {
    lastEnv = now_ms;

    if (bme.performReading()) {
      DateTime dt     = rtc.now();
      uint32_t ms_off = now_ms % 1000;

      char iso[30];
      buildISO(dt, ms_off, iso, sizeof(iso));
      uint64_t epochMs = toEpochMs(dt, ms_off);

      char row[140];
      snprintf(row, sizeof(row),
        "%s,%llu,%.2f,%.2f,%.2f,%.0f,,",
        iso, epochMs,
        bme.temperature,
        bme.humidity,
        bme.pressure / 100.0,
        bme.gas_resistance
      );
      envFile.println(row);
      envRowCount++;

      if (envRowCount % 60 == 0) envFile.flush();
    }
  }

  // ── MIC — fill buffer on Core 1 ──────────────────────────
  size_t bytesRead = 0;
  i2s_read(I2S_NUM_0, rawBuf, sizeof(rawBuf), &bytesRead, 0);

  if (bytesRead > 0) {
    int samplesRead = bytesRead / sizeof(int32_t);
    int monoSamples = samplesRead / 2;

    for (int i = 0; i < monoSamples; i++) {
      int32_t raw = (rawBuf[i * 2] >> 8);

      float in  = (float)raw;
      float out = HPF_ALPHA * (hpf_prev_out + in - hpf_prev_in);
      hpf_prev_in  = in;
      hpf_prev_out = out;

      int32_t sample = (int32_t)(out * GAIN_FACTOR);
      if (sample > 32767)  sample = 32767;
      if (sample < -32768) sample = -32768;
      if (abs(sample) < NOISE_GATE_THRESHOLD) sample = 0;

      if (audioBufLens[fillBuf] < STAGE_BUF_SAMPLES) {
        audioBufs[fillBuf][audioBufLens[fillBuf]++] = (int16_t)sample;
      }
    }

    if (audioBufLens[fillBuf] >= STAGE_BUF_SAMPLES) {
      bufReady[fillBuf] = true;
      fillBuf = (fillBuf + 1) % NUM_BUFS;
    }
  }

  // ── Status every 30 minutes ──────────────────────────────
  static unsigned long lastStatus = 0;
  if (now_ms - lastStatus >= 1800000UL) {
    lastStatus = now_ms;
    uint32_t elapsed   = (now_ms - runStartMs) / 1000;
    uint32_t remaining = (RUN_DURATION_MS - (now_ms - runStartMs)) / 1000;
    float wavMB = segmentBytesWritten / 1048576.0;

    Serial.println("--- Status ---");
    Serial.printf("Elapsed:   %lu s (%lu hr %lu min)\n",
      elapsed, elapsed / 3600, (elapsed % 3600) / 60);
    Serial.printf("Remaining: %lu s (%lu hr %lu min)\n",
      remaining, remaining / 3600, (remaining % 3600) / 60);
    Serial.printf("ACC rows:  %lu\n",   accRowCount);
    Serial.printf("ENV rows:  %lu\n",   envRowCount);
    Serial.printf("Free heap: %u\n",    ESP.getFreeHeap());
    Serial.printf("WAV seg:   h%02d — %.2f MB\n",
      hourIndex - 1, wavMB);
    Serial.println("--------------");
  }
}