#include <Arduino.h>
#include <Wire.h>
#include <SPI.h>
#include <SD.h>
#include <RTClib.h>
#include <driver/i2s.h>
#include "../../src/logging/session.h"

// ── Pin definitions ──────────────────────────────────────
#define SD_CS         4
#define SD_SCK        18
#define SD_MISO       19
#define SD_MOSI       23
#define SDA_PIN       21
#define SCL_PIN       22
#define PIN_BCLK      26
#define PIN_WS        25
#define I2S_DOUT      33

// ── Audio config ─────────────────────────────────────────
#define SAMPLE_RATE       16000
#define BITS_PER_SAMPLE   16
#define DMA_BUF_COUNT     16
#define DMA_BUF_LEN       1024
#define WRITE_BUF_SIZE    2048
#define GAIN_FACTOR       2

// ── High pass filter ─────────────────────────────────────
#define HPF_ALPHA             0.98f
float   hpf_prev_in         = 0.0f;
float   hpf_prev_out        = 0.0f;

// ── Noise gate ────────────────────────────────────────────
#define NOISE_GATE_THRESHOLD  200

// ── Staging buffer ───────────────────────────────────────
#define STAGE_BUF_SAMPLES  8192
int16_t stageBuf[STAGE_BUF_SAMPLES];
int     stageBufIdx = 0;

// ── Bench test duration ──────────────────────────────────
#define BENCH_DURATION_MS  (1UL * 60UL * 1000UL)  // 1 minute

// ── Session config ───────────────────────────────────────
#define SEGMENT_DURATION_S  3600
#define BYTES_PER_SAMPLE    2
#define BYTES_PER_SECOND    (SAMPLE_RATE * BYTES_PER_SAMPLE)
#define BYTES_PER_SEGMENT   ((uint32_t)BYTES_PER_SECOND * SEGMENT_DURATION_S)

const char* NIGHT = "n00";
const char* UNIT  = "01";

// ── I2S installed flag ────────────────────────────────────
bool i2sInstalled = false;

// ── Globals ──────────────────────────────────────────────
RTC_DS3231 rtc;
File wavFile;
File indexFile;

uint32_t segmentBytesWritten = 0;
uint8_t  hourIndex           = 0;
uint32_t sessionStartEpoch   = 0;

int32_t rawBuf[WRITE_BUF_SIZE];

// ── Helpers ──────────────────────────────────────────────
void buildISO(DateTime dt, char* buf, size_t len) {
  snprintf(buf, len,
    "%04d-%02d-%02dT%02d:%02d:%02d.000",
    dt.year(), dt.month(),  dt.day(),
    dt.hour(), dt.minute(), dt.second()
  );
}

uint64_t toEpochMs(DateTime dt) {
  return (uint64_t)dt.unixtime() * 1000ULL;
}

void buildWavFilename(DateTime dt, uint8_t h, char* buf, size_t len) {
  snprintf(buf, len,
    "/PILLOW_%s_%04d%02d%02d_MIC_%s_h%02d.wav",
    UNIT,
    dt.year(), dt.month(), dt.day(),
    NIGHT, h
  );
}

void buildIndexFilename(DateTime dt, char* buf, size_t len) {
  snprintf(buf, len,
    "/PILLOW_%s_%04d%02d%02d_MIC_%s_index.csv",
    UNIT,
    dt.year(), dt.month(), dt.day(),
    NIGHT
  );
}

// ── Peripheral teardown ───────────────────────────────────
void teardownPeripherals() {
  if (i2sInstalled) {
    i2s_driver_uninstall(I2S_NUM_0);
    i2sInstalled = false;
  }
  SD.end();
  SPI.end();

  // Return SPI pins to known idle state for next sketch
  pinMode(SD_CS,   OUTPUT); digitalWrite(SD_CS,   HIGH);
  pinMode(SD_SCK,  OUTPUT); digitalWrite(SD_SCK,  HIGH);
  pinMode(SD_MOSI, OUTPUT); digitalWrite(SD_MOSI, HIGH);
  pinMode(SD_MISO, INPUT);

  Serial.println("Peripherals torn down — safe to power down or reflash.");
}

// ── SD mount with retry ───────────────────────────────────
bool mountSD() {
  // Force all SPI pins to known idle state
  // Clears residual state from previous sketches or boot process
  pinMode(SD_CS,   OUTPUT); digitalWrite(SD_CS,   HIGH);
  pinMode(SD_SCK,  OUTPUT); digitalWrite(SD_SCK,  HIGH);
  pinMode(SD_MOSI, OUTPUT); digitalWrite(SD_MOSI, HIGH);
  pinMode(SD_MISO, INPUT);
  delay(1000);

  SPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);

  for (int attempt = 1; attempt <= 5; attempt++) {
    Serial.printf("SD mount attempt %d/5...\n", attempt);
    if (SD.begin(SD_CS, SPI, 4000000)) {
      Serial.println("microSD OK");
      return true;
    }
    Serial.println("Mount failed — retrying in 2 seconds.");
    SD.end();
    delay(2000);
    SPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
  }
  return false;
}

// ── SD remount after error ────────────────────────────────
bool remountSD() {
  Serial.println("WARN — SD error, attempting remount...");
  SD.end();
  delay(1000);
  SPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
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

// ── Index CSV ────────────────────────────────────────────
void writeIndexRow(File& f, const char* wavName,
                   const char* tsIso, uint64_t epochMs,
                   uint32_t durationS) {
  char row[160];
  snprintf(row, sizeof(row),
    "%s,%s,%llu,%lu,%d",
    wavName, tsIso, epochMs,
    (unsigned long)durationS, SAMPLE_RATE
  );
  f.println(row);
  f.flush();
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
    .data_in_num  = I2S_DOUT
  };
  i2s_driver_install(I2S_NUM_0, &cfg, 0, NULL);
  i2s_set_pin(I2S_NUM_0, &pins);
  i2s_zero_dma_buffer(I2S_NUM_0);
  i2sInstalled = true;
}

// ── Open new WAV segment ──────────────────────────────────
void openNewSegment(DateTime dt) {
  if (wavFile) {
    if (stageBufIdx > 0) {
      wavFile.write((uint8_t*)stageBuf,
                    stageBufIdx * BYTES_PER_SAMPLE);
      segmentBytesWritten += stageBufIdx * BYTES_PER_SAMPLE;
      stageBufIdx = 0;
    }
    patchWavHeader(wavFile, segmentBytesWritten);
    wavFile.flush();
    wavFile.close();
    Serial.print("Segment closed. Bytes: ");
    Serial.println(segmentBytesWritten);
  }

  segmentBytesWritten = 0;
  stageBufIdx         = 0;
  hpf_prev_in         = 0.0f;
  hpf_prev_out        = 0.0f;

  char wavName[60];
  buildWavFilename(dt, hourIndex, wavName, sizeof(wavName));

  wavFile = SD.open(wavName, FILE_WRITE);
  if (!wavFile) {
    Serial.print("FAIL — could not open WAV: ");
    Serial.println(wavName);
    teardownPeripherals();
    while (true);
  }

  writeWavHeader(wavFile, 0);

  char iso[30];
  buildISO(dt, iso, sizeof(iso));
  uint64_t epochMs    = toEpochMs(dt);
  const char* wavBase = wavName + 1;
  writeIndexRow(indexFile, wavBase, iso, epochMs, SEGMENT_DURATION_S);

  Serial.print("Opened segment: "); Serial.println(wavName);
  hourIndex++;
}

void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial.println("=============================");
  Serial.println("INMP441 MIC LOGGER");
  Serial.printf("Sample rate: %d Hz\n", SAMPLE_RATE);
  Serial.printf("Bench test: %lu min\n", BENCH_DURATION_MS / 60000UL);
  Serial.println("=============================");

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

  // ── microSD — init BEFORE I2S, with pin reset ────────────
  if (!mountSD()) {
    Serial.println("FAIL — microSD not mounted after 5 attempts.");
    teardownPeripherals();
    while (true);
  }

  // ── Index CSV ────────────────────────────────────────────
  char indexName[60];
  buildIndexFilename(now, indexName, sizeof(indexName));
  indexFile = SD.open(indexName, FILE_WRITE);
  if (!indexFile) {
    Serial.println("FAIL — could not open index CSV.");
    teardownPeripherals();
    while (true);
  }
  indexFile.println("file_name,ts_iso_start,ts_epoch_ms_start,duration_s,sample_rate_hz");
  indexFile.flush();
  Serial.print("Index file: "); Serial.println(indexName);

  // ── I2S — init AFTER SD ───────────────────────────────
  initI2S();
  Serial.println("I2S OK");

  // ── Open first segment ───────────────────────────────────
  openNewSegment(now);
  Serial.println("Logging started — speak or tap near mic.");
  Serial.println("=============================");
}

void loop() {
  // ── Bench test stop ───────────────────────────────────────
  if (millis() > BENCH_DURATION_MS) {
    if (stageBufIdx > 0 && wavFile) {
      wavFile.write((uint8_t*)stageBuf,
                    stageBufIdx * BYTES_PER_SAMPLE);
      segmentBytesWritten += stageBufIdx * BYTES_PER_SAMPLE;
      stageBufIdx = 0;
    }
    patchWavHeader(wavFile, segmentBytesWritten);
    wavFile.flush();
    wavFile.close();
    indexFile.close();

    uint32_t elapsed = rtc.now().unixtime() - sessionStartEpoch;
    float mb = segmentBytesWritten / 1048576.0;
    Serial.println();
    Serial.println("=============================");
    Serial.println("Bench test complete.");
    Serial.printf("Duration:  %lu s\n", elapsed);
    Serial.printf("WAV size:  %.2f MB\n", mb);
    Serial.println("=============================");

    teardownPeripherals();
    while (true);
  }

  // ── Read I2S ─────────────────────────────────────────────
  size_t bytesRead = 0;
  i2s_read(I2S_NUM_0, rawBuf, sizeof(rawBuf),
           &bytesRead, portMAX_DELAY);

  int samplesRead = bytesRead / sizeof(int32_t);
  int monoSamples = samplesRead / 2;

  // ── Extract left channel + HPF + gain + noise gate ────────
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

    if (stageBufIdx < STAGE_BUF_SAMPLES) {
      stageBuf[stageBufIdx++] = (int16_t)sample;
    }
  }

  // ── Write to SD when staging buffer full ─────────────────
  if (stageBufIdx >= STAGE_BUF_SAMPLES) {
    if (wavFile) {
      uint32_t writeBytes = stageBufIdx * BYTES_PER_SAMPLE;
      size_t written = wavFile.write((uint8_t*)stageBuf, writeBytes);
      delay(1);

      if (written != writeBytes) {
        Serial.println("WARN — write incomplete, attempting recovery...");
        wavFile.flush();
        wavFile.close();
        if (remountSD()) {
          char wavName[60];
          buildWavFilename(rtc.now(), hourIndex - 1,
                           wavName, sizeof(wavName));
          wavFile = SD.open(wavName, FILE_APPEND);
          if (!wavFile) {
            Serial.println("FAIL — could not reopen WAV after recovery.");
          } else {
            Serial.println("WAV file reopened — continuing.");
          }
        }
      } else {
        segmentBytesWritten += writeBytes;
      }
    }
    stageBufIdx = 0;
  }

  // ── Hourly rollover ──────────────────────────────────────
  if (segmentBytesWritten >= BYTES_PER_SEGMENT) {
    openNewSegment(rtc.now());
  }

  // ── Status every 15 seconds ──────────────────────────────
  static unsigned long lastReport = 0;
  if (millis() - lastReport >= 15000) {
    lastReport = millis();
    uint32_t elapsed = rtc.now().unixtime() - sessionStartEpoch;
    float mb = segmentBytesWritten / 1048576.0;
    Serial.printf("Elapsed: %lu s | WAV: %.2f MB | Hour: h%02d\n",
      elapsed, mb, hourIndex - 1);
  }
}